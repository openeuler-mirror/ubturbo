/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP3.0 hist_tracking.c test code
 * Author: z30062841
 * Create: 2024-12-28
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/types.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/container_of.h>
#include <linux/vmalloc.h>

#include "check.h"
#include "hist_ops.h"
#include "iomem.h"
#include "acpi_mem.h"
#include "scan_main.h"
#include "iomem.h"
#include "hist_tracking.h"

using namespace std;

extern "C" struct list_head remote_ram_list;
extern "C" int nr_local_numa;

class HistTrackingTest : public ::testing::Test {
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

extern "C" struct smap_hist_dev g_smap_hist_dev;
extern "C" void hist_tracking_enable(void);
TEST_F(HistTrackingTest, hist_tracking_enable)
{
    hist_tracking_enable();
    EXPECT_EQ(true, g_smap_hist_dev.thread_enable);
}

extern "C" int hist_tracking_disable(void);
TEST_F(HistTrackingTest, hist_tracking_disable)
{
    hist_tracking_disable();
    EXPECT_EQ(false, g_smap_hist_dev.thread_enable);
}

extern "C" void hist_dev_pgsize_update(u8 page_size_mode);
TEST_F(HistTrackingTest, hist_dev_pgsize_update)
{
    MOCKER(hist_update_pgsize).stubs();
    hist_dev_pgsize_update(PAGE_MODE_2M);
}

extern "C" int hist_tracking_set_page_size(u8 pgsize);
TEST_F(HistTrackingTest, hist_tracking_set_page_size_invalid)
{
    int ret = hist_tracking_set_page_size(1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HistTrackingTest, hist_tracking_set_page_size_valid)
{
    MOCKER(hist_update_pgsize).stubs();
    int ret = hist_tracking_set_page_size(0);
    EXPECT_EQ(0, ret);
}

extern "C" int hist_module_init(void);
TEST_F(HistTrackingTest, hist_module_init)
{
    int ret;
    struct smap_hist_dev *dev = (struct smap_hist_dev *)malloc(sizeof(struct smap_hist_dev));
    MOCKER(hist_init).stubs().will(returnValue(0));
    ret = hist_module_init();
    EXPECT_EQ(0, ret);
    free(dev);
}

TEST_F(HistTrackingTest, hist_module_init_two)
{
    int ret;
    struct smap_hist_dev *dev = (struct smap_hist_dev *)malloc(sizeof(struct smap_hist_dev));
    MOCKER(hist_init).stubs().will(returnValue(1));
    ret = hist_module_init();
    EXPECT_EQ(1, ret);
    free(dev);
}
