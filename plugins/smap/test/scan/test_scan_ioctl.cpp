/*
 * Description: smap scan ioctl module
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "access_pid.h"
#include "acpi_mem.h"
#include "check.h"
#include "iomem.h"
#include "scan_ioctl.h"
#include "scan_main.h"

#include <cstring>

using namespace std;

/*
 * 方案B：内核 struct access_add_pid_payload 的 numa_nodes 已由定长数组改为柔性数组，
 * 单条记录字节数由 access_add_pid_msg.stride 给出（= 定长头 + nr_words * sizeof(unsigned long)）。
 * 测试按 stride 构造记录缓冲，位图字数取编译期上界 NUMA_NODES_BIT_WORDS。
 */
static constexpr u32 kAddPidStride =
    (u32)(sizeof(struct access_add_pid_payload) + NUMA_NODES_BIT_WORDS * sizeof(unsigned long));

static inline struct access_add_pid_payload *AddPidAt(struct access_add_pid_payload *base, int i)
{
    return access_add_pid_payload_at(base, i, kAddPidStride);
}

extern "C" struct access_pid_struct ap_data;

/* Reset the slot table to all-FREE (replaces INIT_LIST_HEAD(&ap_data.list)). */
static void ap_test_reset_slots(void)
{
    int i;

    for (i = 0; i < AP_MAX_SLOTS; i++) {
        atomic_set(&ap_data.slots[i].state, AP_SLOT_FREE);
        ap_data.slots[i].pid = 0;
        ap_data.slots[i].ap = nullptr;
        refcount_set(&ap_data.slots[i].refs, 0);
    }
}

class ScanIoctlTestKernel : public ::testing::Test {
public:
    static struct access_add_pid_msg m_msg;

protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        ap_test_reset_slots();
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }

    long checkIoctlAddPid(struct access_add_pid_msg msg);
    static unsigned long mockCopyFromUserSetMsg(void *to, const void *from, unsigned long n);
    static unsigned long mockCopyFromUserSetPayload(void *to, const void *from, unsigned long n);
    static bool checkCopyFromUserIsMsg(const void *from);
    static bool checkCopyFromUserIsPayload(const void *from);
};

struct access_add_pid_msg ScanIoctlTestKernel::m_msg = {.count = 0, .payload = NULL};

extern "C" long ioctl_add_pid(void __user *argp);
extern "C" int add_payload(int len, struct access_add_pid_payload *payload, u32 stride, int page_size);
extern "C" int check_msg_validity(struct access_add_pid_msg *msg);

unsigned long ScanIoctlTestKernel::mockCopyFromUserSetMsg(void *to, const void *from, unsigned long n)
{
    cout << "mockCopyFromUserSetMsg" << endl;
    struct access_add_pid_msg *tmpMsg = (struct access_add_pid_msg *)to;
    *tmpMsg = m_msg;
    return 0UL;
};

unsigned long ScanIoctlTestKernel::mockCopyFromUserSetPayload(void *to, const void *from, unsigned long n)
{
    cout << "mockCopyFromUserSetPayload" << endl;
    struct access_add_pid_payload *tmpPayload = (struct access_add_pid_payload *)to;
    if (n != (unsigned long)m_msg.count * kAddPidStride) {
        return n;
    }
    for (int i = 0; i < m_msg.count; ++i) {
        memcpy(AddPidAt(tmpPayload, i), AddPidAt(m_msg.payload, i), kAddPidStride);
    }
    return 0UL;
};

bool ScanIoctlTestKernel::checkCopyFromUserIsMsg(const void *from)
{
    return from == &m_msg;
}

bool ScanIoctlTestKernel::checkCopyFromUserIsPayload(const void *from)
{
    return from == m_msg.payload;
}

long ScanIoctlTestKernel::checkIoctlAddPid(struct access_add_pid_msg msg)
{
    long ret = 0;
    m_msg = msg;
    MOCKER(copy_from_user)
        .stubs()
        .with(any(), checkWith(checkCopyFromUserIsMsg))
        .will(invoke(ScanIoctlTestKernel::mockCopyFromUserSetMsg));
    MOCKER(copy_from_user)
        .stubs()
        .with(any(), checkWith(checkCopyFromUserIsPayload))
        .will(invoke(ScanIoctlTestKernel::mockCopyFromUserSetPayload));
    ret = ioctl_add_pid(&m_msg);
    return ret;
}

