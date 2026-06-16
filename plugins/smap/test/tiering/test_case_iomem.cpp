/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP 测试代码
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/list.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/fs.h>

#include "iomem.h"
#include "smap_msg.h"

extern list_head remote_ram_list;
class IomemTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::cout << "[Phase SetUp Begin]" << std::endl;
        INIT_LIST_HEAD(&remote_ram_list);
        std::cout << "[Phase SetUp End]" << std::endl;
    }
    void TearDown() override
    {
        std::cout << "[Phase TearDown Begin]" << std::endl;
        GlobalMockObject::reset();
        std::cout << "[Phase TearDown End]" << std::endl;
    }
};

struct obmm_dev_info {
    struct list_head list;
    struct mutex lock;
};

struct read_obmm_callback {
    struct dir_context ctx;
    int ret;
};

extern "C" struct obmm_dev_info obmm_dev;
extern "C" void free_remote_ram(struct list_head *head);
TEST_F(IomemTest, free_remote_ram_dt)
{
    ram_segment *ramdata1 = new ram_segment;
    ram_segment *ramdata2 = new ram_segment;
    list_head test_list;
    INIT_LIST_HEAD(&ramdata1->node);
    list_add(&ramdata2->node, &ramdata1->node);
    free_remote_ram(&ramdata1->node);
    if (ramdata1 != nullptr) {
        delete(ramdata1);
    }
}
extern "C" void copy_remote_ram(struct list_head *dst, struct list_head *src);
TEST_F(IomemTest, copy_remote_ram_dt)
{
    ram_segment ramdata1 = {0};
    ram_segment ramdata2 = {0};
    list_head test_list;
    INIT_LIST_HEAD(&test_list);
    list_add(&ramdata1.node, &test_list);
    list_add(&ramdata2.node, &test_list);
    copy_remote_ram(&ramdata1.node, &ramdata2.node);
}

extern "C" bool pfn_valid(unsigned long pfn);
extern "C" int page_to_nid(const struct page *page);
extern "C" struct page *pfn_to_online_page(unsigned long pfn);
extern "C" int insert_remote_ram(u64 pa_start, u64 pa_end, struct list_head *head);
TEST_F(IomemTest, insert_remote_rams_dt)
{
    int ret = 0;
    u64 pa_start = 0;
    u64 pa_end = 3;
    LIST_HEAD(test_list);
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(ret, 0);
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(ret, 0);
}
TEST_F(IomemTest, insert_remote_rams_dt_One)
{
    int ret = 0;
    u64 pa_start = 0;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);

    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(NUMA_NO_NODE));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(ret, -EINVAL);
}
TEST_F(IomemTest, insert_remote_rams_dt_Two)
{
    int ret = 0;
    u64 pa_start = 0;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);

    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    MOCKER(kmalloc).stubs().will(returnValue((void*)nullptr));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(ret, -ENOMEM);
}

TEST_F(IomemTest, insert_remote_rams_dt_Three)
{
    int ret = 0;
    u64 pa_start = 0;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
 
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);

    struct ram_segment *seg;
    struct ram_segment *tmp;
    list_for_each_entry_safe(seg, tmp, &test_list, node)
    {
        list_del(&seg->node);
        kfree(seg);
    }
}

