/*
* Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
* Description: SMAP3.0 accessed_bit测试代码
*/

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/pgtable.h>
#include <linux/errno.h>
#include <linux/hugetlb.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "acpi_mem.h"
#include "iomem.h"
#include "scan_ioctl.h"
#include "scan_main.h"

using namespace std;

extern "C" int init_acpi_mem(void);
extern "C" bool cancel_delayed_work_sync(struct delayed_work *dwork);
extern "C" bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);

class AccessTrackingTest : public ::testing::Test {
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

extern "C" bool is_access_hugepage(void);
TEST_F(AccessTrackingTest, is_access_hugepage)
{
    (void)is_access_hugepage();
}

extern "C" int access_tracking_set_page_size(u8 page_size_index);
TEST_F(AccessTrackingTest, access_tracking_set_page_size)
{
    int ret = access_tracking_set_page_size(5);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessTrackingTest, node_page_count)
{
    set_node_page_count(0, 10);
    EXPECT_EQ(10, get_node_page_count(0));
    EXPECT_EQ(0, get_node_page_count(-1));
    EXPECT_EQ(0, get_node_page_count(SMAP_MAX_NUMNODES));
    set_node_page_count(0, 0);
}

extern "C" void access_work_func(struct work_struct *work);
extern "C" int access_walk_pagemap_prepare(struct access_pid *ap);
extern "C" int scan_accessed_bit_forward_vm(struct access_pid *ap, int page_size);
extern "C" int scan_accessed_bit_forward_mm(struct access_pid *ap, int page_size);
extern "C" void ap_put_slot(struct ap_slot *slot);
extern "C" int __init scan_init(void);
TEST_F(AccessTrackingTest, scan_prepare_failure_stops_without_retry)
{
    struct access_pid ap = {};
    struct ap_slot slot = {};
    ap.pid = 123;
    ap.type = NORMAL_SCAN;
    ap.ntimes = 1;
    ap.slot = &slot;
    ap.freq_ready = true;
    atomic_set(&slot.state, AP_SLOT_INUSE);

    MOCKER(access_walk_pagemap_prepare).expects(once()).will(returnValue(-ENOMEM));
    MOCKER(queue_delayed_work).expects(never());
    MOCKER(complete).expects(once()).with(&ap.work_done);
    MOCKER(ap_put_slot).expects(once());
    access_work_func(&ap.scan_work.work);

    EXPECT_EQ(0, ap.cur_times);
    EXPECT_FALSE(ap.freq_ready);
}

TEST_F(AccessTrackingTest, scan_failure_stops_without_retry)
{
    MOCKER(scan_accessed_bit_forward_vm).expects(once()).will(returnValue(-EINVAL));
    MOCKER(scan_accessed_bit_forward_mm).expects(once()).will(returnValue(-ENOMEM));
    MOCKER(access_walk_pagemap_prepare).expects(never());
    MOCKER(queue_delayed_work).expects(never());
    MOCKER(complete).expects(exactly(2));
    MOCKER(ap_put_slot).expects(exactly(2));

    for (smap_pid_type pid_type : {SMAP_PID_VM, SMAP_PID_PROCESS}) {
        struct access_pid ap = {};
        struct ap_slot slot = {};
        ap.pid = 123;
        ap.pid_type = pid_type;
        ap.type = NORMAL_SCAN;
        ap.ntimes = 2;
        ap.slot = &slot;
        ap.freq_ready = true;
        atomic_set(&slot.state, AP_SLOT_INUSE);

        access_work_func(&ap.scan_work.work);

        EXPECT_EQ(0, ap.cur_times);
        EXPECT_FALSE(ap.freq_ready);
    }
}

TEST_F(AccessTrackingTest, scan_init)
{
    int ret;

    MOCKER(init_acpi_mem).stubs().will(returnValue(2));
    ret = scan_init();
    EXPECT_EQ(2, ret);

    GlobalMockObject::verify();
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(refresh_remote_ram).stubs().will(returnValue(3));
    MOCKER(reset_acpi_mem).stubs();
    ret = scan_init();
    EXPECT_EQ(3, ret);
}

extern "C" void release_remote_ram(void);
extern "C" int create_scan_workqueue(void);
extern "C" void access_print_acpi_mem(void);
TEST_F(AccessTrackingTest, scan_init_two)
{
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(refresh_remote_ram).stubs().will(returnValue(0));
    MOCKER(release_remote_ram).stubs();
    MOCKER(reset_acpi_mem).stubs();
    MOCKER(get_node_actc_len).stubs().will(returnValue((u64)1));
    MOCKER(get_node_page_cnt_iomem).stubs().will(returnValue((u64)1));

    struct ram_segment *seg = (struct ram_segment *)malloc(sizeof(struct ram_segment));
    INIT_LIST_HEAD(&seg->node);
    seg->numa_node = 1;
    seg->start = 1;
    seg->end = 10;
    list_add(&seg->node, &remote_ram_list);
    MOCKER(scan_ioctl_init).stubs().will(returnValue(0));
    MOCKER(create_scan_workqueue).stubs().will(returnValue(0));
    MOCKER(access_print_acpi_mem).stubs();
    int ret = scan_init();
    EXPECT_EQ(0, ret);
    list_del(&seg->node);
    free(seg);
}

extern "C" void release_remote_ram(void);
extern "C" void __exit scan_exit(void);
extern "C" void destroy_scan_workqueue(void);
TEST_F(AccessTrackingTest, scan_exit)
{
    MOCKER(destroy_scan_workqueue).stubs();
    MOCKER(release_remote_ram).stubs();
    MOCKER(reset_acpi_mem).stubs();
    scan_exit();
}

extern "C" ktime_t calc_time_us(ktime_t start_time);
TEST_F(AccessTrackingTest, calc_time_us)
{
    ktime_t ret = calc_time_us(0);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    ret = calc_time_us(-1000000);
    EXPECT_EQ(1000000 / 1000, ret);
}

extern "C" void cancel_ap_scan_work(struct access_pid *ap);
TEST_F(AccessTrackingTest, cancel_ap_scan_work_null)
{
    cancel_ap_scan_work(nullptr);
}

TEST_F(AccessTrackingTest, cancel_ap_scan_work_no_func)
{
    struct access_pid ap = {};
    ap.scan_work.work.func = nullptr;
    cancel_ap_scan_work(&ap);
}

TEST_F(AccessTrackingTest, cancel_ap_scan_work_with_func)
{
    struct access_pid ap = {};
    ap.scan_work.work.func = (void (*)(struct work_struct *))1;
    MOCKER(cancel_delayed_work_sync).stubs().will(returnValue(true));
    cancel_ap_scan_work(&ap);
    GlobalMockObject::verify();
}

extern "C" int set_scan_cpus(u32 cpu_start, u32 cpu_end);
TEST_F(AccessTrackingTest, set_scan_cpus_not_openeuler)
{
    int ret = set_scan_cpus(0, 4);
    EXPECT_EQ(0, ret);
}

extern "C" void submit_one_work(struct access_pid *ap);
TEST_F(AccessTrackingTest, submit_one_work)
{
    struct access_pid ap = {};
    ap.pid = 1;
    ap.scan_time = 100;

    MOCKER(cancel_ap_scan_work).stubs();
    MOCKER(queue_delayed_work).stubs().will(returnValue(true));
    submit_one_work(&ap);

    GlobalMockObject::verify();
}

extern "C" u8 remote_scan_mode;
extern "C" bool hist_inited;
extern "C" int nr_local_numa;
extern "C" int set_remote_scan_mode(u8 mode);
extern "C" bool is_remote_scan_hist_active(void);
extern "C" bool is_freq_hist_only_node(int node);
extern "C" int hist_module_init(void);
extern "C" int hist_tracking_set_page_size(u8 page_size_mode);
extern "C" int hist_tracking_disable(void);

TEST_F(AccessTrackingTest, set_remote_scan_mode_invalid)
{
    EXPECT_EQ(-EINVAL, set_remote_scan_mode(NR_REMOTE_SCAN_MODE));
}

TEST_F(AccessTrackingTest, set_remote_scan_mode_lazy_init_failed)
{
    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
    MOCKER(hist_module_init).stubs().will(returnValue(-ENODEV));

    EXPECT_EQ(-ENODEV, set_remote_scan_mode(SMAP_REMOTE_SCAN_HARD));
    EXPECT_EQ(false, hist_inited);
    EXPECT_EQ(SMAP_REMOTE_SCAN_COMBINED, remote_scan_mode);
}

TEST_F(AccessTrackingTest, set_remote_scan_mode_lazy_init_success)
{
    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_SOFT;
    MOCKER(hist_module_init).stubs().will(returnValue(0));
    MOCKER(hist_tracking_set_page_size).stubs().will(returnValue(0));

    EXPECT_EQ(0, set_remote_scan_mode(SMAP_REMOTE_SCAN_COMBINED));
    EXPECT_EQ(true, hist_inited);
    EXPECT_EQ(SMAP_REMOTE_SCAN_COMBINED, remote_scan_mode);

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
}

TEST_F(AccessTrackingTest, set_remote_scan_mode_soft)
{
    hist_inited = true;
    remote_scan_mode = SMAP_REMOTE_SCAN_HARD;
    MOCKER(hist_tracking_disable).stubs().will(returnValue(0));

    EXPECT_EQ(0, set_remote_scan_mode(SMAP_REMOTE_SCAN_SOFT));
    EXPECT_EQ(SMAP_REMOTE_SCAN_SOFT, remote_scan_mode);

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
}

TEST_F(AccessTrackingTest, is_remote_scan_hist_active)
{
    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_HARD;
    EXPECT_EQ(false, is_remote_scan_hist_active());

    hist_inited = true;
    EXPECT_EQ(true, is_remote_scan_hist_active());

    remote_scan_mode = SMAP_REMOTE_SCAN_SOFT;
    EXPECT_EQ(false, is_remote_scan_hist_active());

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
}

TEST_F(AccessTrackingTest, is_freq_hist_only_node)
{
    int savedNrLocalNuma = nr_local_numa;
    nr_local_numa = 4;

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_HARD;
    /* 硬件未初始化时不跳过软件累计 */
    EXPECT_EQ(false, is_freq_hist_only_node(4));

    hist_inited = true;
    EXPECT_EQ(true, is_freq_hist_only_node(4));
    EXPECT_EQ(false, is_freq_hist_only_node(3));
    EXPECT_EQ(false, is_freq_hist_only_node(SMAP_MAX_NUMNODES));

    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
    EXPECT_EQ(false, is_freq_hist_only_node(4));

    hist_inited = false;
    remote_scan_mode = SMAP_REMOTE_SCAN_COMBINED;
    nr_local_numa = savedNrLocalNuma;
}
