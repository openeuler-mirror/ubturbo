/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: feature test
 */
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <iostream>
#include <cstring>
#include "gtest/gtest.h"
#include "linux/compiler_attributes.h"
#include "linux/device.h"
#include "linux/types.h"
#include "linux/workqueue.h"
#include "linux/dmaengine.h"
#include "mockcpp/ChainingMockHelper.h"
#include "mockcpp/mokc.h"
#include "stub_struct.h"
#include "urma.h"
#include "ubcore_types.h"
#include "ubcore_opcode.h"

using namespace std;

struct ub_dma_desc_full {
    struct ub_virt_dma_desc vd;
    uint64_t src_addr;
    uint64_t dest_addr;
    size_t len;
    struct tmp_urma_segment_info tmp_sge;
};

class TestUbDmaFeature : public ::testing::Test {
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
    int folio_mc_dma_copy(struct folio *dst, struct folio *src);
    struct dma_chan *__dma_request_channel(const dma_cap_mask_t *mask, dma_filter_fn fn, void *fn_param,
                                           struct device_node *np);
    int dma_mapping_error(struct device *dev, dma_addr_t dma_addr);
    struct dma_async_tx_descriptor *dmaengine_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dest, dma_addr_t src,
                                                              size_t len, unsigned long flags);
    int ub_dma_init(void);
    int init_urma_mem_trans(ubcore_comp_callback_t jfce_handler);
    void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
    struct dma_async_tx_descriptor *ub_dma_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dest, dma_addr_t src,
                                                           size_t len, unsigned long flags);
    dma_cookie_t ub_dma_vchan_tx_submit(struct dma_async_tx_descriptor *tx);
    void ub_dma_issue_pending(struct dma_chan *chan);
    int get_urma_trans_segment(struct urma_trans_segment_info *src_info, struct urma_trans_segment_info *dst_info);
    int urma_run_send(uint64_t src_va, uint64_t dst_va, struct ubcore_target_seg *src_seg,
                      struct ubcore_target_seg *dst_seg, uint32_t len);
    void ub_dma_free_chan_resources(struct dma_chan *chan);
    struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active);
}

struct ub_dma_dev *udc;
void *test_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
    udc = (ub_dma_dev*)calloc(1, size);
    return static_cast<void*>(udc);
}
bool folio_dma_chan_filter(struct dma_chan *chan, void *param)
{
    return true;
}

struct dma_chan *test_request_channel(const dma_cap_mask_t *mask, dma_filter_fn fn, void *fn_param,
                                      struct device_node *np)
{
    struct device dev;
    struct dma_chan *chan;
    list_for_each_entry(chan, &udc->slave.channels, device_node)
    {
        if (chan->client_count) {
            continue;
        }
        cout << chan->device->dev->init_name << endl;
        if (fn && !fn(chan, fn_param)) {
            continue;
        }
        return chan;
    }
    return nullptr;
}

TEST_F(TestUbDmaFeature, test_folio_mc_dma_copy)
{
    device_driver driver;
    workqueue_struct workque;
    MOCKER(alloc_workqueue).stubs().will(returnValue(&workque));
    MOCKER(devm_kzalloc).stubs().will(invoke(test_kzalloc));
    MOCKER(init_urma_mem_trans).stubs().will(returnValue(0));
    int ret = ub_dma_init();
    EXPECT_EQ(ret, 0);

    struct dma_async_tx_descriptor *tx;
    dma_chan *chan = test_request_channel(0, folio_dma_chan_filter, NULL, NULL);
    EXPECT_TRUE(chan != nullptr);

    tx = ub_dma_prep_dma_memcpy(chan, 0x50100000000, 0x50000000000, 1024, DMA_PREP_INTERRUPT); // 1024
    EXPECT_TRUE(tx->tx_submit != NULL);
    EXPECT_TRUE(tx->desc_free != NULL);
    EXPECT_TRUE(tx->flags == DMA_PREP_INTERRUPT);
    EXPECT_TRUE(tx->chan == chan);
    ub_dma_vchan *vchan = container_of(chan, struct ub_dma_vchan, vc.chan);
    EXPECT_TRUE(vchan->vc.desc_allocated.next != &vchan->vc.desc_allocated);
    dma_cookie_t cookie = ub_dma_vchan_tx_submit(tx);
    EXPECT_TRUE(tx->chan->cookie == 2); // 2
    EXPECT_TRUE(cookie == 2); // 2
    EXPECT_TRUE(tx->cookie == 2); // 2
    EXPECT_TRUE(vchan->vc.desc_allocated.next == &vchan->vc.desc_allocated);
    EXPECT_TRUE(vchan->vc.desc_submitted.next != &vchan->vc.desc_submitted);

    MOCKER(get_urma_trans_segment).stubs().will(returnValue(0));
    MOCKER(urma_run_send).stubs().will(returnValue(0));
    ub_dma_issue_pending(chan);
    EXPECT_TRUE(vchan->vc.desc_submitted.next == &vchan->vc.desc_submitted);
    EXPECT_TRUE(vchan->vc.desc_issued.next != &vchan->vc.desc_issued);

    ub_dma_free_chan_resources(chan);
    EXPECT_TRUE(vchan->vc.desc_issued.next == &vchan->vc.desc_issued);
    EXPECT_TRUE(vchan->vc.desc_submitted.next == &vchan->vc.desc_submitted);
    EXPECT_TRUE(vchan->vc.desc_allocated.next == &vchan->vc.desc_allocated);
    EXPECT_TRUE(vchan->vc.desc_completed.next == &vchan->vc.desc_completed);
}