TEST_F(IomemTest, insert_remote_rams_dt_Four)
{
    int ret = 0;
    u64 pa_start = 2;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 0,
        .end =1,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

TEST_F(IomemTest, insert_remote_rams_dt_Five)
{
    int ret = 0;
    u64 pa_start = 1;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 0,
        .end =0,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(20));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

TEST_F(IomemTest, insert_remote_rams_dt_Six)
{
    int ret = 0;
    u64 pa_start = 1;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 4,
        .end =4,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

TEST_F(IomemTest, insert_remote_rams_dt_Seven)
{
    int ret = 0;
    u64 pa_start = 1;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 2,
        .end =4,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

TEST_F(IomemTest, insert_remote_rams_dt_Eight)
{
    int ret = 0;
    u64 pa_start = 1;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 4,
        .end =2,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

TEST_F(IomemTest, insert_remote_rams_dt_Nine)
{
    int ret = 0;
    u64 pa_start = 1;
    u64 pa_end = 3;
    struct page page;
    LIST_HEAD(test_list);
    struct ram_segment test_node = {
        .numa_node = 1,
        .start = 1,
        .end =3,
    };
    list_add_tail(&test_node.node, &test_list);
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue(&page));
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    ret = insert_remote_ram(pa_start, pa_end, &test_list);
    EXPECT_EQ(0, ret);
 
    list_del(&test_node.node);
}

extern "C" int fixed_remote_ram(struct list_head *head);
TEST_F(IomemTest, fixed_remote_ram_dt)
{
    struct ram_segment *seg = (struct ram_segment*)kmalloc(sizeof(*seg), GFP_KERNEL);
    LIST_HEAD(test_list);
    int ret = fixed_remote_ram(&test_list);
    EXPECT_EQ(ret, 0);
}
TEST_F(IomemTest, fixed_remote_ram_dt_One)
{
    LIST_HEAD(test_list);
    MOCKER(kmalloc).stubs().will(returnValue((void*)nullptr));
    int ret = fixed_remote_ram(&test_list);
    EXPECT_EQ(ret, -ENOMEM);
}
extern "C" int walk_system_ram_remote_range(struct list_head *head);
extern "C" int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start,
    u64 end, void *arg, int (*func)(struct resource *, void *));
TEST_F(IomemTest, walk_system_ram_remote_range_dt)
{
    int ret = walk_system_ram_remote_range(nullptr);
    EXPECT_EQ(-EINVAL, ret);

    LIST_HEAD(head);
    MOCKER(walk_iomem_res_desc).stubs().will(returnValue(0));
    ret = walk_system_ram_remote_range(&head);
    EXPECT_EQ(0, ret);
}

extern "C" void free_obmm_dev(void);
extern "C" void update_obmm_dev(u64 memid);
TEST_F(IomemTest, update_obmm_dev_dt)
{
    constexpr int OBMM_DEV1_MEMID = 1;
    constexpr int OBMM_DEV2_MEMID = 2;

    ASSERT_TRUE(list_empty(&obmm_dev.list));
    update_obmm_dev(0);
    EXPECT_TRUE(list_empty(&obmm_dev.list));

    struct memid_range *tmp = (struct memid_range *)kzalloc(sizeof(*tmp), GFP_KERNEL);
    ASSERT_NE(nullptr, tmp);

    list_add(&tmp->node, &obmm_dev.list);

    update_obmm_dev(OBMM_DEV1_MEMID);
    tmp = container_of(obmm_dev.list.prev, struct memid_range, node);
    EXPECT_EQ(OBMM_DEV1_MEMID, tmp->memid);
    EXPECT_EQ(0, tmp->start);
    EXPECT_EQ(0, tmp->end);

    update_obmm_dev(OBMM_DEV2_MEMID);
    tmp = container_of(obmm_dev.list.prev, struct memid_range, node);
    EXPECT_EQ(OBMM_DEV2_MEMID, tmp->memid);
    EXPECT_EQ(0, tmp->start);
    EXPECT_EQ(0, tmp->end);

    free_obmm_dev();
}

extern "C" int kern_path(const char *name, unsigned int flags, struct path *path);
extern "C" bool is_import_shmdev(const char *name);
TEST_F(IomemTest, is_import_shmdev_dt)
{
    bool ret = is_import_shmdev("obmm_shmde");
    EXPECT_EQ(false, ret);

    ret = is_import_shmdev("obmm_shmdev1");
    EXPECT_EQ(true, ret);
}

extern "C" struct file *filp_open(const char *filename, int flags, umode_t mode);
extern "C" int filp_close(struct file *filp, fl_owner_t id);
extern "C" bool IS_ERR(const void *ptr);
extern "C" int extract_hex_content(const char *file_path, u64 *content);
TEST_F(IomemTest, extract_hex_content_dt)
{
    struct file filp;
    char buf[6] = "0x100";
    u64 content;

    MOCKER(filp_open).stubs().will(returnValue((struct file *)&filp));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(kernel_read).stubs().with(any(), outBoundP((void *)buf, sizeof(buf)), any(), any()).will(returnValue(5));
    MOCKER(filp_close).stubs().will(ignoreReturnValue());
    int ret = extract_hex_content("size", &content);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0x100, content);
}

extern "C" bool fill_obmmdev(struct dir_context *ctx, const char *name, int namelen,
    loff_t offset, u64 ino, unsigned int d_type);
TEST_F(IomemTest, fill_obmmdev_dt)
{
    struct read_obmm_callback call;
    call.ret = 1;

    bool ret = fill_obmmdev(&call.ctx, "obmm", 4, 0, 0, 0);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0, call.ret);
}

