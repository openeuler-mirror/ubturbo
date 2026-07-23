/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user oom migrate ut code (方案B: move_pages + numa_maps 段级本地过滤)
 * Create: 2024-10-25
 */

#include <errno.h>
#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "manage/oom_migrate.h"
#include "smap_env.h"
#include "strategy/migration.h"

using namespace std;

class OomMigrateTest : public ::testing::Test {
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

/*
 * 真实 securec/string 已链接进 smap_dt，snprintf_s/sscanf_s/strstr/strtoull 跑真实，
 * 仅 mock OpenNumaMaps/fgets/pclose/IsHugeMode 喂入 numa_maps 行。
 */
extern "C" FILE *OpenNumaMaps(pid_t pid);
extern "C" char *fgets(char *__restrict __s, int __n, FILE *__restrict __stream);
extern "C" int pclose(FILE *stream);

/* ============ CollectVaddrsFromNumaMaps ============ */
extern "C" int CollectVaddrsFromNumaMaps(pid_t pid, int nrLocalNuma, uint64_t pageSize, uint64_t maxPages,
                                         uint64_t **outAddrs, int *outCnt);

/* 本地过滤：只收集含 N0=/N1=(本地)的段，按段内本地页总数枚举候选 vaddr */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_Filter)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line1[] = "70000000 N2=10 kernelpagesize_kB=4\n";          /* 纯远端，无本地页，跳过 */
    static char line2[] = "80000000 N0=1 N1=3 N2=1 kernelpagesize_kB=4\n"; /* 本地页=1+3=4 */
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets)
        .stubs()
        .will(returnValue(line1))
        .then(returnValue(line2))
        .then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));

    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 4096, 100, &addrs, &cnt); /* nrLocalNuma=2 */
    EXPECT_EQ(0, ret);
    EXPECT_EQ(4, cnt); /* N0=1 + N1=3，多本地节点全部计入 */
    EXPECT_EQ(0x80000000UL, addrs[0]);
    EXPECT_EQ(0x80000000UL + 3 * 4096, addrs[3]);
    free(addrs);
}

/* 大页模式：IsHugeMode=true，非 huge 段被 IsNumaMapLineHuge 过滤 */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_HugeFilter)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line1[] = "80000000 N1=3 kernelpagesize_kB=4\n";    /* 非 huge，跳过 */
    static char line2[] = "90000000 N1=2 kernelpagesize_kB=2048\n"; /* huge */
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets)
        .stubs()
        .will(returnValue(line1))
        .then(returnValue(line2))
        .then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(true));

    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 2048 * 1024, 100, &addrs, &cnt);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(2, cnt);
    EXPECT_EQ(0x90000000UL, addrs[0]);
    free(addrs);
}

/* 多本地节点 count 求和：N0=3 + N1=4 → 枚举 7 个候选 vaddr */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_MultiLocalSum)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "00a00000 N0=3 N1=4 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));

    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 4096, 100, &addrs, &cnt);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(7, cnt); /* 3 + 4 */
    EXPECT_EQ(0xa00000UL, addrs[0]);
    EXPECT_EQ(0xa00000UL + 6 * 4096, addrs[6]);
    free(addrs);
}

/* 段内本地页数超过 maxPages：枚举在 maxPages 处截断 */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_MaxPagesCap)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "00b00000 N1=100 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));

    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 4096, 5, &addrs, &cnt); /* maxPages=5 */
    EXPECT_EQ(0, ret);
    EXPECT_EQ(5, cnt);
    free(addrs);
}

/* 全段无本地页：cnt=0，不迁移 */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_NoLocal)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "70000000 N2=10 N3=5 kernelpagesize_kB=4\n"; /* 仅远端 */
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));

    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 4096, 100, &addrs, &cnt);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, cnt);
    EXPECT_EQ(nullptr, addrs);
    free(addrs);
}

/* OpenNumaMaps 失败：返回 -ENODEV */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_OpenFail)
{
    pid_t pid = 123;
    MOCKER(OpenNumaMaps).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 2, 4096, 100, &addrs, &cnt);
    EXPECT_EQ(-ENODEV, ret);
    EXPECT_EQ(0, cnt);
}

/* nrLocalNuma <= 0：无本地节点，直接返回 0，不读 numa_maps */
TEST_F(OomMigrateTest, TestCollectVaddrsFromNumaMaps_ZeroNrLocalNuma)
{
    pid_t pid = 123;
    MOCKER(OpenNumaMaps).expects(never());
    uint64_t *addrs = NULL;
    int cnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, 0, 4096, 100, &addrs, &cnt);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, cnt);
}

/* ============ MigratePidFromToL2 ============ */
extern "C" long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                              int flags);

static long MockSmapMovePagesAllOk(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                                   int flags)
{
    for (unsigned long i = 0; i < count; i++) {
        status[i] = 0; /* 全部迁移成功 */
    }
    return 0;
}