TEST_F(ScanIoctlTestKernel, IoctlAddPidWithOnePid)
{
    int i;
    long ret;
    struct access_add_pid_msg msg;

    msg.count = 1;
    msg.stride = kAddPidStride;
    msg.payload = (struct access_add_pid_payload *)vzalloc((size_t)msg.count * msg.stride);
    ASSERT_NE(nullptr, msg.payload);
    for (i = 0; i < msg.count; i++) {
        struct access_add_pid_payload *p = AddPidAt(msg.payload, i);
        p->nr_words = NUMA_NODES_BIT_WORDS;
        p->pid = 14587;
        p->numa_nodes[0] = 0x10;
        p->scan_time = 50;
        p->ntimes = 40;
        p->duration = 1;
        p->type = HAM_SCAN;
    }

    // success case
    MOCKER(add_payload).stubs().will(returnValue(0));
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(0, ret);

    // add_payload failed case
    GlobalMockObject::verify();
    MOCKER(add_payload).stubs().will(returnValue(-EINVAL));
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(-EINVAL, ret);

    // following tests will not run add_payload, since don't need add_payload mock
    // invalid scan_type case
    AddPidAt(msg.payload, 0)->type = (scan_type) - 1;
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(-EINVAL, ret);

    AddPidAt(msg.payload, 0)->type = MAX_SCAN_TYPE;
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(-EINVAL, ret);

    if (msg.payload) {
        vfree(msg.payload);
    }
}

TEST_F(ScanIoctlTestKernel, IoctlAddPidWithMultiPid)
{
    int i;
    long ret;
    struct access_add_pid_msg msg;

    msg.count = 15;
    msg.stride = kAddPidStride;
    msg.payload = (struct access_add_pid_payload *)vzalloc((size_t)msg.count * msg.stride);
    ASSERT_NE(nullptr, msg.payload);
    for (i = 0; i < msg.count; i++) {
        struct access_add_pid_payload *p = AddPidAt(msg.payload, i);
        p->nr_words = NUMA_NODES_BIT_WORDS;
        p->pid = 14587 + i;
        p->numa_nodes[0] = 0x10;
        p->duration = 1;
        if (i % 2) {
            p->scan_time = 50;
            p->ntimes = 40;
            p->type = HAM_SCAN;
        } else {
            p->scan_time = 10;
            p->ntimes = 200;
            p->type = STATISTIC_SCAN;
        }
    }
    // set add_payload mock
    MOCKER(add_payload).stubs().will(returnValue(0)).then(returnValue(-EINVAL));

    // success case
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(0, ret);

    // failed case
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(-EINVAL, ret);

    // following tests will not run add_payload, since don't need add_payload mock
    // invalid scan_type case
    AddPidAt(msg.payload, 14)->type = (scan_type)-1;
    ret = checkIoctlAddPid(msg);
    EXPECT_EQ(-EINVAL, ret);

    if (msg.payload)
        vfree(msg.payload);
}

TEST_F(ScanIoctlTestKernel, AddPayloadTest)
{
    int len = 1;
    alignas(8) unsigned long payload_storage[4 + NUMA_NODES_BIT_WORDS] = {};
    struct access_add_pid_payload *payload =
        reinterpret_cast<struct access_add_pid_payload *>(payload_storage);
    payload->nr_words = NUMA_NODES_BIT_WORDS;
    MOCKER(access_add_ham_pid).stubs()
        .will(returnValue(-EINVAL))
        .then(returnValue(0));
    MOCKER(access_add_statistic_pid).stubs()
        .will(returnValue(-EINVAL))
        .then(returnValue(0));
    MOCKER(access_add_pid).stubs()
        .will(returnValue(-EINVAL))
        .then(returnValue(0));

    // test all case
    EXPECT_EQ(-EINVAL, add_payload(len, payload, kAddPidStride, PMD_SIZE));
    EXPECT_EQ(-EINVAL, add_payload(len, payload, kAddPidStride, PMD_SIZE));
    EXPECT_EQ(-EINVAL, add_payload(len, payload, kAddPidStride, PMD_SIZE));
    EXPECT_EQ(0, add_payload(len, payload, kAddPidStride, PMD_SIZE));
}