extern "C" void release_remote_ram(void);
TEST_F(IomemTest, release_remote_ram_dt)
{
    release_remote_ram();
}

extern "C" int refresh_remote_ram(void);
TEST_F(IomemTest, refresh_remote_ram_dt)
{
    smap_scene = NORMAL_SCENE;
    MOCKER(walk_system_ram_remote_range).stubs().will(returnValue(1)).then(returnValue(0));
    int ret = refresh_remote_ram();
    EXPECT_EQ(ret, 1);
    ret = refresh_remote_ram();
    EXPECT_EQ(ret, 0);
}
TEST_F(IomemTest, refresh_remote_ram_dt_One)
{
    smap_scene = UB_QEMU_SCENE;
    MOCKER(fixed_remote_ram).stubs().will(returnValue(0));
    int ret = refresh_remote_ram();
    EXPECT_EQ(ret, 0);
    smap_scene = NORMAL_SCENE;
}
extern "C" bool is_smap_pg_huge(void);
extern "C" int calc_acidx_paddr_iomem(u64 index, int nid, u64 *paddr);
TEST_F(IomemTest, calc_acidx_paddr_iomem_dt)
{
    u64 paddr[10] = {0};
    u64 addr;
    u64 index = 1;
    int nid = 1;
    ram_segment newdata1 = {0};
    ram_segment newdata2 = {0};

    list_add_tail(&newdata1.node, &remote_ram_list);
    list_add_tail(&newdata2.node, &remote_ram_list);
    int ret = calc_acidx_paddr_iomem(index, nid, paddr);
    EXPECT_EQ(ret, -34);
    newdata1.end = 1 << 22;
    nid = 0;
    ret = calc_acidx_paddr_iomem(index, nid, paddr);
    EXPECT_EQ(ret, 0);
    MOCKER(is_smap_pg_huge).stubs().will(returnValue(false));
    ret = calc_acidx_paddr_iomem(index, nid, paddr);
    EXPECT_EQ(ret, 0);

    GlobalMockObject::verify();

    nid = 0;
    index = 0x00000400;
    newdata1.start = 0x0;
    newdata1.end =   0x7fffffff;
    newdata1.numa_node = 0;

    newdata2.start = 0x2080000000;
    newdata2.end = 0x20807fffff;
    newdata2.numa_node = 0;
    
    MOCKER(is_smap_pg_huge).stubs().will(returnValue(true));
    ret = calc_acidx_paddr_iomem(index, nid, &addr);
    ASSERT_EQ(0, ret);
    EXPECT_EQ(0x2080000000, addr);

    list_del(&newdata1.node);
    list_del(&newdata2.node);
}

extern "C" int find_range_by_memid(u64 memid, u64 *start, u64 *end);
TEST_F(IomemTest, find_range_by_memid_dt)
{
    u64 start;
    u64 end;
    struct memid_range mr;
    mr.memid = 1;
    mr.start = 0x0;
    mr.end = 0xFF;

    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));
    list_add(&mr.node, &obmm_dev.list);
    int ret = find_range_by_memid(1, &start, &end);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0xFF, end);

    ret = find_range_by_memid(2, &start, &end);
    EXPECT_EQ(-ENOENT, ret);
    list_del(&mr.node);
}

// ========== DT supplement: merge_ram_segments ==========

extern "C" void merge_ram_segments(struct list_head *head);

TEST_F(IomemTest, MergeRamSegmentsEmptyList)
{
    LIST_HEAD(empty_list);
    merge_ram_segments(&empty_list);
    EXPECT_TRUE(list_empty(&empty_list));
}

TEST_F(IomemTest, MergeRamSegmentsSingleEntry)
{
    struct ram_segment *seg = (struct ram_segment *)kmalloc(sizeof(*seg), GFP_KERNEL);
    ASSERT_NE(nullptr, seg);
    seg->numa_node = 0;
    seg->start = 0x1000;
    seg->end = 0x2000;
    LIST_HEAD(test_list);
    list_add_tail(&seg->node, &test_list);

    merge_ram_segments(&test_list);

    EXPECT_EQ(0x1000, seg->start);
    EXPECT_EQ(0x2000, seg->end);

    list_del(&seg->node);
    kfree(seg);
}

