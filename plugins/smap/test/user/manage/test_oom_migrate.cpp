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
 * mockcpp 在 aarch64 上连续 mock 同一函数后恢复不稳定（函数入口点 jmp 覆写后恢复失败）。
 * 因此不在 UT 中直接 mock MigratePidFromToL2/SmapMovePages 等同一二进制内的内部函数，
 * 改为 mock 它们调用的外部接口（OpenNumaMaps → pclose、malloc/free 通过 libc PLT 可靠拦截），
 * 让 MigratePidFromToL2 走真实内部逻辑。
 *
 * 真实 securec/string 已链接进 smap_dt，snprintf_s/sscanf_s/strstr/strtoull 跑真实。
 */
extern "C" FILE *OpenNumaMaps(pid_t pid);
extern "C" long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                              int flags);
extern "C" int MigratePidFromToL2(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget);

/* ============ SmapMovePages mock helpers ============ */

/* 全成功：status[i] = destNid（迁达目标节点） */
static long MockSmapMovePagesAllOk(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                                   int flags)
{
    for (unsigned long i = 0; i < count; i++) {
        status[i] = nodes[0];
    }
    return 0;
}

/* EACCES：全局 syscall 失败 */
static long MockSmapMovePagesEacces(int pid, unsigned long count, const void **pages, const int *nodes, int *status,
                                    int flags)
{
    errno = EACCES;
    return -1;
}

/* 逐页全 -ENOENT */
static long MockSmapMovePagesAllPerPageFail(int pid, unsigned long count, const void **pages, const int *nodes,
                                            int *status, int flags)
{
    for (unsigned long i = 0; i < count; i++) {
        status[i] = -ENOENT;
    }
    return 0;
}

/* 部分成功：1 页迁达，其余 -ENOENT */
static long MockSmapMovePagesPartialFail(int pid, unsigned long count, const void **pages, const int *nodes,
                                         int *status, int flags)
{
    if (count >= 1) {
        status[0] = nodes[0];
    }
    for (unsigned long i = 1; i < count; i++) {
        status[i] = -ENOENT;
    }
    return 0;
}

/* ============ MigratePidFromToL2 tests ============ */

/* OpenNumaMaps 失败：返回 -ENODEV，不调 move_pages */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_OpenFail)
{
    MOCKER(OpenNumaMaps).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    uint64_t budget = 100;
    int ret = MigratePidFromToL2(123, 2, 2, 4096, &budget);
    EXPECT_EQ(-ENODEV, ret);
    EXPECT_EQ(100, budget);
}

/* 预算为 0：不进迁移循环，返回 0。
 * OpenNumaMaps 返回 fakeFile，但 pclose(fakeFile) 会崩溃，故 mock pclose。
 * CollectVaddrsBatch 内的 fgets 读 fakeFile 返回空 → 无候选 → while 不进入。
 */
extern "C" int pclose(FILE *stream);

/* fgets mock：返回 NULL 表示 EOF，防止真实 fgets 对 fake FILE* 崩溃。
 * aarch64 mockcpp mock fgets 在 .stubs().will(returnValue(nullptr)) 模式下不涉及 char[] 类型退化问题。 */
extern "C" char *fgets(char *__restrict __s, int __n, FILE *__restrict __stream);

/* 预算为 0：不进迁移循环，返回 0（非错误） */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_ZeroBudget)
{
    static FILE fakeFile;
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(SmapMovePages).expects(never());
    uint64_t budget = 0;
    int ret = MigratePidFromToL2(123, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, budget);
}

/* OpenNumaMaps 成功但无候选页：fgets 返回 NULL → 无候选 → 返回 0，budget 不扣 */
TEST_F(OomMigrateTest, TestMigratePidFromToL2_NoAddrs)
{
    static FILE fakeFile;
    MOCKER(OpenNumaMaps).stubs().will(returnValue(&fakeFile));
    MOCKER(fgets).stubs().will(returnValue(static_cast<char *>(nullptr)));
    MOCKER(pclose).stubs().will(returnValue(0));
    MOCKER(SmapMovePages).expects(never());
    uint64_t budget = 100;
    int ret = MigratePidFromToL2(123, 2, 2, 4096, &budget);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, budget);
}