TEST_F(ScanIoctlTestKernel, CheckMsgValidityTest)
{
    struct access_add_pid_msg msg;
    alignas(8) unsigned long payload_storage[4 + NUMA_NODES_BIT_WORDS] = {};
    struct access_add_pid_payload *payload =
        reinterpret_cast<struct access_add_pid_payload *>(payload_storage);
    msg.count = 1;
    msg.stride = kAddPidStride;
    msg.payload = payload;
    // success case
    EXPECT_EQ(0, check_msg_validity(&msg));
    // failed case
    msg.count = 0;
    EXPECT_EQ(-EINVAL, check_msg_validity(&msg));

    msg.count = 301;
    EXPECT_EQ(-EINVAL, check_msg_validity(&msg));

    msg.payload = nullptr;
    EXPECT_EQ(-EINVAL, check_msg_validity(&msg));

    EXPECT_EQ(-EINVAL, check_msg_validity(nullptr));
}

static struct access_add_pid_msg *g_test_msg = nullptr;
static struct access_add_pid_payload *g_test_payload = nullptr;
static size_t g_test_payload_size = 0;

static unsigned long mock_copy_from_user(void *to, const void *from, unsigned long n)
{
    if (n == sizeof(struct access_add_pid_msg) && g_test_msg) {
        *(struct access_add_pid_msg *)to = *g_test_msg;
    } else if (n == g_test_payload_size && g_test_payload) {
        for (size_t i = 0; i < n / kAddPidStride; i++) {
            memcpy(AddPidAt((struct access_add_pid_payload *)to, (int)i),
                   AddPidAt(g_test_payload, (int)i), kAddPidStride);
        }
    }
    return 0UL;
}

TEST_F(ScanIoctlTestKernel, IoctlAddPid)
{
    struct access_add_pid_msg msg;
    struct access_add_pid_payload *payload_data = nullptr;
    int i;
    int ret = 0;

    msg.count = 1;
    msg.stride = kAddPidStride;
    payload_data = (struct access_add_pid_payload *)vzalloc((size_t)msg.count * msg.stride);
    ASSERT_NE(nullptr, payload_data);
    msg.payload = payload_data;
    ASSERT_NE(nullptr, msg.payload);
    for (i = 0; i < msg.count; i++) {
        struct access_add_pid_payload *p = AddPidAt(payload_data, i);
        p->nr_words = NUMA_NODES_BIT_WORDS;
        p->pid = i;
        p->numa_nodes[0] = 0x10;
        p->ntimes = 40;
        /* check_payload_validity 要求 scan_time 在 (0, MAX_SCAN_PERIOD] 内 */
        p->scan_time = 100;
    }

    g_test_msg = &msg;
    g_test_payload = payload_data;
    g_test_payload_size = (size_t)msg.count * msg.stride;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user));

    MOCKER(access_add_ham_pid).stubs().will(returnValue(0));
    MOCKER(access_add_pid).stubs().will(returnValue(0));
    ret = ioctl_add_pid(NULL);
    EXPECT_EQ(0, ret);

    vfree(payload_data);

    g_test_msg = nullptr;
    g_test_payload = nullptr;
    g_test_payload_size = 0;
}