TEST_F(IomemTest, MergeRamSegmentsAdjacentSameNid)
{
    struct ram_segment *seg1 = (struct ram_segment *)kmalloc(sizeof(*seg1), GFP_KERNEL);
    struct ram_segment *seg2 = (struct ram_segment *)kmalloc(sizeof(*seg2), GFP_KERNEL);
    ASSERT_NE(nullptr, seg1);
    ASSERT_NE(nullptr, seg2);

    seg1->numa_node = 0;
    seg1->start = 0x1000;
    seg1->end = 0x1FFF;

    seg2->numa_node = 0;
    seg2->start = 0x2000;
    seg2->end = 0x3FFF;

    LIST_HEAD(test_list);
    list_add_tail(&seg1->node, &test_list);
    list_add_tail(&seg2->node, &test_list);

    merge_ram_segments(&test_list);

    EXPECT_EQ(0x1000, seg1->start);
    EXPECT_EQ(0x3FFF, seg1->end);

    kfree(seg1);
}

TEST_F(IomemTest, MergeRamSegmentsGapBetweenSegments)
{
    struct ram_segment *seg1 = (struct ram_segment *)kmalloc(sizeof(*seg1), GFP_KERNEL);
    struct ram_segment *seg2 = (struct ram_segment *)kmalloc(sizeof(*seg2), GFP_KERNEL);
    ASSERT_NE(nullptr, seg1);
    ASSERT_NE(nullptr, seg2);

    seg1->numa_node = 0;
    seg1->start = 0x1000;
    seg1->end = 0x1FFF;

    seg2->numa_node = 0;
    seg2->start = 0x3000;
    seg2->end = 0x3FFF;

    LIST_HEAD(test_list);
    list_add_tail(&seg1->node, &test_list);
    list_add_tail(&seg2->node, &test_list);

    merge_ram_segments(&test_list);

    EXPECT_EQ(0x1000, seg1->start);
    EXPECT_EQ(0x1FFF, seg1->end);
    EXPECT_EQ(0x3000, seg2->start);
    EXPECT_EQ(0x3FFF, seg2->end);

    list_del(&seg1->node);
    list_del(&seg2->node);
    kfree(seg1);
    kfree(seg2);
}

TEST_F(IomemTest, MergeRamSegmentsDifferentNid)
{
    struct ram_segment *seg1 = (struct ram_segment *)kmalloc(sizeof(*seg1), GFP_KERNEL);
    struct ram_segment *seg2 = (struct ram_segment *)kmalloc(sizeof(*seg2), GFP_KERNEL);
    ASSERT_NE(nullptr, seg1);
    ASSERT_NE(nullptr, seg2);

    seg1->numa_node = 0;
    seg1->start = 0x1000;
    seg1->end = 0x1FFF;

    seg2->numa_node = 1;
    seg2->start = 0x2000;
    seg2->end = 0x3FFF;

    LIST_HEAD(test_list);
    list_add_tail(&seg1->node, &test_list);
    list_add_tail(&seg2->node, &test_list);

    merge_ram_segments(&test_list);

    EXPECT_EQ(0, seg1->numa_node);
    EXPECT_EQ(1, seg2->numa_node);

    list_del(&seg1->node);
    list_del(&seg2->node);
    kfree(seg1);
    kfree(seg2);
}

TEST_F(IomemTest, MergeRamSegmentsMultipleMerges)
{
    struct ram_segment *seg1 = (struct ram_segment *)kmalloc(sizeof(*seg1), GFP_KERNEL);
    struct ram_segment *seg2 = (struct ram_segment *)kmalloc(sizeof(*seg2), GFP_KERNEL);
    struct ram_segment *seg3 = (struct ram_segment *)kmalloc(sizeof(*seg3), GFP_KERNEL);
    ASSERT_NE(nullptr, seg1);
    ASSERT_NE(nullptr, seg2);
    ASSERT_NE(nullptr, seg3);

    seg1->numa_node = 0;
    seg1->start = 0x1000;
    seg1->end = 0x1FFF;

    seg2->numa_node = 0;
    seg2->start = 0x2000;
    seg2->end = 0x2FFF;

    seg3->numa_node = 0;
    seg3->start = 0x3000;
    seg3->end = 0x3FFF;

    LIST_HEAD(test_list);
    list_add_tail(&seg1->node, &test_list);
    list_add_tail(&seg2->node, &test_list);
    list_add_tail(&seg3->node, &test_list);

    merge_ram_segments(&test_list);

    EXPECT_EQ(0x1000, seg1->start);
    EXPECT_EQ(0x3FFF, seg1->end);

    kfree(seg1);
}

