/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: virt dma test
 */
#include "gtest/gtest.h"
#include "linux/list.h"
#include "linux/dmaengine.h"
#include "mockcpp/ChainingMockHelper.h"
#include "mockcpp/mokc.h"
#include "stub_struct.h"

using namespace std;

size_t ub_vchan_complete_free_count = 0;
size_t ub_vchan_free_list_count = 0;

class TestUbVirtDma : public testing::Test {
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        ub_vchan_complete_free_count = 0;
        ub_vchan_free_list_count = 0;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }
};
extern "C" dma_cookie_t ub_dma_vchan_tx_submit(struct dma_async_tx_descriptor *tx);
TEST_F(TestUbVirtDma, ub_dma_vchan_tx_submit)
{
    ub_virt_dma_chan chan = {};
    ub_virt_dma_desc desc = {};
    desc.node = LIST_HEAD_INIT(desc.node);
    desc.tx.chan = &chan.chan;
    chan.chan.cookie = 1;
    chan.desc_submitted = LIST_HEAD_INIT(chan.desc_submitted);
    auto cookie = ub_dma_vchan_tx_submit(&desc.tx);
    ASSERT_EQ(cookie, desc.tx.cookie);
    ASSERT_EQ(desc.node.prev, &chan.desc_submitted);
}
extern "C" int vchan_tx_desc_free(struct dma_async_tx_descriptor *tx);
TEST_F(TestUbVirtDma, vchan_tx_desc_free)
{
    ub_virt_dma_chan chan = {};
    ub_virt_dma_desc desc = {};
    desc.node = LIST_HEAD_INIT(desc.node);
    desc.tx.chan = &chan.chan;
    chan.chan.cookie = 1;
    chan.desc_free = +[](struct ub_virt_dma_desc *) {
        ub_vchan_complete_free_count++;
    };
    vchan_tx_desc_free(&desc.tx);
    ASSERT_NE(desc.node.next, &desc.node);
    ASSERT_NE(desc.node.prev, &desc.node);
    ASSERT_EQ(ub_vchan_complete_free_count, 1);
}

extern "C" void ub_vchan_complete(struct tasklet_struct *t);
TEST_F(TestUbVirtDma, ub_vchan_complete)
{
    ub_virt_dma_chan chan = {};
    ub_virt_dma_desc desc = {};
    desc.tx.chan = &chan.chan;
    chan.desc_free = +[](struct ub_virt_dma_desc *) {
        ub_vchan_complete_free_count++;
    };
    desc.node = LIST_HEAD_INIT(desc.node);
    chan.desc_completed = desc.node;
    ub_vchan_complete(&chan.task);
    ASSERT_EQ(ub_vchan_complete_free_count, 1);
}
extern "C" void vchan_init(struct ub_virt_dma_chan *vc, struct dma_device *dmadev);
TEST_F(TestUbVirtDma, vchan_init)
{
    ub_virt_dma_chan chan = {};
    dma_device dmadev = {};
    dmadev.channels = LIST_HEAD_INIT(dmadev.channels);
    vchan_init(&chan, &dmadev);
    ASSERT_EQ(chan.desc_allocated.next, &chan.desc_allocated);
    ASSERT_EQ(chan.desc_completed.next, &chan.desc_completed);
    ASSERT_EQ(chan.desc_submitted.next, &chan.desc_submitted);
    ASSERT_EQ(chan.desc_terminated.next, &chan.desc_terminated);
    ASSERT_EQ(chan.desc_issued.next, &chan.desc_issued);
    ASSERT_EQ(chan.chan.device_node.next, &dmadev.channels);
}

extern "C" void vchan_dma_desc_free_list(struct ub_virt_dma_chan *vc, struct list_head *head);
TEST_F(TestUbVirtDma, vchan_dma_desc_free_list)
{
    ub_virt_dma_chan chan = {};
    dma_device dmadev = {};
    dmadev.channels = LIST_HEAD_INIT(dmadev.channels);
    vchan_init(&chan, &dmadev);

    ub_virt_dma_desc desc1 = {};
    ub_virt_dma_desc desc2 = {};
    desc1.tx.chan = &chan.chan;
    desc2.tx.chan = &chan.chan;
    desc1.node = LIST_HEAD_INIT(desc1.node);
    desc2.node = LIST_HEAD_INIT(desc2.node);
    chan.desc_free = +[](struct ub_virt_dma_desc *) {
        ub_vchan_free_list_count++;
    };

    LIST_HEAD(head);
    list_add_tail(&desc1.node, &head);
    list_add_tail(&desc2.node, &head);

    vchan_dma_desc_free_list(&chan, &head);
    ASSERT_EQ(ub_vchan_free_list_count, 2);
}
