/*
 * Description: segment test
 */

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <iostream>
#include <cstring>
#include "gtest/gtest.h"
#include "linux/compiler_attributes.h"
#include "linux/device.h"
#include "linux/mm_types.h"
#include "linux/types.h"
#include "linux/dmaengine.h"
#include "linux/migrate.h"
#include "ubcore_types.h"
#include "mockcpp/ChainingMockHelper.h"
#include "mockcpp/mokc.h"
#include "stub_struct.h"
#include "urma.h"
#include "ubcore_opcode.h"

using namespace std;

class TestSegment : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }
};

extern "C" {
    int get_urma_trans_segment(struct urma_trans_segment_info *src_info, struct urma_trans_segment_info *dst_info);
    void ub_dma_free(struct ub_dma_dev *udev);
    int check_mem_info(u64 pa_start, u64 pa_end);
    int pfn_valid(unsigned long pfn);
    int pfn_to_nid(unsigned long pfn);
    struct page *pfn_to_online_page(unsigned long pfn);
    int ub_dma_register_segment(u64 pa_start, u64 pa_end);
    void unregister_urma_segment(struct urma_sge_info *sge_info);
    int ub_dma_unregister_segment(u64 start_pa, u64 end_pa);
    int urma_meta_sge_init(void);
    int get_single_urma_trans_segment(struct urma_trans_segment_info *info, bool is_i_seg);
    void ub_dma_unregister_all_segment(void);
    int urma_register_segment(uint64_t start_va, uint64_t end_va, struct urma_sge_info *sge_info);
}

TEST_F(TestSegment, check_mem_info_test)
{
    u64 pa_start = 0;
    u64 pa_end = 0;

    pa_start = 2; // 2
    pa_end = 0;
    int ret1 = check_mem_info(pa_start, pa_end);
    EXPECT_EQ(ret1, -EINVAL);
    pa_start = 0;
    pa_end = 3; // 3
    MOCKER(pfn_valid).stubs().will(returnValue(0));
    ret1 = check_mem_info(pa_start, pa_end);
    EXPECT_EQ(ret1, -EINVAL);
    GlobalMockObject::verify();
    MOCKER(pfn_to_nid).stubs().will(returnValue(1)).then(returnValue(0));
    ret1 = check_mem_info(pa_start, pa_end);
    EXPECT_EQ(ret1, -EINVAL);
    GlobalMockObject::verify();
    ret1 = check_mem_info(pa_start, pa_end);
    EXPECT_EQ(ret1, 0);
}

TEST_F(TestSegment, ub_dma_register_segment_test)
{
    u64 pa_start = 0;
    u64 pa_end = 3; // 3
    MOCKER(kzalloc).stubs().will(returnValue((void*)nullptr));
    MOCKER(check_mem_info).stubs().will(returnValue(0));
    int ret1 = ub_dma_register_segment(pa_start, pa_end);
    EXPECT_EQ(ret1, -ENOMEM);

    GlobalMockObject::verify();
    MOCKER(check_mem_info).stubs().will(returnValue(0));
    MOCKER(urma_register_segment).stubs().will(returnValue(1));
    ret1 = ub_dma_register_segment(pa_start, pa_end);
    EXPECT_EQ(ret1, 1);

    GlobalMockObject::verify();
    MOCKER(check_mem_info).stubs().will(returnValue(0));
    MOCKER(urma_register_segment).stubs().will(returnValue(0));
    urma_meta_sge_init();
    ret1 = ub_dma_register_segment(pa_start, pa_end);
    EXPECT_EQ(ret1, 0);

    GlobalMockObject::verify();
    MOCKER(unregister_urma_segment).stubs();
    ub_dma_unregister_segment(0, 3);
}

TEST_F(TestSegment, get_urma_trans_segment_test)
{
    struct urma_trans_segment_info *src_info;
    struct urma_trans_segment_info *dst_info;
    int ret = get_urma_trans_segment(src_info, dst_info);
    EXPECT_EQ(0, ret);
}

TEST_F(TestSegment, get_single_urma_trans_segment_test)
{
    urma_meta_sge_init();
    struct urma_trans_segment_info info;
    info.addr = 0x1000;
    info.len = 0x100;
    info.sge = nullptr;
    int ret = get_single_urma_trans_segment(&info, true);
    EXPECT_EQ(ret, -ENOENT);

    GlobalMockObject::verify();
    ret = get_single_urma_trans_segment(&info, false);
    EXPECT_EQ(ret, -ENOENT);
}

TEST_F(TestSegment, ub_dma_unregister_segment_not_found_test)
{
    urma_meta_sge_init();
    int ret = ub_dma_unregister_segment(0, 3);
    EXPECT_EQ(ret, -ENOENT);
}

extern "C" {
    void *test_seg_kzalloc(size_t size, gfp_t flags);
    void test_seg_kfree(void *p);
}

void *test_seg_kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void test_seg_kfree(void *p)
{
    free(p);
}

TEST_F(TestSegment, ub_dma_unregister_segment_hash_collision_test)
{
    u64 pa_a = 0;
    u64 pa_b = 0x2; // 0x2, same hash key with pa_a (same va >> 27 bucket)
    int ret;

    // use real malloc-backed allocator so the hash table works
    MOCKER(kzalloc).stubs().will(invoke(test_seg_kzalloc));
    MOCKER(kfree).stubs().will(invoke(test_seg_kfree));
    MOCKER(check_mem_info).stubs().will(returnValue(0));
    MOCKER(urma_register_segment).stubs().will(returnValue(0));
    urma_meta_sge_init();
    ASSERT_EQ(ub_dma_register_segment(pa_a, pa_a + 0x3), 0);
    ASSERT_EQ(ub_dma_register_segment(pa_b, pa_b + 0x3), 0);

    // unregister A in a hash bucket containing A and B, only A is removed, and B is still usable
    MOCKER(unregister_urma_segment).stubs();
    // mismatched range should not delete any node in the bucket (including A)
    ret = ub_dma_unregister_segment(pa_a + 0x1, pa_a + 0x3);
    EXPECT_EQ(ret, -ENOENT);

    ret = ub_dma_unregister_segment(pa_a, pa_a + 0x3);
    EXPECT_EQ(ret, 0);

    // A has been removed; the node found here is still B (same bucket), but start_va mismatches
    ret = ub_dma_unregister_segment(pa_a, pa_a + 0x3);
    EXPECT_EQ(ret, -ENOENT);

    ret = ub_dma_unregister_segment(pa_b, pa_b + 0x3);
    EXPECT_EQ(ret, 0);
}

TEST_F(TestSegment, ub_dma_unregister_all_segment_test)
{
    urma_meta_sge_init();
    MOCKER(unregister_urma_segment).stubs();
    ub_dma_unregister_all_segment();
}