// ========== DT supplement: update_resource ==========

extern "C" int update_resource(struct resource *r, void *arg);

TEST_F(IomemTest, UpdateResourceNullResource)
{
    LIST_HEAD(head);
    int ret = update_resource(nullptr, &head);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(IomemTest, UpdateResourceNullArg)
{
    struct resource res = {};
    int ret = update_resource(&res, nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(IomemTest, UpdateResourceSysramDriverManagedSuccess)
{
    struct resource res;
    res.flags = IORESOURCE_SYSRAM_DRIVER_MANAGED;
    res.start = 0x100000;
    res.end = 0x200000;
    LIST_HEAD(head);

    MOCKER(insert_remote_ram).stubs().will(returnValue(0));
    int ret = update_resource(&res, &head);
    EXPECT_EQ(0, ret);
}

TEST_F(IomemTest, UpdateResourceSysramInsertFail)
{
    struct resource res;
    res.flags = IORESOURCE_SYSRAM_DRIVER_MANAGED;
    res.start = 0x100000;
    res.end = 0x200000;
    LIST_HEAD(head);

    MOCKER(insert_remote_ram).stubs().will(returnValue(-EINVAL));
    MOCKER(free_remote_ram).stubs().will(ignoreReturnValue());
    int ret = update_resource(&res, &head);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(IomemTest, UpdateResourceNonSysram)
{
    struct resource res;
    res.flags = IORESOURCE_MEM;
    res.start = 0x100000;
    res.end = 0x200000;
    LIST_HEAD(head);

    int ret = update_resource(&res, &head);
    EXPECT_EQ(0, ret);
}

// ========== DT supplement: init_obmm_dev ==========

extern "C" int init_obmm_dev(void);

TEST_F(IomemTest, InitObmmDevSuccess)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));

    int ret = init_obmm_dev();
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(list_empty(&obmm_dev.list));

    free_obmm_dev();
}

TEST_F(IomemTest, InitObmmDevKzallocFail)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));

    MOCKER(kzalloc).stubs().will(returnValue((void*)nullptr));
    int ret = init_obmm_dev();
    EXPECT_EQ(-ENOMEM, ret);
}

// ========== DT supplement: clean_obmm_dev ==========

extern "C" void clean_obmm_dev(void);

TEST_F(IomemTest, CleanObmmDevEmptyList)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));
    clean_obmm_dev();
    EXPECT_TRUE(list_empty(&obmm_dev.list));
}

TEST_F(IomemTest, CleanObmmDevRemoveStale)
{
    free_obmm_dev();

    struct memid_range *mr1 = (struct memid_range *)kzalloc(sizeof(*mr1), GFP_KERNEL);
    ASSERT_NE(nullptr, mr1);
    mr1->memid = 0;
    mr1->seq = 2;
    list_add(&mr1->node, &obmm_dev.list);

    struct memid_range *mr2 = (struct memid_range *)kzalloc(sizeof(*mr2), GFP_KERNEL);
    ASSERT_NE(nullptr, mr2);
    mr2->memid = 5;
    mr2->seq = 1;
    list_add_tail(&mr2->node, &obmm_dev.list);

    clean_obmm_dev();

    struct memid_range *remaining = list_first_entry(&obmm_dev.list, struct memid_range, node);
    EXPECT_EQ(0, remaining->memid);
    EXPECT_EQ(2, remaining->seq);

    free_obmm_dev();
}

