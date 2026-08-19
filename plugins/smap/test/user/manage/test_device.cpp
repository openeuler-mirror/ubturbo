/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user device ut code
 */

#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dirent.h>

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "manage/device.h"
#include "securec.h"


using namespace std;

static struct dirent MakeDirent(const char *name)
{
    struct dirent entry = {};

    (void)snprintf(entry.d_name, sizeof(entry.d_name), "%s", name);
    return entry;
}


class DeviceTest : public ::testing::Test {
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
int ConfigTrackingDev(int *trackingFds, uint32_t pageSize);
int EnableTracking(struct ProcessManager *manager);
int OpenAndFlockFd(int fd);
}
TEST_F(DeviceTest, TestIInitTrackingDevOne)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    int ret;

    MOCKER(reinterpret_cast<int (*)(const char *, int)>(open)).stubs().will(returnValue(-1));
    ret = InitTrackingDev(&pm);
    EXPECT_EQ(-ENODEV, ret);
}

TEST_F(DeviceTest, TestIInitTrackingDevTwo)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    int ret;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0))
        .then(returnValue(-1));
    MOCKER(OpenAndFlockFd).stubs().will(returnValue(0));
    MOCKER(reinterpret_cast<int (*)(const char *, int)>(open)).stubs().will(returnValue(0));
    ret = InitTrackingDev(&pm);
    EXPECT_EQ(-EINVAL, ret);
    GlobalMockObject::verify();

    MOCKER(OpenAndFlockFd).stubs().will(returnValue(0));
    MOCKER(reinterpret_cast<int (*)(const char *, int)>(open)).stubs().will(returnValue(0));
    MOCKER((int (*)(const char *, int))access).stubs().will(returnValue(-1));
    errno = 0;
    ret = InitTrackingDev(&pm);
    EXPECT_EQ(-ENODEV, ret);
}

extern "C" int ConfigureTrackingDevices(struct ProcessManager *manager);
TEST_F(DeviceTest, TestIInitTrackingDevThree)
{
    int ret;
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(reinterpret_cast<int (*)(const char *, int)>(open)).stubs().will(returnValue(0));
    MOCKER((int (*)(const char *, int))access).stubs().will(returnValue(0));
    MOCKER(ConfigureTrackingDevices).stubs().will(returnValue(0));
    MOCKER(EnableTracking).stubs().will(returnValue(0));
    MOCKER(OpenAndFlockFd).stubs().will(returnValue(0));
    ret = InitTrackingDev(&pm);
    EXPECT_EQ(0, ret);
}

extern "C" int SendCmdToAllNodes(int fds[], unsigned long cmd, int arg);
TEST_F(DeviceTest, TestEnableTracking)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    int ret;

    MOCKER(SendCmdToAllNodes).stubs().will(returnValue(0));
    ret = EnableTracking(&pm);
    EXPECT_EQ(0, ret);
}

TEST_F(DeviceTest, TestDisableTracking)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    int ret;

    MOCKER(SendCmdToAllNodes).stubs().will(returnValue(0));
    ret = DisableTracking(&pm);
    EXPECT_EQ(0, ret);
}

TEST_F(DeviceTest, TestSendCmdToAllNodes)
{
    int fds[100] = {0};
    int ret;

    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl)).stubs().will(returnValue(-1));
    ret = SendCmdToAllNodes(fds, 0, 0);
    EXPECT_EQ(-EBADF, ret);
    GlobalMockObject::verify();

    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl)).stubs().will(returnValue(0));
    ret = SendCmdToAllNodes(fds, 0, 0);
    EXPECT_EQ(0, ret);
}

TEST_F(DeviceTest, TestSendCmdToAllNodesGroup)
{
    int fds[100] = {-1};
    int ret;

    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl)).stubs().will(returnValue(0));
    ret = SendCmdToAllNodes(fds, 0, 0);
    EXPECT_EQ(0, ret);
}