extern "C" {
    int ubcore_poll_jfc(struct ubcore_jfc *jfc, int cr_cnt, struct ubcore_cr *cr);
    void ub_dma_client_comp_callback(struct ubcore_jfc *jfc);
}

ubcore_cr jfr_cr[1024];
uint32_t jfr_cr_p = 0;

int test_ubcore_poll_jfc(struct ubcore_jfc *jfc, int cr_cnt, struct ubcore_cr *cr)
{
    int cnt = 0;
    for (int i = 0; i < cr_cnt; i++) {
        cr[i].status = jfr_cr[jfr_cr_p++].status;
        cnt++;
    }
    return cnt;
}
TEST_F(TestUbDmaFeature, test_ub_dma_client_comp_callback)
{
    ubcore_jfc jfc;
    ub_dma_dev udv;
    ubcore_cr cr[1024];
    urma_mem_trans urma_handler;
    jfc.urma_jfc = (uint64_t)&udv;
    urma_handler.cr = cr;
    for (int i = 0; i < 10; i++) { // 10
        jfr_cr[i].status = UBCORE_CR_SUCCESS;
    }
    jfr_cr_p = 0;
    workqueue_struct workque;
    MOCKER(alloc_workqueue).stubs().will(returnValue(&workque));
    MOCKER(devm_kzalloc).stubs().will(invoke(test_kzalloc));
    MOCKER(init_urma_mem_trans).stubs().will(returnValue(0));
    int ret = ub_dma_init();
    EXPECT_EQ(ret, 0);

    struct dma_chan *chan;
    list_for_each_entry(chan, &udc->slave.channels, device_node) {
        break;
    }
    ub_dma_vchan *vchan = container_of(chan, struct ub_dma_vchan, vc.chan);
    struct ub_dma_desc_full txd = {};
    dma_device dmaDev;
    device dev;
    txd.vd.tx.cookie = 2; // 2
    txd.vd.tx.chan = &vchan->vc.chan;
    vchan->vc.desc_submitted.next = &vchan->vc.desc_submitted;
    vchan->vc.desc_submitted.prev = &vchan->vc.desc_submitted;
    vchan->vc.desc_completed.next = &vchan->vc.desc_completed;
    vchan->vc.desc_completed.prev = &vchan->vc.desc_completed;
    vchan->vc.desc_issued.next = &txd.vd.node;
    vchan->vc.desc_issued.prev = &txd.vd.node;
    txd.vd.node.next = &vchan->vc.desc_issued;
    txd.vd.node.prev = &vchan->vc.desc_issued;
    vchan->node.next = &vchan->node;
    vchan->node.prev = &vchan->node;
    vchan->vc.chan.device = &dmaDev;
    vchan->vc.chan.device->dev = &dev;
    MOCKER(ubcore_poll_jfc).stubs().will(invoke(test_ubcore_poll_jfc));
    MOCKER(get_urma_jetty).stubs().will(returnValue(&urma_handler));
    ub_dma_client_comp_callback(&jfc);
    EXPECT_EQ(vchan->status, DMA_COMPLETE);
    EXPECT_TRUE(vchan->vc.desc_issued.next == &vchan->vc.desc_issued);
    EXPECT_TRUE(vchan->vc.desc_submitted.next == &vchan->vc.desc_submitted);
    EXPECT_TRUE(vchan->vc.desc_allocated.next == &vchan->vc.desc_allocated);
    EXPECT_TRUE(vchan->vc.desc_completed.next != &vchan->vc.desc_completed);
}