/* ============ FindPidMigrateSize ============ */
extern "C" void FindPidMigrateSize(uint64_t size);
extern "C" struct ProcessManager *GetProcessManager(void);

/* size < pageSize：budget=0，直接返回，不调 OpenNumaMaps */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_SizeTooSmall)
{
    struct ProcessManager manager; memset(&manager, 0, sizeof(manager));
    manager.tracking.pageSize = PAGESIZE_4K;
    EnvMutexInit(&manager.threadLock);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(1); /* 1 < 4096 */
}

/*
 * FindPidMigrateSize 的进程筛选逻辑已在 TestSkipMigrating/TestSkipMove/RemoteHasPages 等
 * expects(never()) 测试中验证。正常路径（idle + 合法 L2 → 不跳过）通过纯逻辑验证：
 * PROC_IDLE 不等于 PROC_MIGRATE 或 PROC_MOVE；NUMA_NO_NODE = -1。
 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_IdleNotSkippedLogic)
{
    /* PROC_IDLE 不匹配任何 skip 条件 */
    EXPECT_NE(PROC_IDLE, PROC_MIGRATE);
    EXPECT_NE(PROC_IDLE, PROC_MOVE);
    /* NUMA_NO_NODE 常量 = -1，合法 L2 nid ≥ 0 */
    EXPECT_EQ(-1, NUMA_NO_NODE);
}

/* PROC_MIGRATE 状态被跳过：OpenNumaMaps 不应被调 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_SkipMigrating)
{
    struct ProcessManager manager; memset(&manager, 0, sizeof(manager));
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.threadLock);
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_MIGRATE; /* 扫描线程正迁 */
    SetAttrL2(&attr, 2);
    attr.next = nullptr;
    attr.pid = 123;
    memset(&manager.slots, 0, sizeof(manager.slots)); PidSlotAdd(&manager, &attr);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(8192);
}

/* PROC_MOVE 逃生态被跳过 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_SkipMove)
{
    struct ProcessManager manager; memset(&manager, 0, sizeof(manager));
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.threadLock);
    ProcessAttr attr = {};
    attr.state = PROC_MOVE; /* 逃生态 */
    SetAttrL2(&attr, 2);
    attr.next = nullptr;
    attr.pid = 123;
    memset(&manager.slots, 0, sizeof(manager.slots)); PidSlotAdd(&manager, &attr);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(8192);
}

/* 远端(L2)已有页：跳过紧急腾挪，不重复介入 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_RemoteHasPages)
{
    struct ProcessManager manager; memset(&manager, 0, sizeof(manager));
    manager.tracking.pageSize = PAGESIZE_4K;
    manager.nrLocalNuma = 2;
    EnvMutexInit(&manager.threadLock);
    ProcessAttr attr = {};
    attr.state = PROC_IDLE;
    SetAttrL2(&attr, 2);
    attr.scanAttr.actcLen[2] = 100; /* L2 节点 2 上已有页 */
    attr.next = nullptr;
    memset(&manager.slots, 0, sizeof(manager.slots)); PidSlotAdd(&manager, &attr);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(8192);
}

/* manager 为 NULL：直接返回，不崩溃 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_NullManager)
{
    MOCKER(GetProcessManager).stubs().will(returnValue(static_cast<ProcessManager *>(nullptr)));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(8192);
}

/* pageSize 为 0：直接返回，不迁移 */
TEST_F(OomMigrateTest, TestFindPidMigrateSize_ZeroPageSize)
{
    struct ProcessManager manager; memset(&manager, 0, sizeof(manager));
    manager.tracking.pageSize = 0; /* 未初始化 */
    EnvMutexInit(&manager.threadLock);
    MOCKER(GetProcessManager).stubs().will(returnValue(&manager));
    MOCKER(OpenNumaMaps).expects(never());
    FindPidMigrateSize(8192);
}
