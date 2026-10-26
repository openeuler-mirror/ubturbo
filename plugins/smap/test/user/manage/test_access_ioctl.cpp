/*
 * Description: smap5.0 access ioctl ut code
 */

#include <linux/errno.h>
#include <sys/ioctl.h>
#include <cstdlib>

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/access_ioctl.h"
#include "manage/manage.h"

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
    struct AccessAddPidPayload payload = {};
    payload.scanTime = 200;
    payload.type = STATISTIC_SCAN;
    payload.duration = 1;
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
/* ---- GetNodeCapFromStatus：Mems_allowed 打印宽度解析获取 cap（方案 F） ---- */

/* 实机样例（openEuler 24.03 aarch64，CONFIG_NODES_SHIFT=8 → MAX_NUMNODES=256） */
extern "C" int32_t ParseNodeCapFromLine(const char *line, uint32_t *cap);
extern "C" int32_t GetNodeCapFromStatus(uint32_t *cap);
TEST_F(AccessIoctlTest, TestParseNodeCapNormal)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine(
        "Mems_allowed:\t00000000,00000000,00000000,00000000,00000000,00000000,00000000,0000140f\n", &cap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_NODE_CAP_LIMIT / 4, cap); /* 64 个 hex 字符 → 256 */
}

TEST_F(AccessIoctlTest, TestParseNodeCapNarrow)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Mems_allowed:\t0000000000000003\n", &cap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(64U, cap); /* 16 个 hex 字符 → 64 */
}

/* 值本身不参与解析：全 0 位图与全置位位图宽度相同，cap 相同 */
TEST_F(AccessIoctlTest, TestParseNodeCapValueIrrelevant)
{
    uint32_t capAllZero = 0;
    uint32_t capAllSet = 0;
    EXPECT_EQ(0, ParseNodeCapFromLine(
        "Mems_allowed:\t00000000,00000000,00000000,00000000,00000000,00000000,00000000,00000000\n", &capAllZero));
    EXPECT_EQ(0, ParseNodeCapFromLine(
        "Mems_allowed:\tffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff\n", &capAllSet));
    EXPECT_EQ(capAllZero, capAllSet);
}

TEST_F(AccessIoctlTest, TestParseNodeCapFieldNotFound)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Cpus_allowed:\t00000000,00000003\n", &cap);
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ((uint32_t)SMAP_NODE_CAP_LIMIT, cap); /* 失败即回退 */
}

/* Mems_allowed_list 是 %pbl 压缩区间列表，无宽度信息，不能误匹配 */
TEST_F(AccessIoctlTest, TestParseNodeCapListLineRejected)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Mems_allowed_list:\t0-3,10,12\n", &cap);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessIoctlTest, TestParseNodeCapBadChar)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Mems_allowed:\t00000000,0000000g\n", &cap);
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ((uint32_t)SMAP_NODE_CAP_LIMIT, cap);
}

TEST_F(AccessIoctlTest, TestParseNodeCapEmpty)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Mems_allowed:\t\n", &cap);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessIoctlTest, TestParseNodeCapNotPowerOfTwo)
{
    uint32_t cap = 0;
    int32_t ret = ParseNodeCapFromLine("Mems_allowed:\t000000000000\n", &cap); /* 12 hex → 48 */
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ((uint32_t)SMAP_NODE_CAP_LIMIT, cap);
}

TEST_F(AccessIoctlTest, TestParseNodeCapExceedLimit)
{
    char line[512];
    uint32_t cap = 0;
    int n = snprintf(line, sizeof(line), "Mems_allowed:\t");
    for (int i = 0; i < 300; i++) { /* 300 hex → 1200 > SMAP_NODE_CAP_LIMIT */
        line[n++] = '0';
    }
    line[n] = '\0';
    int32_t ret = ParseNodeCapFromLine(line, &cap);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessIoctlTest, TestParseNodeCapNullArgs)
{
    uint32_t cap = 0;
    EXPECT_EQ(-EINVAL, ParseNodeCapFromLine(nullptr, &cap));
    EXPECT_EQ(-EINVAL, ParseNodeCapFromLine("Mems_allowed:\t00000003\n", nullptr));
}

/* 真机 /proc/self/status：UT 运行于 Linux 服务器，内核必提供该字段且宽度合法 */
TEST_F(AccessIoctlTest, TestGetNodeCapFromStatusRealFile)
{
    uint32_t cap = 0;
    int32_t ret = GetNodeCapFromStatus(&cap);
    EXPECT_EQ(0, ret);
    EXPECT_GE(cap, 4U);
    EXPECT_LE(cap, (uint32_t)SMAP_NODE_CAP_LIMIT);
    EXPECT_EQ(0U, cap & (cap - 1)); /* 2 的幂 */
}

TEST_F(AccessIoctlTest, TestGetNodeCapFromStatusNullArg)
{
    EXPECT_EQ(-EINVAL, GetNodeCapFromStatus(nullptr));
}