TEST_F(ScanIoctlTestKernel, IoctlAddPidTwo)
{
    struct access_add_pid_msg msg;
    int i;
    int ret = 0;

    msg.count = 1;
    msg.stride = kAddPidStride;
    msg.payload = (struct access_add_pid_payload *)vzalloc((size_t)msg.count * msg.stride);
    ASSERT_NE(nullptr, msg.payload);
    for (i = 0; i < msg.count; i++) {
        struct access_add_pid_payload *p = AddPidAt(msg.payload, i);
        p->nr_words = NUMA_NODES_BIT_WORDS;
        p->pid = i;
        p->numa_nodes[0] = 0x10;
    }

    MOCKER(copy_from_user)
        .stubs()
        .with(outBoundP((void *)&msg, sizeof(msg)))
        .will(returnValue(0UL))
        .then(returnValue(1UL));

    ret = ioctl_add_pid(NULL); // or ioctl_add_pid(msg)
    EXPECT_EQ(-EFAULT, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlAddPidThree)
{
    struct access_add_pid_msg msg;
    struct access_add_pid_payload *payload_data = nullptr;
    int i;
    int ret = 0;

    // 测试 1
    msg.count = 0;
    msg.payload = nullptr;
    g_test_msg = &msg;
    g_test_payload = nullptr;
    g_test_payload_size = 0;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user));

    ret = ioctl_add_pid(NULL);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    GlobalMockObject::reset();

    msg.count = 1;
    msg.stride = kAddPidStride;
    payload_data = (struct access_add_pid_payload *)vzalloc((size_t)msg.count * msg.stride);
    ASSERT_NE(nullptr, payload_data);
    msg.payload = payload_data;

    for (i = 0; i < msg.count; i++) {
        struct access_add_pid_payload *p = AddPidAt(payload_data, i);
        p->nr_words = NUMA_NODES_BIT_WORDS;
        p->pid = i;
        p->numa_nodes[0] = 0x10;
        p->ntimes = 40;
        p->type = HAM_SCAN;
        p->duration = 0;
        /* check_payload_validity 要求 scan_time 在 (0, MAX_SCAN_PERIOD] 内 */
        p->scan_time = 100;
    }

    g_test_msg = &msg;
    g_test_payload = payload_data;
    g_test_payload_size = (size_t)msg.count * msg.stride;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user));

    MOCKER(access_add_ham_pid).stubs().will(returnValue(1));

    ret = ioctl_add_pid(NULL);
    EXPECT_EQ(1, ret);

    // scan_time == 0 should be rejected
    payload_data[0].scan_time = 0;
    ret = ioctl_add_pid(NULL);
    EXPECT_EQ(-EINVAL, ret);

    vfree(payload_data);

    g_test_msg = nullptr;
    g_test_payload = nullptr;
    g_test_payload_size = 0;
}

extern "C" long ioctl_remove_pid(void __user *argp);
TEST_F(ScanIoctlTestKernel, IoctlRemovePid)
{
    int ret = 0;
    struct access_remove_pid_msg msg;

    MOCKER(copy_from_user).stubs().will(returnValue(1UL));
    ret = ioctl_remove_pid(NULL);
    EXPECT_EQ(-EFAULT, ret);

    GlobalMockObject::verify();
    msg.count = 0;
    MOCKER(copy_from_user).stubs().with(outBoundP((void *)&msg, sizeof(msg))).will(returnValue(0UL));
    ret = ioctl_remove_pid(NULL);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlRemovePidTwo)
{
    struct access_remove_pid_msg msg;
    struct access_remove_pid_payload payload = {};

    msg.count = 1;
    msg.payload = &payload;
    msg.payload[0].pid = 1;
    MOCKER(copy_from_user).stubs().with(outBoundP((void *)&msg, sizeof(msg))).will(returnValue(0UL));
    MOCKER(access_remove_pid).stubs();
    MOCKER(access_remove_ham_pid).stubs();
    int ret = ioctl_remove_pid(NULL);
    EXPECT_EQ(0, ret);
}

extern "C" long ioctl_remove_all_pid(void __user *argp);
TEST_F(ScanIoctlTestKernel, IoctlRemoveAllPid)
{
    int ret = 0;

    MOCKER(access_remove_all_pid).stubs();
    ret = ioctl_remove_all_pid(NULL);
    EXPECT_EQ(0, ret);
}

extern "C" long ioctl_walk_pagemap(void __user *argp);
static pid_t g_stubWalkPid;

static unsigned long StubCopyFromUserPidPages(void *to, const void *from, unsigned long n)
{
    (void)from;
    if (n == sizeof(struct smap_pid_page_num)) {
        struct smap_pid_page_num *dst = (struct smap_pid_page_num *)to;

        dst->pid = g_stubWalkPid;
        memset(dst->page_num, 0, sizeof(dst->page_num));
    }
    return 0;
}