TEST_F(DeviceTest, TestConfigTrackingDev)
{
    int fds[100] = {0};
    int ret;

    MOCKER(SendCmdToAllNodes).stubs().will(returnValue(0));
    ret = ConfigTrackingDev(fds, PAGESIZE_2M);
    EXPECT_EQ(0, ret);

    ret = ConfigTrackingDev(fds, PAGESIZE_4K);
    EXPECT_EQ(0, ret);
}

extern "C" int DisableTracking(struct ProcessManager *manager);
TEST_F(DeviceTest, TestDeinitTrackingDev)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    pm.fds.nodes[0] = 1; pm.fds.nodes[1] = 2; pm.fds.migrate = 3; pm.fds.access = 4;
    for (int i = 2; i < MAX_NODES; i++) {
        pm.fds.nodes[i] = DEFAULT_FD;
    }

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(static_cast<int (*)(int)>(close)).expects(exactly(4)).will(ignoreReturnValue());
    DeinitTrackingDev(&pm);
    EXPECT_EQ(DEFAULT_FD, pm.fds.nodes[0]);
    EXPECT_EQ(DEFAULT_FD, pm.fds.nodes[1]);
    EXPECT_EQ(DEFAULT_FD, pm.fds.migrate);
    EXPECT_EQ(DEFAULT_FD, pm.fds.access);
}

extern "C" int FindFdByNode(int fds[], int fdsLength);
TEST_F(DeviceTest, TestFindFdByNode)
{
    int fds[1] = {-5};
    int fdsLength = 1;
    int ret = FindFdByNode(fds, fdsLength);
    EXPECT_EQ(-EINVAL, ret);

    fds[0] = 0;
    ret = FindFdByNode(fds, fdsLength);
    EXPECT_EQ(0, ret);
}

