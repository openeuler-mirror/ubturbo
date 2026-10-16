/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 access ioctl ut code
 */

#include <cstdlib>
#include <sys/ioctl.h>
#include <linux/errno.h>

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "manage/access_ioctl.h"

using namespace std;

class AccessIoctlTest : public ::testing::Test {
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

extern "C" int open(const char *pathname, int flags);
TEST_F(AccessIoctlTest, TestAccessIoctlAddPid)
{
    int ret;
    int len = 0;
    struct AccessAddPidPayload payload;
    ret = AccessIoctlAddPid(len, &payload);
    EXPECT_EQ(-EINVAL, ret);
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(-1));
    len = 1;
    ret = AccessIoctlAddPid(len, &payload);
    EXPECT_EQ(-EBADF, ret);
}

TEST_F(AccessIoctlTest, TestAccessIoctlRemovePid)
{
    int ret;
    int len = 0;
    struct AccessRemovePidPayload payload;
    ret = AccessIoctlRemovePid(len, &payload);
    EXPECT_EQ(-EINVAL, ret);

    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(-1));
    len = 1;
    ret = AccessIoctlRemovePid(len, &payload);
    EXPECT_EQ(-EBADF, ret);
}

TEST_F(AccessIoctlTest, TestAccessIoctlRemoveAllPid)
{
    int ret;
    MOCKER(open).stubs().will(returnValue(-EPERM));
    ret = AccessIoctlRemoveAllPid();
    EXPECT_EQ(-EBADF, ret);
}

static int StubWalkIoctlFail(int fd, unsigned long cmd, void *arg)
{
    (void)fd;
    (void)cmd;
    (void)arg;
    errno = ENODEV;
    return -1;
}

TEST_F(AccessIoctlTest, TestAccessIoctlWalkPagemap)
{
    int ret;
    struct ProcessMemBitmap pmb = {.pid = 100};

    /* AccessIoctlWalkPagemap 直接上抛 -errno，manage_scan 依赖 -ESRCH 判断 */
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(invoke(StubWalkIoctlFail));
    ret = AccessIoctlWalkPagemap(100, &pmb);
    EXPECT_EQ(-ENODEV, ret);
}

extern "C" void IoctlSetScanCpuRange(uint32_t cpuMin, uint32_t cpuMax);
TEST_F(AccessIoctlTest, TestIoctlSetScanCpuRangeSuccess)
{
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(0));
    IoctlSetScanCpuRange(1, 3);
}

TEST_F(AccessIoctlTest, TestIoctlSetScanCpuRangeFailed)
{
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(-1));
    IoctlSetScanCpuRange(1, 3);
}
extern "C" void IoctlSetRemoteScanMode(uint32_t mode);
TEST_F(AccessIoctlTest, TestIoctlSetRemoteScanModeSuccess)
{
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(0));
    IoctlSetRemoteScanMode(1);
}

TEST_F(AccessIoctlTest, TestIoctlSetRemoteScanModeFailed)
{
    MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(returnValue(-1));
    IoctlSetRemoteScanMode(2);
}