static unsigned long StubCopyToUserPidPages(void *to, const void *from, unsigned long n)
{
    if (n == sizeof(struct smap_pid_page_num)) {
        memcpy(to, from, n);
    }
    return 0;
}

TEST_F(ScanIoctlTestKernel, IoctlWalkPagemap)
{
    int ret = 0;
    struct access_pid walk_ap = {};
    struct smap_pid_page_num param = {};

    ap_test_reset_slots();
    walk_ap.pid = 4321;
    walk_ap.type = NORMAL_SCAN;
    walk_ap.page_num[0] = 7;
    ap_slot_add(&walk_ap);

    /* 输入拷贝失败 */
    MOCKER(copy_from_user).stubs().will(returnValue(1UL));
    ret = ioctl_walk_pagemap(&param);
    EXPECT_EQ(-EFAULT, ret);

    /* 未跟踪pid返回ESRCH */
    GlobalMockObject::verify();
    g_stubWalkPid = 1234;
    MOCKER(copy_from_user).stubs().will(invoke(StubCopyFromUserPidPages));
    ret = ioctl_walk_pagemap(&param);
    EXPECT_EQ(-ESRCH, ret);

    /* 命中跟踪pid：回填page_num */
    GlobalMockObject::verify();
    g_stubWalkPid = 4321;
    MOCKER(copy_from_user).stubs().will(invoke(StubCopyFromUserPidPages));
    MOCKER(copy_to_user).stubs().will(invoke(StubCopyToUserPidPages));
    ret = ioctl_walk_pagemap(&param);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(4321, param.pid);
    EXPECT_EQ(7UL, param.page_num[0]);
}

extern "C" void update_tracking_data(u16 *tracking_data, struct statistics_tracking_info *stat_info,
                                     struct tracking_info_payload *payload_info);
extern "C" long ioctl_get_tracking(void __user *argp);
TEST_F(ScanIoctlTestKernel, IoctlGetTrackingError)
{
    int ret = 0;
    // copy error case
    struct tracking_info_payload msg = {
        .pid = 100,
        .length = 10,
        .data = NULL,
    };
    MOCKER(copy_from_user).stubs().will(returnValue(1UL));
    ret = ioctl_get_tracking(NULL);
    EXPECT_EQ(-EFAULT, ret);

    // invalid length case
    GlobalMockObject::verify();
    msg.length = 65536;
    MOCKER(copy_from_user).stubs().with(outBoundP((void *)&msg, sizeof(msg))).will(returnValue(0UL));
    ret = ioctl_get_tracking(NULL);
    EXPECT_EQ(-EINVAL, ret);

    // null data case
    GlobalMockObject::verify();
    msg.length = 1;
    MOCKER(copy_from_user).stubs().with(outBoundP((void *)&msg, sizeof(msg))).will(returnValue(0UL));
    ret = ioctl_get_tracking(NULL);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlGetTracking)
{
    int ret = 0;
    struct tracking_info_payload msg;
    u16 *tracking_data = NULL;
    struct statistics_tracking_info *tmp = NULL;

    msg.pid = 1;
    msg.length = 5;
    msg.data = (u16 *)malloc(sizeof(u16) * msg.length);
    ASSERT_NE(nullptr, msg.data);
    tracking_data = (u16 *)vzalloc(sizeof(u16) * msg.length);
    ASSERT_NE(nullptr, tracking_data);

    // 模拟统计PID列表中的数据
    tmp = (struct statistics_tracking_info *)kzalloc(sizeof(struct statistics_tracking_info), GFP_KERNEL);
    tmp->pid = 1;
    tmp->page_num[L1] = 3;
    tmp->page_num[L2] = 2;
    tmp->window_num = 2;
    tmp->sliding_windows = (u8 **)malloc(sizeof(u8 *) * tmp->window_num);
    for (int i = 0; i < tmp->window_num; i++) {
        tmp->sliding_windows[i] = (u8 *)malloc(sizeof(u8) * msg.length);
        for (int j = 0; j < msg.length; j++) {
            tmp->sliding_windows[i][j] = j + 1; // [1, 2, 3 ... msg.length+1]
        }
    }
    list_add(&tmp->node, &statistic_pid_list);
    MOCKER(copy_from_user).stubs().with(outBoundP((void *)&msg, sizeof(msg))).will(returnValue(0UL));
    MOCKER(copy_to_user).stubs().will(returnValue(0UL));

    ret = ioctl_get_tracking(NULL);
    EXPECT_EQ(0, ret);

    // 验证msg.length是否正确
    EXPECT_EQ(5, msg.length);

    // 验证update_tracking_data
    update_tracking_data(tracking_data, tmp, &msg);
    for (int i = 0; i < msg.length; ++i) {
        u16 expect = tmp->window_num * (i + 1); // sliding_windows数值叠加
        EXPECT_EQ(expect, tracking_data[i]);
    }

    // 清理模拟数据
    list_del(&tmp->node);
    for (int i = 0; i < tmp->window_num; i++) {
        free(tmp->sliding_windows[i]);
    }
    free(tmp->sliding_windows);
    free(tmp);
    free(msg.data);
    free(tracking_data);
}

