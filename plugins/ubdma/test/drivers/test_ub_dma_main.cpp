/*
 * Description: main test
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
#include "mockcpp/ChainingMockHelper.h"
#include "mockcpp/mokc.h"
#include "stub_struct.h"
#include "urma.h"
#include "ubcore_types.h"
#include "ubcore_opcode.h"

#define SMAP_MIG_MAGIC 0xbe
#define SMAP_MIG_MIGRATE _IOW(SMAP_MIG_MAGIC, 0, struct migrate_msg)
#define SMAP_MIG_FOLIO_MIGRATE _IOW(SMAP_MIG_MAGIC, 1, struct migrate_msg)

using namespace std;

class TestUbDmaMain : public ::testing::Test {
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
    struct ub_dma_desc *to_ub_dma_desc(struct dma_async_tx_descriptor *tx);
    struct ub_dma_dev *to_ub_dma_dev(struct dma_device *d);
    struct ub_dma_vchan *to_ub_dma_vchan(struct dma_chan *chan);
    void ub_dma_free_desc(struct ub_virt_dma_desc *vd);
    struct dma_async_tx_descriptor *ub_dma_prep_dma_memcpy(
        struct dma_chan *chan, dma_addr_t dest, dma_addr_t src, size_t len, unsigned long flags);
    int get_urma_trans_segment(struct urma_trans_segment_info *src_info, struct urma_trans_segment_info *dst_info);
    void ub_dma_issue_pending(struct dma_chan *chan);
    int urma_run_send(uint64_t src_va, uint64_t dst_va, struct ubcore_target_seg *src_seg,
        struct ubcore_target_seg *dst_seg, uint32_t len);
    enum dma_status ub_dma_tx_status(struct dma_chan *chan, dma_cookie_t cookie, struct dma_tx_state *state);
    dma_addr_t ub_dma_map_page(struct device *dev, struct page* page,
        unsigned long offset, size_t size, enum dma_data_direction dir, unsigned long attrs);
    void ub_dma_unmap_page(struct device *dev, dma_addr_t dma_addr,
        size_t size, enum dma_data_direction dir, unsigned long attrs);
    int ub_dma_config(struct dma_chan *chan, struct dma_slave_config *config);
    void ub_dma_free(struct ub_dma_dev *udev);
    int ub_dma_open(struct inode *inode, struct file *file);
    int ub_dma_release(struct inode *inode, struct file *file);
    int ub_dma_dev_init(void);
    int cdev_add(struct cdev *p, dev_t dev, unsigned count);
    void ub_dma_dev_exit(void);
    void release_urma_mem_trans(void);
    void ub_dma_exit(void);
    int ub_dma_init(void);
    int dma_async_device_register(struct dma_device *device);
    int init_urma_mem_trans(ubcore_comp_callback_t jfce_handler);
}

TEST_F(TestUbDmaMain, ub_dma_prep_dma_memcpy_test)
{
    dma_async_tx_descriptor tx = {0};
    dma_device dev = {0};
    dma_chan chan = {0};
    ub_virt_dma_desc vd = {0};
    ub_dma_desc *ret1 = to_ub_dma_desc(&tx);
    EXPECT_TRUE(ret1 != nullptr);
    ub_dma_dev *ret2 = to_ub_dma_dev(&dev);
    EXPECT_TRUE(ret2 != nullptr);
    ub_dma_vchan *ret3 = to_ub_dma_vchan(&chan);
    EXPECT_TRUE(ret3 != nullptr);
    MOCKER(to_ub_dma_desc).stubs().will(returnValue(ret1));
    MOCKER(kfree).stubs().will(returnValue(0));
    ub_dma_free_desc(&vd);

    GlobalMockObject::verify();
    unsigned long flags = 0;
    dma_addr_t dest = 0;
    dma_addr_t src = 0;
    size_t len = 0;
    ub_dma_vchan vchan = {0};
    MOCKER(to_ub_dma_vchan).stubs().will(returnValue(&vchan));
    dma_async_tx_descriptor *ret4 = ub_dma_prep_dma_memcpy(&chan, dest, src, len, flags);
    EXPECT_TRUE(ret4 == nullptr);
    len = 1;
    vchan.vc.desc_allocated.next = &vchan.vc.desc_allocated;
    vchan.vc.desc_allocated.prev = &vchan.vc.desc_allocated;
    ret4 = ub_dma_prep_dma_memcpy(&chan, dest, src, len, flags);
    EXPECT_TRUE(ret4 != nullptr);
}

void test_init_vchan(ub_dma_desc *txd, dma_device *dmaDev, device *dev, ub_dma_vchan *vchan)
{
    txd->vd.tx.cookie = 2; // 2
    txd->vd.tx.chan = &vchan->vc.chan;
    vchan->vc.desc_completed.next = &vchan->vc.desc_completed;
    vchan->vc.desc_completed.prev = &vchan->vc.desc_completed;
    vchan->vc.desc_issued.next = &vchan->vc.desc_issued;
    vchan->vc.desc_issued.prev = &vchan->vc.desc_issued;
    vchan->vc.desc_submitted.next = &txd->vd.node;
    vchan->vc.desc_submitted.prev = &txd->vd.node;
    txd->vd.node.next = &vchan->vc.desc_submitted;
    txd->vd.node.prev = &vchan->vc.desc_submitted;
    vchan->node.next = &vchan->node;
    vchan->node.prev = &vchan->node;
    vchan->vc.chan.device = dmaDev;
    vchan->vc.chan.device->dev = dev;
}

TEST_F(TestUbDmaMain, ub_dma_issue_pending_test)
{
    dma_device dmaDev;
    device dev;
    struct ub_dma_desc txd;

    ub_dma_vchan *vchan = (ub_dma_vchan *)malloc(sizeof(ub_dma_vchan));
    EXPECT_TRUE(vchan != nullptr);
    ubcore_target_seg *ubseg1 = nullptr;
    ubcore_target_seg ubseg2;
    test_init_vchan(&txd, &dmaDev, &dev, vchan);
    MOCKER(get_urma_trans_segment).stubs().will(returnValue(-1));
    ub_dma_issue_pending(&vchan->vc.chan);
    EXPECT_TRUE(vchan->vc.desc_completed.next != &vchan->vc.desc_completed);
    free(vchan);
}

TEST_F(TestUbDmaMain, ub_dma_map_page_test)
{
    ub_dma_vchan *vchan = (ub_dma_vchan *)malloc(sizeof(ub_dma_vchan));
    EXPECT_TRUE(vchan != nullptr);
    dma_chan chan = {0};
    dma_cookie_t cookie = 0;
    dma_tx_state state = {0};
    dma_device dmaDev;
    device dev;
    struct ub_dma_desc txd;
    test_init_vchan(&txd, &dmaDev, &dev, vchan);
    int ret1 = ub_dma_tx_status(&vchan->vc.chan, cookie, &state);
    EXPECT_TRUE(ret1 != 0);

    GlobalMockObject::verify();
    page page1 = {0};
    dma_addr_t ret2 = ub_dma_map_page(&dev, &page1, 0, 0, DMA_BIDIRECTIONAL, 0);
    EXPECT_TRUE(ret2 != 0);

    GlobalMockObject::verify();
    ub_dma_unmap_page(&dev, 0, 0, DMA_BIDIRECTIONAL, 0);

    GlobalMockObject::verify();
    dma_slave_config config;
    ret1 = ub_dma_config(&chan, &config);
    EXPECT_EQ(ret1, 0);

    GlobalMockObject::verify();
    ub_dma_dev udev = {0};
    udev.vchan_num = 0;
    ub_dma_free(&udev);
    free(vchan);
}

unsigned long error1_copy_from_user(void *to, const void *from, unsigned long n)
{
    struct migrate_msg *msg = (migrate_msg*)to;
    return 1;
}

unsigned long error2_copy_from_user(void *to, const void *from, unsigned long n)
{
    struct migrate_msg *msg = (migrate_msg*)to;
    msg->pfn = 0;
    return 0;
}

unsigned long error3_copy_from_user(void *to, const void *from, unsigned long n)
{
    struct migrate_msg *msg = (migrate_msg*)to;
    msg->pfn = 1;
    return 0;
}

TEST_F(TestUbDmaMain, ub_dma_dev_init_test)
{
    struct file *fd;
    struct inode *inode;
    int ret1 = ub_dma_open(inode, fd);
    EXPECT_EQ(ret1, 0);
    ret1 = ub_dma_release(inode, fd);
    EXPECT_EQ(ret1, 0);

    GlobalMockObject::verify();
    MOCKER(alloc_chrdev_region).stubs().will(returnValue(-1));
    ret1 = ub_dma_dev_init();
    EXPECT_EQ(ret1, -1);
    GlobalMockObject::verify();
    MOCKER(cdev_add).stubs().will(returnValue(1));
    ret1 = ub_dma_dev_init();
    EXPECT_EQ(ret1, 1);

    GlobalMockObject::verify();
    MOCKER(IS_ERR).stubs().will(returnValue(true));
    ret1 = ub_dma_dev_init();
    EXPECT_EQ(ret1, 0);

    GlobalMockObject::verify();
    MOCKER(IS_ERR).stubs().will(returnValue(false)).then(returnValue(true));
    ret1 = ub_dma_dev_init();
    EXPECT_EQ(ret1, 0);

    GlobalMockObject::verify();
    ub_dma_dev_exit();
}

TEST_F(TestUbDmaMain, ub_dma_init_test)
{
    MOCKER(release_urma_mem_trans).stubs();
    MOCKER(ub_dma_dev_exit).stubs();
    ub_dma_exit();
    MOCKER(ub_dma_dev_init).stubs().will(returnValue(1));
    int ret1 = ub_dma_init();
    EXPECT_EQ(ret1, -EBUSY);

    GlobalMockObject::verify();
    MOCKER(ub_dma_dev_init).stubs().will(returnValue(0));
    MOCKER(devm_kzalloc).stubs().will(returnValue((void*)0));
    ret1 = ub_dma_init();
    EXPECT_EQ(ret1, -ENOMEM);

    GlobalMockObject::verify();
    MOCKER(ub_dma_dev_init).stubs().will(returnValue(0));
    MOCKER(devm_kcalloc).stubs().will(returnValue((void*)0));
    MOCKER(ub_dma_free).stubs();
    MOCKER(ub_dma_dev_exit).stubs();
    ret1 = ub_dma_init();
    EXPECT_EQ(ret1, -ENOMEM);

    GlobalMockObject::verify();
    MOCKER(ub_dma_dev_init).stubs().will(returnValue(0));
    MOCKER(dma_async_device_register).stubs().will(returnValue(1));
    MOCKER(ub_dma_free).stubs();
    MOCKER(ub_dma_dev_exit).stubs();
    ret1 = ub_dma_init();
    EXPECT_EQ(ret1, 1);

    GlobalMockObject::verify();
    MOCKER(ub_dma_dev_init).stubs().will(returnValue(0));
    MOCKER(init_urma_mem_trans).stubs().will(returnValue(1));
    MOCKER(ub_dma_free).stubs();
    MOCKER(ub_dma_dev_exit).stubs();
    ret1 = ub_dma_init();
    EXPECT_EQ(ret1, 1);
}