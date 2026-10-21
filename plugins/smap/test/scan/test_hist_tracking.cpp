/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP3.0 hist_tracking.c test code
 * Author: z30062841
 * Create: 2024-12-28
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/types.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "acpi_mem.h"
#include "check.h"
#include "hist_ops.h"
#include "hist_tracking.h"
#include "iomem.h"
#include "scan_main.h"

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
    hist_thread_resume();
    EXPECT_EQ(true, g_smap_hist_dev.thread_enable);
}

extern "C" int hist_tracking_disable(void);
TEST_F(HistTrackingTest, hist_tracking_disable)
{
    hist_thread_pause();
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

extern "C" u8 remote_scan_mode;
extern "C" bool hist_inited;
extern "C" bool is_hist_tracking_node(int node);
TEST_F(HistTrackingTest, is_hist_tracking_node)
{
    int savedNrLocalNuma = nr_local_numa;
    nr_local_numa = 4;

    /* 硬件未初始化时不参与远端扫描，等价纯软 */
    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_HARD;
    EXPECT_EQ(false, is_hist_tracking_node(4));

    /* 纯软模式下硬件子系统不参与 */
    hist_inited = true;
    remote_scan_mode = SMAP_REMOTE_SCAN_SOFT;
    EXPECT_EQ(false, is_hist_tracking_node(4));

    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
    EXPECT_EQ(true, is_hist_tracking_node(4));
    EXPECT_EQ(false, is_hist_tracking_node(3));
    EXPECT_EQ(false, is_hist_tracking_node(SMAP_MAX_NUMNODES));

    remote_scan_mode = SMAP_REMOTE_SCAN_HARD;
    EXPECT_EQ(true, is_hist_tracking_node(4));

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
    nr_local_numa = savedNrLocalNuma;
}