extern "C" bool IsLocalNuma(unsigned long nid);
TEST_F(DeviceTest, TestIsLocalNumaExceedLocalNumaNum)
{
    bool ret = IsLocalNuma(LOCAL_NUMA_NUM);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsLocalNumaBuildFailed)
{
    MOCKER((int (*)(char *, size_t, size_t, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    bool ret = IsLocalNuma(0);
    EXPECT_FALSE(ret);
}

extern "C" FILE *fopen(const char *filename, const char *modes);
TEST_F(DeviceTest, TestIsLocalNumaOpenFailed)
{
    MOCKER((int (*)(char *, size_t, size_t, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue((FILE *)nullptr));
    bool ret = IsLocalNuma(0);
    EXPECT_FALSE(ret);
}

extern "C" int fgetc(FILE *stream);
extern "C" int fclose(FILE *stream);
TEST_F(DeviceTest, TestIsLocalNumaReadFailed)
{
    FILE tmpFile;

    MOCKER((int (*)(char *, size_t, size_t, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue(EOF));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsLocalNuma(0);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsLocalNumaReadRemote)
{
    FILE tmpFile;
    int remoteValue = '1';

    MOCKER((int (*)(char *, size_t, size_t, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue(remoteValue));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsLocalNuma(0);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsLocalNumaReadLocal)
{
    FILE tmpFile;
    int localValue = '0';

    MOCKER((int (*)(char *, size_t, size_t, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue(localValue));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsLocalNuma(0);
    EXPECT_TRUE(ret);
}

extern "C" int GetNrLocalNumaFromKernel(struct ProcessManager *manager);
TEST_F(DeviceTest, TestConfigureTrackingDevicesSetNrLocalNumaFailed)
{
    struct ProcessManager manager;
    MOCKER(GetNrLocalNumaFromKernel).stubs().will(returnValue(-1));
    int ret = ConfigureTrackingDevices(&manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(DeviceTest, TestConfigureTrackingDevicesConfigTrackingDevFailed)
{
    struct ProcessManager manager;
    MOCKER(GetNrLocalNumaFromKernel).stubs().will(returnValue(0));
    MOCKER(ConfigTrackingDev).stubs().will(returnValue(-1));
    int ret = ConfigureTrackingDevices(&manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(DeviceTest, TestConfigureTrackingDevicesSuccess)
{
    struct ProcessManager manager;
    MOCKER(GetNrLocalNumaFromKernel).stubs().will(returnValue(0));
    MOCKER(ConfigTrackingDev).stubs().will(returnValue(0));
    int ret = ConfigureTrackingDevices(&manager);
    EXPECT_EQ(0, ret);
}

extern "C" bool IsNumaCriticalErr(int nid);
TEST_F(DeviceTest, TestIsNumaCriticalErrPathBuildFailed)
{
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsNumaCriticalErrFileNotFound)
{
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue((FILE *)nullptr));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsNumaCriticalErrFileEmpty)
{
    FILE tmpFile;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue(EOF));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsNumaCriticalErrNotCritical)
{
    FILE tmpFile;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue('0'));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_FALSE(ret);
}

TEST_F(DeviceTest, TestIsNumaCriticalErrIsCritical)
{
    FILE tmpFile;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue('1'));
    MOCKER(fclose).stubs().will(returnValue(0));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_TRUE(ret);
}

TEST_F(DeviceTest, TestIsNumaCriticalErrCloseFailed)
{
    FILE tmpFile;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&tmpFile));
    MOCKER(fgetc).stubs().will(returnValue('1'));
    MOCKER(fclose).stubs().will(returnValue(1));
    bool ret = IsNumaCriticalErr(4);
    EXPECT_TRUE(ret);
}

extern "C" void GetUbFluxMb(void);
TEST_F(DeviceTest, TestGetUbFluxMbAllNodesFail)
{
    struct ProcessManager *pm = GetProcessManager();
    int savedFds[MAX_NODES];
    for (int i = 0; i < MAX_NODES; i++) {
        savedFds[i] = pm->fds.nodes[i];
        pm->fds.nodes[i] = -1;
    }
    pm->ubBwMonitor.ubBwThreshold = 500;

    GetUbFluxMb();
    EXPECT_NE(0, pm->ubBwMonitor.currentFluxRet);

    for (int i = 0; i < MAX_NODES; i++) {
        pm->fds.nodes[i] = savedFds[i];
    }
}

TEST_F(DeviceTest, TestGetUbFluxMbSuccess)
{
    struct ProcessManager *pm = GetProcessManager();
    int savedFd = pm->fds.nodes[LOCAL_NUMA_NUM];
    pm->fds.nodes[LOCAL_NUMA_NUM] = 10;
    pm->ubBwMonitor.ubBwThreshold = 500;

    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl))
        .stubs()
        .will(returnValue(0));

    GetUbFluxMb();
    EXPECT_EQ(0, pm->ubBwMonitor.currentFluxRet);

    pm->fds.nodes[LOCAL_NUMA_NUM] = savedFd;
}

extern "C" int ConfigUbWatch(uint32_t durationMs);
TEST_F(DeviceTest, TestConfigUbWatchAllNodesFail)
{
    struct ProcessManager *pm = GetProcessManager();
    int savedFds[MAX_NODES];
    for (int i = 0; i < MAX_NODES; i++) {
        savedFds[i] = pm->fds.nodes[i];
        pm->fds.nodes[i] = -1;
    }

    int ret = ConfigUbWatch(2000);
    EXPECT_EQ(-ENODEV, ret);

    for (int i = 0; i < MAX_NODES; i++) {
        pm->fds.nodes[i] = savedFds[i];
    }
}

TEST_F(DeviceTest, TestConfigUbWatchSuccess)
{
    struct ProcessManager *pm = GetProcessManager();
    int savedFd = pm->fds.nodes[LOCAL_NUMA_NUM];
    pm->fds.nodes[LOCAL_NUMA_NUM] = 10;

    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl))
        .stubs()
        .will(returnValue(0));

    int ret = ConfigUbWatch(2000);
    EXPECT_EQ(0, ret);

    pm->fds.nodes[LOCAL_NUMA_NUM] = savedFd;
}