TEST_F(IomemTest, CleanObmmDevAllSeqMatch)
{
    free_obmm_dev();

    struct memid_range *mr1 = (struct memid_range *)kzalloc(sizeof(*mr1), GFP_KERNEL);
    ASSERT_NE(nullptr, mr1);
    mr1->memid = 0;
    mr1->seq = 3;
    list_add(&mr1->node, &obmm_dev.list);

    struct memid_range *mr2 = (struct memid_range *)kzalloc(sizeof(*mr2), GFP_KERNEL);
    ASSERT_NE(nullptr, mr2);
    mr2->memid = 7;
    mr2->seq = 3;
    list_add_tail(&mr2->node, &obmm_dev.list);

    clean_obmm_dev();

    EXPECT_FALSE(list_empty(&obmm_dev.list));

    free_obmm_dev();
}

// ========== DT supplement: inc_obmm_dev_seq ==========

extern "C" void inc_obmm_dev_seq(void);

TEST_F(IomemTest, IncObmmDevSeqEmptyList)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));
    inc_obmm_dev_seq();
    EXPECT_TRUE(list_empty(&obmm_dev.list));
}

TEST_F(IomemTest, IncObmmDevSeqIncrement)
{
    free_obmm_dev();

    struct memid_range *mr = (struct memid_range *)kzalloc(sizeof(*mr), GFP_KERNEL);
    ASSERT_NE(nullptr, mr);
    mr->memid = 0;
    mr->seq = 5;
    list_add(&mr->node, &obmm_dev.list);

    inc_obmm_dev_seq();

    EXPECT_EQ(6, mr->seq);

    free_obmm_dev();
}

// ========== DT supplement: iterate_obmm_dev_dir ==========

extern "C" int iterate_obmm_dev_dir(void);
extern "C" long PTR_ERR(const void *ptr);

TEST_F(IomemTest, IterateObmmDevDirOpenFail)
{
    MOCKER(filp_open).stubs().will(returnValue((struct file *)(long)-ENOENT));
    MOCKER(IS_ERR).stubs().will(returnValue(true));
    MOCKER(PTR_ERR).stubs().will(returnValue(-ENOENT));
    int ret = iterate_obmm_dev_dir();
    EXPECT_EQ(-ENOENT, ret);
}

TEST_F(IomemTest, IterateObmmDevDirIterateFail)
{
    struct file filp;
    MOCKER(filp_open).stubs().will(returnValue(&filp));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(iterate_dir).stubs().will(returnValue(-ENOTDIR));
    MOCKER(filp_close).stubs().will(ignoreReturnValue());
    int ret = iterate_obmm_dev_dir();
    EXPECT_EQ(-ENOTDIR, ret);
}

TEST_F(IomemTest, IterateObmmDevDirSuccess)
{
    struct file filp;
    MOCKER(filp_open).stubs().will(returnValue(&filp));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(iterate_dir).stubs().will(returnValue(0));
    MOCKER(filp_close).stubs().will(ignoreReturnValue());
    int ret = iterate_obmm_dev_dir();
    EXPECT_EQ(0, ret);
}

// ========== DT supplement: iterate_obmm_dev ==========

extern "C" int iterate_obmm_dev(void);
extern "C" void update_obmm_dev_pa(void);

TEST_F(IomemTest, IterateObmmDevEmptyListInitFail)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));

    MOCKER(kzalloc).stubs().will(returnValue((void*)nullptr));
    int ret = iterate_obmm_dev();
    EXPECT_EQ(-ENOMEM, ret);
}

TEST_F(IomemTest, IterateObmmDevEmptyListInitSuccess)
{
    free_obmm_dev();
    ASSERT_TRUE(list_empty(&obmm_dev.list));

    MOCKER(iterate_obmm_dev_dir).stubs().will(returnValue(0));
    MOCKER(update_obmm_dev_pa).stubs().will(ignoreReturnValue());

    int ret = iterate_obmm_dev();
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(list_empty(&obmm_dev.list));

    free_obmm_dev();
}

TEST_F(IomemTest, IterateObmmDevExistingListIterateFail)
{
    free_obmm_dev();
    struct memid_range *mr = (struct memid_range *)kzalloc(sizeof(*mr), GFP_KERNEL);
    ASSERT_NE(nullptr, mr);
    mr->memid = 0;
    mr->seq = 0;
    list_add(&mr->node, &obmm_dev.list);

    MOCKER(iterate_obmm_dev_dir).stubs().will(returnValue(-ENOENT));
    int ret = iterate_obmm_dev();
    EXPECT_EQ(-ENOENT, ret);

    free_obmm_dev();
}