extern "C" int smap_scan_open(struct inode *inode, struct file *file);
TEST_F(ScanIoctlTestKernel, SmapAccessOpen)
{
    int ret = 0;

    ret = smap_scan_open(NULL, NULL);
    EXPECT_EQ(0, ret);
}

extern "C" int smap_scan_release(struct inode *inode, struct file *file);
TEST_F(ScanIoctlTestKernel, SmapScanRelease)
{
    int ret = 0;

    ret = smap_scan_release(NULL, NULL);
    EXPECT_EQ(0, ret);
}

extern "C" long smap_scan_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
TEST_F(ScanIoctlTestKernel, SmapScanIoctl)
{
    long ret = smap_scan_ioctl(NULL, 0, NULL);
    EXPECT_EQ(-22, ret);

    MOCKER(ioctl_add_pid).stubs().will(returnValue(1));
    ret = smap_scan_ioctl(NULL, SMAP_ACCESS_ADD_PID, NULL);
    EXPECT_EQ(1, ret);
}

TEST_F(ScanIoctlTestKernel, SmapScanIoctlTwo)
{
    long ret;

    MOCKER(ioctl_remove_pid).stubs().will(returnValue(2));
    ret = smap_scan_ioctl(NULL, SMAP_ACCESS_REMOVE_PID, NULL);
    EXPECT_EQ(2, ret);

    MOCKER(ioctl_remove_all_pid).stubs().will(returnValue(3));
    ret = smap_scan_ioctl(NULL, SMAP_ACCESS_REMOVE_ALL_PID, NULL);
    EXPECT_EQ(3, ret);
}

TEST_F(ScanIoctlTestKernel, SmapScanIoctlThree)
{
    long ret;

    MOCKER(ioctl_walk_pagemap).stubs().will(returnValue(4));
    ret = smap_scan_ioctl(NULL, SMAP_ACCESS_WALK_PAGEMAP, NULL);
    EXPECT_EQ(4, ret);

    ret = smap_scan_ioctl(NULL, SMAP_ACCESS_GET_TRACKING + 1, NULL);
    EXPECT_EQ(-ENOTTY, ret);
}

extern "C" void scan_dev_exit(void);
TEST_F(ScanIoctlTestKernel, ScanDevExit)
{
    scan_dev_exit();
}

extern "C" int scan_dev_init(void);
TEST_F(ScanIoctlTestKernel, ScanDevInit)
{
    int ret;
    MOCKER(alloc_chrdev_region).stubs().will(returnValue(-1));
    ret = scan_dev_init();
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    MOCKER(alloc_chrdev_region).stubs().will(returnValue(0));
    MOCKER(cdev_init).stubs().will(returnValue(0));
    MOCKER(cdev_add).stubs().will(returnValue(-1));
    ret = scan_dev_init();
    EXPECT_EQ(-1, ret);
}