static long MockSmapMovePagesPartialFail(int pid, unsigned long count, const void **pages, const int *nodes,
                                         int *status, int flags)
{
    if (count >= 1) {
        status[0] = 0; /* 成功 */
    }
    for (unsigned long i = 1; i < count; i++) {
        status[i] = -ENOENT; /* 未映射，失败 */
    }
    return 0;
}

/* 正常迁移：收集到候选地址，move_pages 全成功，预算扣减 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_Normal)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N1=2 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).stubs().will(invoke(MockSmapMovePagesAllOk));

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget); /* nrLocalNuma=2, destNid=2 */
    EXPECT_EQ(0, ret);
    EXPECT_EQ(98, budget); /* 迁了 2 页 */
}

/* 部分失败：第 1 页成功，其余 -ENOENT，预算只扣成功页 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_PartialFail)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N0=1 N1=2 kernelpagesize_kB=4\n"; /* 本地页=3 */
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).stubs().will(invoke(MockSmapMovePagesPartialFail));

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);     /* 有 1 页成功，不算全失败 */
    EXPECT_EQ(99, budget); /* 仅扣 1 页 */
}

/* 无候选地址：CollectVaddrs 返回 cnt=0，不调 SmapMovePages */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_NoAddrs)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N2=5 kernelpagesize_kB=4\n"; /* 无本地页 */
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).expects(never());

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, budget);
}

/* 预算为 0：直接返回，不读 numa_maps */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_ZeroBudget)
{
    pid_t pid = 123;
    MOCKER(OpenNumaMaps).expects(never());
    uint64_t budget = 0;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
}

/* EACCES：全局失败不写 status[]，预算不扣，返回 -EACCES。 */
static long MockSmapMovePagesEacces(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                                    int flags)
{
    errno = EACCES;
    return -1; /* 全局失败，不写 status[] */
}

TEST_F(OomMigrateTest, TestMigratePidFromToL2_EaccesNoRetry)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N1=2 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).expects(once()).will(invoke(MockSmapMovePagesEacces));

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(-EACCES, ret); /* ok==0，返回 -errno */
    EXPECT_EQ(100, budget);  /* 零成功，预算不扣 */
}

/* ============ FindPidMigrateSize ============ */
extern "C" void FindPidMigrateSize(uint64_t size);
extern "C" int MigratePidFromToL2(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget);
extern "C" int EnvMutexInit(EnvMutex *mutex);
extern "C" struct ProcessManager *GetProcessManager(void);

static int MockMigratePidDrainBudget(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget)
{
    *pageBudget = 0; /* 迁够即停 */
    return 0;
}

/* size < pageSize：budget=0，直接返回，不迁移 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_SizeTooSmall)
{
    struct ProcessManager manager = {};
    manager.tracking.pageSize = PAGESIZE_4K;
    EnvMutexInit(&manager.lock);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(MigratePidFromToL2).expects(never());
    FindPidMigrateSize(1); /* 1 < 4096 */
}

/* 正常：idle 进程 + 合法 L2 → 调一次 MigratePidFromToL2，预算耗尽即停（无需 L1） */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_Normal)
{
    struct ProcessManager manager = {};
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.lock);
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_IDLE;
    SetAttrL2(&attr, 2); /* 远端节点 2（内核 nid）；不依赖 L1 */
    attr.next = nullptr;
    manager.processes = &attr;
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(MigratePidFromToL2).expects(once()).will(invoke(MockMigratePidDrainBudget));
    FindPidMigrateSize(8192); /* budget=2 */
}

/* PROC_MIGRATE 状态被跳过，不双重迁移 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_SkipMigrating)
{
    struct ProcessManager manager = {};
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.lock);
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_MIGRATE; /* 扫描线程正迁 */
    SetAttrL2(&attr, 2);
    attr.next = nullptr;
    manager.processes = &attr;
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(MigratePidFromToL2).expects(never());
    FindPidMigrateSize(8192);
}

/* L2 无效（NUMA_NO_NODE）被跳过：无远端目标，不迁 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_L2Invalid)
{
    struct ProcessManager manager = {};
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.lock);
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_IDLE;
    /* 未设 L2 → GetAttrL2 返回 NUMA_NO_NODE */
    attr.next = nullptr;
    manager.processes = &attr;
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(MigratePidFromToL2).expects(never());
    FindPidMigrateSize(8192);
}

/* 远端(L2)已有页：跳过紧急腾挪，不重复介入 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_RemoteHasPages)
{
    struct ProcessManager manager = {};
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.lock);
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_IDLE;
    SetAttrL2(&attr, 2);
    attr.scanAttr.actcLen[2] = 100; /* L2 节点 2 上已有页 */
    attr.next = nullptr;
    manager.processes = &attr;
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(MigratePidFromToL2).expects(never());
    FindPidMigrateSize(8192);
}