TEST_F(IomemTest, IterateObmmDevWithExistingList)
{
    free_obmm_dev();
    struct memid_range *mr = (struct memid_range *)kzalloc(sizeof(*mr), GFP_KERNEL);
    ASSERT_NE(nullptr, mr);
    mr->memid = 0;
    mr->seq = 5;
    list_add(&mr->node, &obmm_dev.list);

    MOCKER(iterate_obmm_dev_dir).stubs().will(returnValue(0));
    MOCKER(update_obmm_dev_pa).stubs().will(ignoreReturnValue());

    int ret = iterate_obmm_dev();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(6, mr->seq);

    free_obmm_dev();
}

// ========== DT supplement: calc_acidx_paddr_iomem (additional) ==========

extern "C" u32 g_pagesize_huge;

TEST_F(IomemTest, CalcAcidxPaddrIomemNidNotFound)
{
    u64 paddr;

    struct ram_segment *seg = (struct ram_segment *)kmalloc(sizeof(*seg), GFP_KERNEL);
    ASSERT_NE(nullptr, seg);
    seg->numa_node = 0;
    seg->start = 0x1000;
    seg->end = 0x1FFF;
    INIT_LIST_HEAD(&remote_ram_list);
    list_add_tail(&seg->node, &remote_ram_list);

    MOCKER(is_smap_pg_huge).stubs().will(returnValue(false));
    int ret = calc_acidx_paddr_iomem(0, 5, &paddr);
    EXPECT_EQ(-34, ret);

    list_del(&seg->node);
    kfree(seg);
}

TEST_F(IomemTest, CalcAcidxPaddrIomemIndexOutOfRange)
{
    u64 paddr;
    g_pagesize_huge = 0x200000;

    struct ram_segment *seg = (struct ram_segment *)kmalloc(sizeof(*seg), GFP_KERNEL);
    ASSERT_NE(nullptr, seg);
    seg->numa_node = 1;
    seg->start = 0x1000;
    seg->end = 0x1FFF;
    INIT_LIST_HEAD(&remote_ram_list);
    list_add_tail(&seg->node, &remote_ram_list);

    MOCKER(is_smap_pg_huge).stubs().will(returnValue(true));
    int ret = calc_acidx_paddr_iomem(10, 1, &paddr);
    EXPECT_EQ(-34, ret);

    list_del(&seg->node);
    kfree(seg);
}

// ========== DT supplement: find_range_by_memid (additional) ==========

TEST_F(IomemTest, FindRangeByMemidInvalidRange)
{
    u64 start, end;
    free_obmm_dev();

    struct memid_range mr;
    mr.memid = 1;
    mr.start = 0xFF;
    mr.end = 0x0;

    list_add(&mr.node, &obmm_dev.list);
    int ret = find_range_by_memid(1, &start, &end);
    EXPECT_EQ(-ENOENT, ret);

    list_del(&mr.node);
}

TEST_F(IomemTest, FindRangeByMemidStartEqualsEnd)
{
    u64 start, end;
    free_obmm_dev();

    struct memid_range mr;
    mr.memid = 1;
    mr.start = 0x1000;
    mr.end = 0x1000;

    list_add(&mr.node, &obmm_dev.list);
    int ret = find_range_by_memid(1, &start, &end);
    EXPECT_EQ(-ENOENT, ret);

    list_del(&mr.node);
}

// ========== DT supplement: fill_obmmdev (additional) ==========

TEST_F(IomemTest, FillObmmdevMatchingName)
{
    struct read_obmm_callback call;
    call.ret = 1;

    MOCKER(update_obmm_dev).stubs().will(ignoreReturnValue());
    bool ret = fill_obmmdev(&call.ctx, "obmm_shmdev5", 11, 0, 0, 0);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(0, call.ret);
}

TEST_F(IomemTest, FillObmmdevSscanfFail)
{
    struct read_obmm_callback call;
    call.ret = 1;

    bool ret = fill_obmmdev(&call.ctx, "obmm_shmdevABC", 13, 0, 0, 0);
    EXPECT_EQ(false, ret);
    EXPECT_NE(0, call.ret);
}