extern "C" bool IS_ERR(const void *ptr);
extern "C" long PTR_ERR(const void *ptr);
TEST_F(ScanIoctlTestKernel, ScanDevInitTwo)
{
    int ret;

    MOCKER(alloc_chrdev_region).stubs().will(returnValue(0));
    MOCKER(cdev_init).stubs().will(returnValue(0));
    MOCKER(cdev_add).stubs().will(returnValue(0));
    MOCKER(IS_ERR).stubs().will(returnValue(false)).then(returnValue(true));
    MOCKER(PTR_ERR).stubs().will(returnValue(-2));
    ret = scan_dev_init();
    EXPECT_EQ(-2, ret);
}

extern "C" void access_remove_all_pid(void);
extern "C" void scan_ioctl_exit(void);
TEST_F(ScanIoctlTestKernel, ScanIoctlExit)
{
    MOCKER(access_remove_all_pid).stubs().will(ignoreReturnValue());
    MOCKER(scan_dev_exit).stubs().will(ignoreReturnValue());
    scan_ioctl_exit();
}

extern "C" int scan_ioctl_init(void);
TEST_F(ScanIoctlTestKernel, ScanIoctlInit)
{
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    int ret = scan_ioctl_init();
    EXPECT_EQ(ret, 0);
}

extern "C" long ioctl_set_scan_cpu(void __user *argp);
extern "C" int set_scan_cpus(u32 cpu_start, u32 cpu_end);

static unsigned long mockCopyFromUserSetScanCpuRange(void *to, const void *from, unsigned long n)
{
    struct smap_scan_cpu_range *dst = (struct smap_scan_cpu_range *)to;
    dst->cpu_min = 0;
    dst->cpu_max = 1;
    return 0;
}

static unsigned long mockCopyFromUserSetScanCpuRangeInvalid(void *to, const void *from, unsigned long n)
{
    return n;
}

static unsigned long mockCopyFromUserSetScanCpuRangeMinOverMax(void *to, const void *from, unsigned long n)
{
    struct smap_scan_cpu_range *dst = (struct smap_scan_cpu_range *)to;
    dst->cpu_min = 5;
    dst->cpu_max = 3;
    return 0;
}

static unsigned long mockCopyFromUserSetScanCpuRangeMaxOverNumCpus(void *to, const void *from, unsigned long n)
{
    struct smap_scan_cpu_range *dst = (struct smap_scan_cpu_range *)to;
    dst->cpu_min = 0;
    dst->cpu_max = 100;
    return 0;
}

TEST_F(ScanIoctlTestKernel, IoctlSetScanCpuCopyFromUserFailed)
{
    MOCKER(copy_from_user).stubs().will(invoke(mockCopyFromUserSetScanCpuRangeInvalid));
    long ret = ioctl_set_scan_cpu(nullptr);
    EXPECT_EQ(-EFAULT, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlSetScanCpuMinOverMax)
{
    MOCKER(copy_from_user).stubs().will(invoke(mockCopyFromUserSetScanCpuRangeMinOverMax));
    long ret = ioctl_set_scan_cpu(nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlSetScanCpuMaxOverNumCpus)
{
    MOCKER(copy_from_user).stubs().will(invoke(mockCopyFromUserSetScanCpuRangeMaxOverNumCpus));
    long ret = ioctl_set_scan_cpu(nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlSetScanCpuSetScanCpusFailed)
{
    MOCKER(copy_from_user).stubs().will(invoke(mockCopyFromUserSetScanCpuRange));
    MOCKER(set_scan_cpus).stubs().will(returnValue(-EINVAL));
    long ret = ioctl_set_scan_cpu(nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ScanIoctlTestKernel, IoctlSetScanCpuSuccess)
{
    MOCKER(copy_from_user).stubs().will(invoke(mockCopyFromUserSetScanCpuRange));
    MOCKER(set_scan_cpus).stubs().will(returnValue(0));
    long ret = ioctl_set_scan_cpu(nullptr);
    EXPECT_EQ(0, ret);
}

extern "C" int access_tracking_restart_pid(pid_t pid);
TEST_F(ScanIoctlTestKernel, IoctlRestartPidNoent)
{
    ap_test_reset_slots();
    EXPECT_EQ(-ENOENT, access_tracking_restart_pid(8899));
}
