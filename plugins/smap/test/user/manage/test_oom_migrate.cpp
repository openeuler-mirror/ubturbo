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

/* ============ MigratePidFromToL2 ============ */
extern "C" long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                              int flags);

static long MockSmapMovePagesAllOk(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                                   int flags)
{
    /* move_pages(2) 成功时 status[i] = 页面最终所在 nid（=目标 nodes[0]），并非 0 */
    for (unsigned long i = 0; i < count; i++) {
        status[i] = nodes[0];
    }
    return 0;
}

static long MockSmapMovePagesPartialFail(int pid, unsigned long count, const void **pages, const int *nodes,
                                         int *status, int flags)
{
    if (count >= 1) {
        status[0] = nodes[0]; /* 成功：迁达目标节点 */
    }
    for (unsigned long i = 1; i < count; i++) {
        status[i] = -ENOENT; /* 未映射，失败 */
    }
    return 0;
}

/* syscall 整体成功但逐页全失败（-ENOENT）：ok==0，必须返回非 0，不能返回 0 误导调用方。 */
static long MockSmapMovePagesAllPerPageFail(int pid, unsigned long count, const void **pages, const int *nodes,
                                             int *status, int flags)
{
    for (unsigned long i = 0; i < count; i++) {
        status[i] = -ENOENT;
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

/* 预算为 0：while(*pageBudget>0) 不进循环，无候选，返回 0（非错误）。OpenNumaMaps 仍调、fgets/move_pages 不调 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_ZeroBudget)
{
    pid_t pid = 123;
    static FILE fakeFile;
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).expects(never());
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(SmapMovePages).expects(never());

    uint64_t budget = 0;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, budget);
}

/* OpenNumaMaps 失败：返回 -ENODEV，不调 move_pages */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_OpenFail)
{
    pid_t pid = 123;
    MOCKER(OpenNumaMaps).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    MOCKER(SmapMovePages).expects(never());

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(-ENODEV, ret);
    EXPECT_EQ(100, budget);
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

/* syscall 成功但逐页全 -ENOENT：movedCnt==0，必须返回非 0（-ENOENT），不能返回 0 误导调用方。 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_AllPerPageFail)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N1=2 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).stubs().will(invoke(MockSmapMovePagesAllPerPageFail));

    uint64_t budget = 100;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(-ENOENT, ret); /* 有候选但全逐页失败，返回首个 errno */
    EXPECT_EQ(100, budget);  /* 零成功，预算不扣 */
}

/*
 * 单段本地页数 > MAX_MOVE_PAGES_BATCH：流式跨批迁移（512 + 余量），验证固定批数组边扫边迁。
 * 段 N1=600，budget=1000：批1 迁 512、批2 迁 88，共 600，预算剩 400。
 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_StreamMultiBatch)
{
    pid_t pid = 123;
    static FILE fakeFile;
    static char line[] = "80000000 N1=600 kernelpagesize_kB=4\n";
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(line)).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(SmapMovePages).expects(exactly(2)).will(invoke(MockSmapMovePagesAllOk));

    uint64_t budget = 1000;
    int ret = MigratePidFromToL2(pid, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(400, budget); /* 1000 - 600 = 400 */
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
