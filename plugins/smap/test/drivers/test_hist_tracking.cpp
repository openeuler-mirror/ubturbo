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
#include "bus.h"
#include "hist_ops.h"
#include "access_iomem.h"
#include "access_acpi_mem.h"
#include "access_tracking.h"
#include "access_iomem.h"
#include "hist_tracking.h"

using namespace std;

extern "C" struct list_head access_dev;
extern "C" struct list_head remote_ram_list;
extern "C" int nr_local_numa;

class HistTrackingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        INIT_LIST_HEAD(&access_dev);
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
extern "C" void hist_tracking_enable(struct device *ldev);
extern "C" u64 hist_calc_access_len(struct access_tracking_dev *hdev);
TEST_F(HistTrackingTest, hist_tracking_enable)
{
    struct access_tracking_dev hdev = {};
    hdev.is_hist = true;
    hdev.enable_on = false;
    hdev.page_count = 0;
    MOCKER(hist_calc_access_len).stubs().will(returnValue((u64)2));
    hist_tracking_enable(&hdev.ldev);
    EXPECT_EQ(true, hdev.enable_on);
    EXPECT_EQ(true, g_smap_hist_dev.thread_enable);
}

extern "C" void hist_tracking_disable(struct device *ldev);
TEST_F(HistTrackingTest, hist_tracking_disable)
{
    struct access_tracking_dev hdev = {};

    hdev.enable_on = true;
    hdev.is_hist = true;
    hist_tracking_disable(&hdev.ldev);
    EXPECT_EQ(false, hdev.enable_on);
    EXPECT_EQ(false, g_smap_hist_dev.thread_enable);
}

extern "C" void hist_dev_pgsize_update(u8 page_size_mode);
TEST_F(HistTrackingTest, hist_dev_pgsize_update)
{
    MOCKER(hist_update_pgsize).stubs();
    hist_dev_pgsize_update(PAGE_MODE_2M);
}

extern "C" int hist_tracking_set_page_size(struct device *ldev, u8 pgsize);
TEST_F(HistTrackingTest, hist_tracking_set_page_size_invalid)
{
    struct access_tracking_dev hdev = {};
    int ret = hist_tracking_set_page_size(&hdev.ldev, 1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HistTrackingTest, hist_tracking_set_page_size_valid)
{
    struct access_tracking_dev hdev = {};
    MOCKER(hist_update_pgsize).stubs();
    MOCKER(hist_calc_access_len).stubs().will(returnValue((u64)2));
    int ret = hist_tracking_set_page_size(&hdev.ldev, 0);
    EXPECT_EQ(0, ret);
}

extern "C" void hist_tracking_deinit(void);
TEST_F(HistTrackingTest, hist_tracking_deinit)
{
    struct access_tracking_dev *hdev = (struct access_tracking_dev *)kmalloc(
        sizeof(struct access_tracking_dev), GFP_KERNEL);
    hdev->is_hist = 1;
    hdev->tracking_dev = (struct tracking_dev *)malloc(sizeof(struct tracking_dev));
    list_add_tail(&hdev->list, &access_dev);
    MOCKER(tracking_dev_remove).stubs().will(ignoreReturnValue());
    hist_tracking_deinit();
}

extern "C" int hist_tracking_init(void);
TEST_F(HistTrackingTest, hist_tracking_init_success)
{
    int ret;
    struct tracking_dev *trk_dev = (struct tracking_dev *)malloc(sizeof(struct tracking_dev));

    nr_local_numa = SMAP_MAX_NUMNODES - 1;
    MOCKER(hist_calc_access_len).stubs().will(returnValue((u64)1));
    MOCKER(tracking_dev_add).stubs().will(returnValue(trk_dev));
    ret = hist_tracking_init();
    EXPECT_EQ(0, ret);
}

TEST_F(HistTrackingTest, hist_tracking_init_tracking_add_fail)
{
    int ret;

    nr_local_numa = SMAP_MAX_NUMNODES - 1;
    MOCKER(hist_calc_access_len).stubs().will(returnValue((u64)2));
    MOCKER(tracking_dev_add).stubs().will(returnValue((struct tracking_dev *)nullptr));
    ret = hist_tracking_init();
    EXPECT_EQ(-ENODEV, ret);
}

TEST_F(HistTrackingTest, hist_tracking_init_device_add_fail)
{
    int ret;

    nr_local_numa = SMAP_MAX_NUMNODES - 1;
    MOCKER(hist_calc_access_len).stubs().will(returnValue((u64)1));
    MOCKER(device_add).stubs().will(returnValue(-1));
    ret = hist_tracking_init();
    EXPECT_EQ(-ENODEV, ret);
}

extern "C" int hist_module_init(void);
extern "C" int init_acpi_mem(void);
TEST_F(HistTrackingTest, hist_module_init)
{
    int ret;
    struct smap_hist_dev *dev = (struct smap_hist_dev *)malloc(sizeof(struct smap_hist_dev));
    MOCKER(hist_init).stubs().will(returnValue(0));
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(hist_tracking_init).stubs().will(returnValue(0));
    ret = hist_module_init();
    EXPECT_EQ(0, ret);
    free(dev);
}

TEST_F(HistTrackingTest, hist_module_init_two)
{
    int ret;
    struct smap_hist_dev *dev = (struct smap_hist_dev *)malloc(sizeof(struct smap_hist_dev));
    MOCKER(hist_init).stubs().will(returnValue(1));
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    ret = hist_module_init();
    EXPECT_EQ(1, ret);
    GlobalMockObject::verify();

    MOCKER(hist_init).stubs().will(returnValue(0));
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(hist_tracking_init).stubs().will(returnValue(1));
    MOCKER(hist_deinit).stubs().will(ignoreReturnValue());
    ret = hist_module_init();
    EXPECT_EQ(1, ret);
    free(dev);
}
