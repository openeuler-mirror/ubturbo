/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: iomem test
 */
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <iostream>
#include <cstring>
#include "gtest/gtest.h"
#include "linux/compiler_attributes.h"
#include "linux/device.h"
#include "linux/memory.h"
#include "linux/notifier.h"
#include "linux/ioport.h"
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

using namespace std;

class TestUbDmaIomem : public ::testing::Test {
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
    int insert_remote_ram(u64 pa_start, u64 pa_end, struct list_head *head);
    int pfn_valid(unsigned long pfn);
    struct page *pfn_to_online_page(unsigned long pfn);
    int page_to_nid(const struct page *page);
    int update_resource(struct resource *r, void *arg);
    int iomem_urma_sge_init(void);
    void destory_iomem_workqueue(void);
    int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start, u64 end, void *arg,
                            int (*func)(struct resource *, void *));
    void destroy_workqueue(struct workqueue_struct *wq);
    void flush_workqueue(struct workqueue_struct *wq);
    bool cancel_work_sync(struct work_struct *work);
    extern struct list_head remote_ram_list;
    int ub_dma_register_segment(u64 pa_start, u64 pa_end);
}

TEST_F(TestUbDmaIomem, insert_remote_ram_test)
{
    list_head head;
    INIT_LIST_HEAD(&head);
    MOCKER(pfn_valid).stubs().will(returnValue(0));
    int ret = insert_remote_ram(0, 1, &head);
    EXPECT_EQ(ret, 0);

    GlobalMockObject::verify();
    MOCKER(kmalloc).stubs().will(returnValue((void *)nullptr));
    ret = insert_remote_ram(0, 1, &head);
    EXPECT_EQ(ret, -ENOMEM);

    GlobalMockObject::verify();
    ret = insert_remote_ram(0, 1, &head);
    EXPECT_EQ(ret, 0);

    GlobalMockObject::verify();
    MOCKER(page_to_nid).stubs().will(returnValue(-1));
    ret = insert_remote_ram(0, 1, &head);
    EXPECT_EQ(ret, -EINVAL);
}

TEST_F(TestUbDmaIomem, update_resource_test)
{
    list_head head;
    resource rsc;
    rsc.flags = IORESOURCE_SYSRAM_DRIVER_MANAGED;
    MOCKER(pfn_valid).stubs().will(returnValue(0));
    int ret = update_resource(nullptr, nullptr);
    EXPECT_EQ(ret, -EINVAL);

    MOCKER(insert_remote_ram).stubs().will(returnValue(1));
    ret = update_resource(&rsc, &head);
    EXPECT_EQ(ret, 1);

    GlobalMockObject::verify();
    MOCKER(insert_remote_ram).stubs().will(returnValue(0));
    ret = update_resource(&rsc, &head);
    EXPECT_EQ(ret, 0);
}

TEST_F(TestUbDmaIomem, iomem_urma_sge_init_test)
{
    int ret = iomem_urma_sge_init();
    EXPECT_EQ(ret, 0);
}

TEST_F(TestUbDmaIomem, destory_iomem_workqueue_test)
{
    destory_iomem_workqueue();
}
