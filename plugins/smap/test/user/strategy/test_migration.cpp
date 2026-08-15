/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user migration ut code
 */

#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <sys/ioctl.h>

#include "advanced-strategy/scene.h"
#include "advanced-strategy/scene_info.h"
#include "manage/device.h"
#include "manage/manage.h"
#include "manage/thread.h"
#include "strategy/migration.h"
#include "strategy/strategy.h"
#include "strategy/strategy_config.h"

using namespace std;

#define TEST_SMAP_MIG_MAGIC 0xB9
#define TEST_SMAP_MIG_MIGRATE _IOW(TEST_SMAP_MIG_MAGIC, 0, struct MigrateMsg)
#define BIT(i) (1U << (i))

extern "C" struct ProcessManager g_processManager;

class MigrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        g_processManager.processes = nullptr;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        g_processManager.processes = nullptr;
        cout << "[Phase TearDown End]" << endl;
    }
};

static void InitSingleGroupedSwapProcess(ProcessAttr *process, pid_t pid, uint64_t usedPages)
{
    process->pid = pid;
    process->state = PROC_MIGRATE;
    process->groupPolicy.enabled = true;
    process->groupPolicy.groupCount = 1;
    process->groupPolicy.groups[0].localCount = 1;
    process->groupPolicy.groups[0].locals[0].nid = 0;
    process->groupPolicy.groups[0].targetCount = 1;
    process->groupPolicy.groups[0].targets[0].nid = 4;
    process->groupPolicy.groups[0].targets[0].quotaPages = 100;
    process->groupPolicy.groups[0].targets[0].usedPages = usedPages;
}

static void InitUnbalancedGroupedSwapResult(struct MigrateMsg *mMsg, pid_t pid)
{
    mMsg->cnt = 2;
    mMsg->migList = (struct MigList *)calloc(2, sizeof(struct MigList));
    mMsg->migList[0].pid = pid;
    mMsg->migList[0].from = 0;
    mMsg->migList[0].to = 4;
    mMsg->migList[0].nr = 10;
    mMsg->migList[0].failedMigNr = 2;
    mMsg->migList[0].successToUser = true;

    mMsg->migList[1].pid = pid;
    mMsg->migList[1].from = 4;
    mMsg->migList[1].to = 0;
    mMsg->migList[1].nr = 10;
    mMsg->migList[1].failedMigNr = 0;
    mMsg->migList[1].successToUser = true;
}

static int MockGroupedSwapCompIoctlSuccess(int fd, unsigned long request, void *arg)
{
    (void)fd;
    struct MigrateMsg *msg = (struct MigrateMsg *)arg;
    EXPECT_EQ((unsigned long)TEST_SMAP_MIG_MIGRATE, request);
    EXPECT_NE(nullptr, msg);
    if (msg == nullptr) {
        return -EINVAL;
    }
    EXPECT_EQ(1, msg->cnt);
    if (msg->cnt <= 0) {
        return -EINVAL;
    }
    EXPECT_EQ(123, msg->migList[0].pid);
    EXPECT_EQ(0, msg->migList[0].from);
    EXPECT_EQ(4, msg->migList[0].to);
    EXPECT_EQ((uint64_t)2, msg->migList[0].nr);
    msg->migList[0].successToUser = true;
    msg->migList[0].failedMigNr = 0;
    msg->migList[0].failedIsolatedNr = 0;
    return 0;
}

static void CheckNoEmptyCompEntryUpdateResult(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    (void)manager;
    EXPECT_NE(nullptr, mMsg);
    if (mMsg == nullptr) {
        return;
    }
    EXPECT_EQ(2, mMsg->cnt);
}

TEST_F(MigrationTest, TestAddMigListAddMultiSuccess)
{
    int i;
    int ret;
    struct MigrateMsg mMsg = {.cnt = 0};
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    struct MigList mList = {.nr = 2, .from = 0, .to = 1};

    mList.addr = (uint64_t *)malloc(sizeof(uint64_t) * mList.nr);
    for (i = 0; i < mList.nr; i++) {
        mList.addr[i] = i;
    }

    MOCKER(IsHugeMode).stubs().will(returnValue(true));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)3));
    ret = AddMigList(&mMsg, &mList);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, mMsg.cnt);
    EXPECT_EQ(2, mMsg.migList[0].nr);

    free(mList.addr);
    free(mMsg.migList[0].addr);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestAddMigListAddMultiAndMoreThanFreePagesSuccess)
{
    int i;
    int ret;
    struct MigrateMsg mMsg = {.cnt = 0};
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    struct MigList mList = {.nr = 2, .from = 0, .to = 1};

    mList.addr = (uint64_t *)malloc(sizeof(uint64_t) * mList.nr);
    for (i = 0; i < mList.nr; i++) {
        mList.addr[i] = i;
    }

    MOCKER(IsHugeMode).stubs().will(returnValue(true));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)1));
    ret = AddMigList(&mMsg, &mList);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, mMsg.cnt);
    EXPECT_EQ(2, mMsg.migList[0].nr);

    free(mList.addr);
    free(mMsg.migList[0].addr);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestAddMigListNoPageToMig)
{
    int i;
    int ret;
    struct MigrateMsg mMsg = {.cnt = 0};
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    struct MigList mList = {.nr = 0, .from = 0, .to = 1};

    ret = AddMigList(&mMsg, &mList);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, mMsg.cnt);
}

TEST_F(MigrationTest, TestAddMigListRejectsKernelEntryOverflow)
{
    uint64_t addr = 0;
    struct MigList mList = {.nr = 1, .addr = &addr};
    struct MigrateMsg mMsg = {
        .cnt = MAX_2M_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT,
    };
    MOCKER(IsHugeMode).stubs().will(returnValue(true));

    EXPECT_EQ(-ENOSPC, AddMigList(&mMsg, &mList));
    EXPECT_EQ(MAX_2M_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT, mMsg.cnt);
}

extern "C" void FreeMigList(struct MigList mList[MAX_NODES][MAX_NODES]);
TEST_F(MigrationTest, TestFreeMigList)
{
    struct MigList mlist[MAX_NODES][MAX_NODES] = {0};
    mlist[0][0].addr = (uint64_t *)malloc(sizeof(uint64_t));
    FreeMigList(mlist);
    EXPECT_EQ(nullptr, mlist[0][0].addr);
}

extern "C" void StrategyInitMigList(struct MigList mList[MAX_NODES][MAX_NODES], int pid);
TEST_F(MigrationTest, TestInitMigList)
{
    struct MigList mlist[MAX_NODES][MAX_NODES] = {0};
    printf("MAX_NODES in test = %d\n", MAX_NODES);
    mlist[0][0].addr = (uint64_t *)malloc(sizeof(uint64_t));
    mlist[0][0].nr = 1;
    mlist[0][0].pid = 1234;
    mlist[0][0].from = 100;
    mlist[0][0].to = 200;

    StrategyInitMigList(mlist, 12345);

    EXPECT_EQ(0, mlist[0][0].nr);
    EXPECT_EQ(12345, mlist[0][0].pid);
    EXPECT_EQ(0, mlist[0][0].from);
    EXPECT_EQ(0, mlist[0][0].to);
    EXPECT_EQ(nullptr, mlist[0][0].addr);
}

extern "C" int BuildMigrationMsg(ProcessAttr *process, struct MigrateMsg *mMsg, uint64_t *migratePage);
extern "C" bool IsNodeForbidden(int nid);

TEST_F(MigrationTest, TestBuildMigrationMsgActcDataInvalid)
{
    int ret;
    uint64_t pages;
    ProcessAttr process = {};
    struct MigrateMsg mMsg = {};

    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(-ENODATA, ret);
}

TEST_F(MigrationTest, TestBuildMigrationMsgRunStrategyFail)
{
    int ret;
    int nid = 4;
    uint64_t pages;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;
    struct MigrateMsg mMsg;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[nid], 0);

    MOCKER(RunStrategy).stubs().will(returnValue(-ENOENT));
    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(-ENOENT, ret);
}

TEST_F(MigrationTest, TestBuildMigrationMsgNullPtrOfMigratePage)
{
    int ret;
    int nid = 4;
    uint64_t pages;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;
    struct MigrateMsg mMsg;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[nid], 0);

    MOCKER(RunStrategy).stubs().will(returnValue(0));
    ret = BuildMigrationMsg(&process, &mMsg, nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(MigrationTest, TestBuildMigrationMsgNoPage)
{
    int ret;
    int nid = 4;
    uint64_t pages;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;
    struct MigrateMsg mMsg;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[nid], 0);

    MOCKER(AddMigList).stubs().will(returnValue(0));
    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestBuildMigrationMsgAllowsNormalPromoteFromForbiddenNode)
{
    int ret;
    int nid = 4;
    uint64_t pages = 0;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    struct MigrateMsg mMsg;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[nid], 1);
    MOCKER(RunStrategy).stubs().will(returnValue(0));

    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    process.groupPolicy.enabled = true;
    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(-EPERM, ret);
    EnvAtomicSet(&g_forbiddenNodes[nid], 0);
}

extern "C" int RunStrategyStub(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES], size_t mlistSize);

static int g_addMigListOrder[2];
static int g_addMigListCount;

static int BuildMigrationMsgDirectionLists(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                           size_t mlistSize)
{
    (void)process;
    (void)mlistSize;
    mlist[0][1].nr = 2;
    mlist[1][0].nr = 1;
    return 0;
}

static int RecordMigListOrder(struct MigrateMsg *mMsg, struct MigList *mList)
{
    (void)mMsg;
    if (g_addMigListCount < 2) {
        g_addMigListOrder[g_addMigListCount] = mList->from;
    }
    g_addMigListCount++;
    return 0;
}

TEST_F(MigrationTest, TestBuildMigrationMsgKeepsMatrixOrder)
{
    ProcessAttr process = {};
    struct MigrateMsg mMsg = {};
    uint64_t pages = 0;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;
    g_addMigListCount = 0;

    MOCKER(CheckActcDataValid).stubs().will(returnValue(0));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    MOCKER(RunStrategy).stubs().will(invoke(BuildMigrationMsgDirectionLists));
    MOCKER(AddMigList).stubs().will(invoke(RecordMigListOrder));

    ASSERT_EQ(0, BuildMigrationMsg(&process, &mMsg, &pages));
    EXPECT_EQ(3U, pages);
    EXPECT_EQ(2, g_addMigListCount);
    EXPECT_EQ(1, g_addMigListOrder[0]);
    EXPECT_EQ(0, g_addMigListOrder[1]);
}

TEST_F(MigrationTest, TestBuildMigrationMsgSuccess)
{
    int ret;
    uint64_t pages = 0;
    ProcessAttr process = {};
    process.pid = 1234;
    process.strategyAttr.nrMigratePages[0][0] = 100;
    process.numaAttr.numaNodes = 0b00010001;
    struct MigrateMsg mMsg = {};
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;
    EnvAtomicSet(&g_forbiddenNodes[4], 0);

    MOCKER(CheckActcDataValid).stubs().will(returnValue(0));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(RunStrategy).stubs().will(invoke(RunStrategyStub));
    MOCKER(AddMigList).stubs().will(returnValue(0));

    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(144, pages);
}

extern "C" int DoMigration(struct MigrateMsg *mMsg, struct ProcessManager *manager);
TEST_F(MigrationTest, TestDoMigration)
{
    struct MigList migList = {.nr = 0};
    struct MigrateMsg mMsg = {.cnt = 1, .migList = &migList};

    struct ProcessManager manager;
    int ret = DoMigration(&mMsg, &manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(MigrationTest, TestDoMigrationInitialized)
{
    struct MigList migList = {.nr = 2};

    migList.addr = (uint64_t *)malloc(sizeof(uint64_t) * migList.nr);

    for (int i = 0; i < migList.nr; ++i) {
        migList.addr[i] = 0x1000 + i * 0x1000;
    }

    struct MigrateMsg mMsg = {.cnt = 1, .migList = &migList};

    struct ProcessManager manager;
    int ret = DoMigration(&mMsg, &manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(MigrationTest, DoMigrationMinusCnt)
{
    struct MigList migList = {.nr = 0};
    struct MigrateMsg mMsg = {.cnt = -1, .migList = &migList};

    struct ProcessManager manager;
    int ret = DoMigration(&mMsg, &manager);
    EXPECT_EQ(-ENOMEM, ret);
}

extern "C" int InitMigrateMsg(struct MigrateMsg *mMsg, struct ProcessManager *manager);
TEST_F(MigrationTest, TestInitMigrateMsg)
{
    struct MigrateMsg mMsg = {.cnt = 1};
    struct ProcessManager manager = {.nr = {0, 1}, .tracking = {.pageSize = 4096}};
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    ASSERT_EQ(nullptr, mMsg.migList);
    int ret = InitMigrateMsg(&mMsg, &manager);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, mMsg.cnt);
    EXPECT_NE(nullptr, mMsg.migList);
    free(mMsg.migList);
}

extern "C" int PerformMigrationPreparation(struct ProcessManager *manager);
extern "C" int BuildAllPidData(void);
extern "C" int CleanStrategyAttribute(struct ProcessManager *manager);
TEST_F(MigrationTest, TestPerformMigrationPreparationOK)
{
    int ret;
    ProcessManager *manager = (ProcessManager *)malloc(sizeof(ProcessManager));
    if (!manager) {
        return;
    }
    manager->processes = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    if (!manager->processes) {
        free(manager);
        return;
    }
    manager->processes->next = NULL;

    MOCKER(BuildAllPidData).stubs().will(returnValue(0));
    MOCKER(CleanStrategyAttribute).stubs().will(returnValue(0));
    ret = PerformMigrationPreparation(manager);
    EXPECT_EQ(0, ret);

    free(manager->processes);
    free(manager);
}

TEST_F(MigrationTest, TestPerformMigrationPreparationEmptyProcesses)
{
    int ret;
    struct ProcessManager manager = {.processes = nullptr};

    MOCKER(CleanStrategyAttribute).stubs().will(returnValue(0));
    MOCKER(BuildAllPidData).stubs().will(returnValue(0));
    ret = PerformMigrationPreparation(&manager);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(MigrationTest, TestPerformMigrationPreparationBuildError)
{
    int ret;
    ProcessAttr process;
    struct ProcessManager manager = {.processes = &process};

    MOCKER(CleanStrategyAttribute).stubs().will(returnValue(0));
    MOCKER(BuildAllPidData).stubs().will(returnValue(-ENOMEM));
    ret = PerformMigrationPreparation(&manager);
    EXPECT_EQ(-ENOMEM, ret);
}

extern "C" int ScanMigrateWork(struct ProcessManager *manager);
extern "C" int PerformMigration(struct ProcessManager *manager);
extern "C" int HandleScene(struct ProcessManager *manager);
extern "C" void UpdateScene(struct ProcessManager *manager);
extern "C" void UpdatePeriodFromConfig(struct ProcessManager *manager);
extern "C" int CollectNodeFreeSnapshot(bool hugePage, int nrLocalNuma, PairPlanContext *context);
extern "C" int BuildPairPlans(const PairPlan inputs[], size_t inputCnt, PairPlanContext *context,
                              PairPidBudget pidBudgets[], size_t budgetCnt, PairPlan plans[],
                              size_t planMaxCnt, size_t *planCnt);
extern "C" int BuildPairSwapPlans(struct ProcessManager *manager, PairPlan plans[], size_t planCnt,
                                  PairPlanContext *context, PairPidBudget budgets[],
                                  size_t budgetCnt);
extern "C" int ApplyPairPlans(struct ProcessManager *manager, const PairPlan plans[], size_t planCnt);
extern "C" int ApplyPairPlansForState(struct ProcessManager *manager, const PairPlan plans[], size_t planCnt);
extern "C" int BuildAllPairPlans(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt);
TEST_F(MigrationTest, TestScanMigrateWorkFileConfOn)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(StrategyConfigRead).stubs().will(ignoreReturnValue());
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(true));
    MOCKER(SetAdaptMem).expects(once()).will(ignoreReturnValue());
    MOCKER(GetAdaptiveRatioEnableConfig).stubs().will(returnValue(true));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(0));
    MOCKER(UpdateScene).stubs();
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdatePeriodFromConfig).stubs().will(ignoreReturnValue());
    MOCKER(PerformMigration).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkFileConfOff)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(StrategyConfigRead).stubs().will(ignoreReturnValue());
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(false));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(0));
    MOCKER(UpdateScene).stubs();
    MOCKER(HandleScene).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(PerformMigration).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkOne)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };

    MOCKER(DisableTracking).stubs().will(returnValue(-1));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkTwo)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(-1));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(-1, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkNoProcess)
{
    int ret;
    struct ProcessManager manager = {.processes = nullptr};

    // trackingEnabled=false时不应调用DisableTracking，直接返回0
    manager.tracking.trackingEnabled = false;
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkNoProcessDisableTracking)
{
    int ret;
    struct ProcessManager manager = {.processes = nullptr};

    // trackingEnabled=true时应调用一次DisableTracking并返回0
    manager.tracking.trackingEnabled = true;
    MOCKER(DisableTracking).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

extern "C" uint32_t g_pageSizeHuge;
extern "C" long CalcDurationUs(struct timeval start, struct timeval end);
TEST_F(MigrationTest, TestCalcDurationUs)
{
    struct timeval start = {0};
    struct timeval end = {0};
    start.tv_sec = 1;
    start.tv_usec = 100;
    end.tv_sec = 2;
    end.tv_usec = 100;

    long duration = CalcDurationUs(start, end);
    EXPECT_EQ(1000000, duration);
}

extern "C" void CalProcessNuma(StrategyAttribute *strategyAttr);
TEST_F(MigrationTest, TestCalProcessNuma)
{
    ProcessAttr attr = {};
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 100;
    attr.strategyAttr.allocRemoteNrPages[0][0] = 200;
    attr.strategyAttr.l3RemoteMemRatio[0][0] = 50;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    CalProcessNuma(&attr.strategyAttr);
    EXPECT_EQ(150, attr.strategyAttr.nrMigratePages[4][0]);
}

typedef struct {
    int numaId;
    int amount;
} NumaMemReduce;

extern "C" int CompareMigIn(const void *a, const void *b);
TEST_F(MigrationTest, TestCompareMigIn)
{
    NumaMemReduce *a = (NumaMemReduce *)malloc(sizeof(NumaMemReduce));
    NumaMemReduce *b = (NumaMemReduce *)malloc(sizeof(NumaMemReduce));
    a->amount = 100;
    b->amount = 200;

    int ret = CompareMigIn(a, b);
    EXPECT_EQ(-100, ret);
    free(a);
    free(b);
}

extern "C" int CompareMigOut(const void *b, const void *a);
TEST_F(MigrationTest, TestCompareMigOut)
{
    NumaMemReduce *a = (NumaMemReduce *)malloc(sizeof(NumaMemReduce));
    NumaMemReduce *b = (NumaMemReduce *)malloc(sizeof(NumaMemReduce));
    a->amount = 200;
    b->amount = 100;

    int ret = CompareMigIn(a, b);
    EXPECT_EQ(100, ret);
    free(a);
    free(b);
}

extern "C" void NumaSwapMemPool(ProcessAttr *current);
TEST_F(MigrationTest, TestNumaSwapMemPool)
{
    int l2Node = 4;
    ProcessAttr attr = {};
    attr.numaAttr.numaNodes = 0x11;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 5;
    attr.walkPage.nrPages[l2Node] = 5;
    attr.strategyAttr.memSize[0][0] = 4096; // 2 hugepage
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = g_pageSizeHuge;
    MOCKER(IsHugeMode).stubs().will(returnValue(true));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(l2Node));

    // promote case
    NumaSwapMemPool(&attr);
    EXPECT_EQ(3, attr.strategyAttr.nrMigratePages[l2Node][0]);

    // demote case
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 140;
    attr.walkPage.nrPages[0] = 200;
    attr.walkPage.nrPages[l2Node] = 140;
    attr.strategyAttr.memSize[0][0] = 307200; // 150 hugepage
    NumaSwapMemPool(&attr);
    EXPECT_EQ(10, attr.strategyAttr.nrMigratePages[0][l2Node]);
}

extern "C" void NumaMigReduceDeal(ProcessAttr *current);
TEST_F(MigrationTest, TestNumaMigReduceDeal)
{
    ProcessAttr attr = {};
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 100;
    attr.strategyAttr.allocRemoteNrPages[0][0] = 200;
    attr.strategyAttr.l3RemoteMemRatio[0][0] = 50;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(GetRunMode).stubs().will(returnValue(0));
    NumaMigReduceDeal(&attr);
    EXPECT_EQ(150, attr.strategyAttr.nrMigratePages[4][0]);
}

static PairPlan MakePairPlan(pid_t pid, int localNid, int remoteNid, int remoteIndex, uint32_t targetPages,
                             uint32_t actualPages)
{
    PairPlan plan = {};
    plan.pid = pid;
    plan.localNid = localNid;
    plan.remoteNid = remoteNid;
    plan.remoteIndex = remoteIndex;
    plan.targetPages = targetPages;
    plan.actualPages = actualPages;
    return plan;
}

static const PairPlan *FindPairPlan(const PairPlan plans[], size_t planCnt, pid_t pid, int localNid, int remoteNid)
{
    for (size_t i = 0; i < planCnt; i++) {
        if (plans[i].pid == pid && plans[i].localNid == localNid && plans[i].remoteNid == remoteNid) {
            return &plans[i];
        }
    }
    return nullptr;
}

TEST_F(MigrationTest, TestCollectNodeFreeSnapshotUsesBasePages)
{
    PairPlanContext context = {};
    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue((uint64_t)100));

    ASSERT_EQ(0, CollectNodeFreeSnapshot(false, 2, &context));
    EXPECT_EQ(2, context.nrLocalNuma);
    for (int nid = 0; nid < 2; nid++) {
        EXPECT_EQ(100U, context.freePages[nid]);
        EXPECT_EQ(5U, context.safetyReservePages[nid]);
        EXPECT_EQ(0U, context.plannedPages[nid]);
    }
    for (int nid = 2; nid < 2 + REMOTE_NUMA_NUM; nid++) {
        EXPECT_EQ(0U, context.freePages[nid]);
        EXPECT_EQ(0U, context.safetyReservePages[nid]);
        EXPECT_EQ(0U, context.plannedPages[nid]);
    }
}

TEST_F(MigrationTest, TestCollectNodeFreeSnapshotUsesHugePages)
{
    PairPlanContext context = {};
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)21));

    ASSERT_EQ(0, CollectNodeFreeSnapshot(true, 2, &context));
    for (int nid = 0; nid < 2; nid++) {
        EXPECT_EQ(21U, context.freePages[nid]);
        EXPECT_EQ(2U, context.safetyReservePages[nid]);
    }
    for (int nid = 2; nid < 2 + REMOTE_NUMA_NUM; nid++) {
        EXPECT_EQ(0U, context.freePages[nid]);
        EXPECT_EQ(0U, context.safetyReservePages[nid]);
    }
}

TEST_F(MigrationTest, TestBuildPairPlansBuildsOneNetDirection)
{
    PairPlan inputs[] = {
        MakePairPlan(300, 0, 2, 0, 40, 40),
        MakePairPlan(100, 0, 2, 0, 80, 20),
        MakePairPlan(200, 1, 3, 1, 0, 50),
    };
    PairPidBudget pidBudgets[] = {
        {.pid = 100, .maxMigratePages = 100},
        {.pid = 200, .maxMigratePages = 100},
        {.pid = 300, .maxMigratePages = 100},
    };
    PairPlanContext context = {.nrLocalNuma = 2};
    for (int nid = 0; nid < MAX_NODES; nid++) {
        context.freePages[nid] = 1000;
    }
    PairPlan plans[3] = {};
    size_t planCnt = 0;

    ASSERT_EQ(0, BuildPairPlans(inputs, 3, &context, pidBudgets, 3, plans, 3, &planCnt));
    ASSERT_EQ(3U, planCnt);
    const PairPlan *demote = FindPairPlan(plans, planCnt, 100, 0, 2);
    const PairPlan *promote = FindPairPlan(plans, planCnt, 200, 1, 3);
    const PairPlan *balanced = FindPairPlan(plans, planCnt, 300, 0, 2);
    ASSERT_NE(nullptr, demote);
    ASSERT_NE(nullptr, promote);
    ASSERT_NE(nullptr, balanced);
    EXPECT_EQ(60U, demote->demotePages);
    EXPECT_EQ(0U, demote->promotePages);
    EXPECT_EQ(0U, promote->demotePages);
    EXPECT_EQ(50U, promote->promotePages);
    EXPECT_EQ(0U, balanced->demotePages);
    EXPECT_EQ(0U, balanced->promotePages);
}

TEST_F(MigrationTest, TestBuildPairPlansAppliesFreeBudgetOnlyToPromote)
{
    PairPlan inputs[] = {
        MakePairPlan(400, 1, 2, 0, 100, 0),
        MakePairPlan(300, 0, 2, 0, 100, 0),
        MakePairPlan(200, 0, 2, 0, 0, 100),
        MakePairPlan(100, 0, 3, 1, 0, 100),
    };
    PairPidBudget pidBudgets[] = {
        {.pid = 400, .maxMigratePages = 100},
        {.pid = 300, .maxMigratePages = 100},
        {.pid = 200, .maxMigratePages = 100},
        {.pid = 100, .maxMigratePages = 100},
    };
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = 100;
    context.safetyReservePages[0] = 10;
    context.freePages[2] = 0;
    context.safetyReservePages[2] = 100;
    PairPlan plans[4] = {};
    size_t planCnt = 0;

    ASSERT_EQ(0, BuildPairPlans(inputs, 4, &context, pidBudgets, 4, plans, 4, &planCnt));
    ASSERT_EQ(4U, planCnt);
    EXPECT_EQ(100, plans[0].pid);
    EXPECT_EQ(90U, plans[0].promotePages);
    EXPECT_EQ(200, plans[1].pid);
    EXPECT_EQ(0U, plans[1].promotePages);
    EXPECT_EQ(300, plans[2].pid);
    EXPECT_EQ(100U, plans[2].demotePages);
    EXPECT_EQ(400, plans[3].pid);
    EXPECT_EQ(100U, plans[3].demotePages);
    EXPECT_EQ(90U, context.plannedPages[0]);
    EXPECT_EQ(0U, context.plannedPages[2]);
}

TEST_F(MigrationTest, TestBuildPairPlansSharesPidBudgetAcrossPairs)
{
    PairPlan inputs[] = {
        MakePairPlan(100, 0, 3, 1, 50, 0),
        MakePairPlan(100, 0, 2, 0, 50, 0),
    };
    PairPidBudget pidBudgets[] = {
        {.pid = 100, .maxMigratePages = 60},
    };
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[2] = 1000;
    context.freePages[3] = 1000;
    PairPlan plans[2] = {};
    size_t planCnt = 0;

    ASSERT_EQ(0, BuildPairPlans(inputs, 2, &context, pidBudgets, 1, plans, 2, &planCnt));
    ASSERT_EQ(2U, planCnt);
    EXPECT_EQ(2, plans[0].remoteNid);
    EXPECT_EQ(50U, plans[0].demotePages);
    EXPECT_EQ(3, plans[1].remoteNid);
    EXPECT_EQ(10U, plans[1].demotePages);
    EXPECT_EQ(60U, pidBudgets[0].plannedPages);
}

TEST_F(MigrationTest, TestBuildPairSwapPlansUsesRemainingBidirectionalBudget)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr process = {.pid = 100, .scanType = NORMAL_SCAN};
    ActcData localPages[10] = {};
    ActcData remotePages[10] = {};
    process.enableSwap = true;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcData[2] = remotePages;
    process.scanAttr.actcLen[0] = 10;
    process.scanAttr.actcLen[2] = 10;
    process.scanAttr.actCount[0].freqBuckets[0] = 10;
    process.scanAttr.actCount[2].freqBuckets[5] = 10;
    manager.processes = &process;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plan = MakePairPlan(100, 0, 2, 0, 10, 10);
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = 10;
    context.safetyReservePages[0] = 2;
    PairPidBudget budget = {.pid = 100, .maxMigratePages = 12, .plannedPages = 2};

    ASSERT_EQ(0, BuildPairSwapPlans(&manager, &plan, 1, &context, &budget, 1));
    EXPECT_EQ(5U, plan.swapPages);
    EXPECT_EQ(12U, budget.plannedPages);
    EXPECT_EQ(5U, context.plannedPages[0]);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

TEST_F(MigrationTest, TestBuildPairSwapPlansAllowsUnconvergedPair)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr process = {.pid = 100, .scanType = NORMAL_SCAN};
    ActcData localPages[10] = {};
    ActcData remotePages[10] = {};
    process.enableSwap = true;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcData[2] = remotePages;
    process.scanAttr.actcLen[0] = 10;
    process.scanAttr.actcLen[2] = 10;
    process.scanAttr.actCount[0].freqBuckets[0] = 10;
    process.scanAttr.actCount[2].freqBuckets[5] = 10;
    manager.processes = &process;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plan = MakePairPlan(100, 0, 2, 0, 10, 8);
    plan.demotePages = 2;
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = 10;
    context.safetyReservePages[0] = 2;
    PairPidBudget budget = {.pid = 100, .maxMigratePages = 12, .plannedPages = 2};

    ASSERT_EQ(0, BuildPairSwapPlans(&manager, &plan, 1, &context, &budget, 1));
    EXPECT_EQ(5U, plan.swapPages);
    EXPECT_EQ(12U, budget.plannedPages);
    EXPECT_EQ(5U, context.plannedPages[0]);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

TEST_F(MigrationTest, TestBuildPairSwapPlansFillsRemoteHotGuarantee)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr process = {.pid = 100, .scanType = NORMAL_SCAN};
    ActcData localPages[10] = {};
    ActcData remotePages[10] = {};
    process.enableSwap = true;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcData[2] = remotePages;
    process.scanAttr.actcLen[0] = 10;
    process.scanAttr.actcLen[2] = 10;
    process.scanAttr.actCount[0].freqBuckets[5] = 10;
    process.scanAttr.actCount[2].freqBuckets[5] = 10;
    process.scanAttr.actCount[2].remoteHotNum = 4;
    manager.processes = &process;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plan = MakePairPlan(100, 0, 2, 0, 10, 10);
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = 10;
    context.safetyReservePages[0] = 2;
    PairPidBudget budget = {.pid = 100, .maxMigratePages = 8};

    ASSERT_EQ(0, BuildPairSwapPlans(&manager, &plan, 1, &context, &budget, 1));
    EXPECT_EQ(4U, plan.swapPages);
    EXPECT_EQ(8U, budget.plannedPages);
    EXPECT_EQ(4U, context.plannedPages[0]);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

TEST_F(MigrationTest, TestBuildPairSwapPlansRejectsAmbiguousSharedRemote)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr process = {.pid = 100, .scanType = NORMAL_SCAN};
    ActcData pages[3][4] = {};
    process.enableSwap = true;
    for (int nid : {0, 1, 2}) {
        process.scanAttr.actcData[nid] = pages[nid];
        process.scanAttr.actcLen[nid] = 4;
    }
    process.scanAttr.actCount[0].freqBuckets[0] = 4;
    process.scanAttr.actCount[1].freqBuckets[0] = 4;
    process.scanAttr.actCount[2].freqBuckets[5] = 4;
    process.scanAttr.actCount[2].remoteHotNum = 4;
    manager.processes = &process;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plans[] = {
        MakePairPlan(100, 0, 2, 0, 4, 4),
        MakePairPlan(100, 1, 2, 0, 4, 4),
    };
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = context.freePages[1] = 100;
    PairPidBudget budget = {.pid = 100, .maxMigratePages = 100};

    ASSERT_EQ(0, BuildPairSwapPlans(&manager, plans, 2, &context, &budget, 1));
    EXPECT_EQ(0U, plans[0].swapPages);
    EXPECT_EQ(0U, plans[1].swapPages);
    EXPECT_EQ(0U, budget.plannedPages);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

TEST_F(MigrationTest, TestBuildPairPlansZeroLocalSafeFreeBlocksPromote)
{
    PairPlan input = MakePairPlan(100, 0, 2, 0, 20, 80);
    PairPidBudget pidBudget = {.pid = 100, .maxMigratePages = 100};
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[0] = 10;
    context.safetyReservePages[0] = 10;
    PairPlan plan = {};
    size_t planCnt = 0;

    ASSERT_EQ(0, BuildPairPlans(&input, 1, &context, &pidBudget, 1, &plan, 1, &planCnt));
    EXPECT_EQ(20U, plan.targetPages);
    EXPECT_EQ(80U, plan.actualPages);
    EXPECT_EQ(0U, plan.demotePages);
    EXPECT_EQ(0U, plan.promotePages);
}

TEST_F(MigrationTest, TestBuildPairPlansFailureDoesNotPublishPartialState)
{
    PairPlan inputs[] = {
        MakePairPlan(100, 0, 2, 0, 50, 0),
        MakePairPlan(100, 0, 2, 0, 60, 0),
    };
    PairPidBudget pidBudget = {.pid = 100, .maxMigratePages = 100};
    PairPlanContext context = {.nrLocalNuma = 2};
    context.freePages[2] = 100;
    PairPlan plans[2] = {};
    plans[0].pid = 999;
    size_t planCnt = 7;

    EXPECT_EQ(-EINVAL, BuildPairPlans(inputs, 2, &context, &pidBudget, 1, plans, 2, &planCnt));
    EXPECT_EQ(0U, planCnt);
    EXPECT_EQ(999, plans[0].pid);
    EXPECT_EQ(0U, context.plannedPages[2]);
    EXPECT_EQ(0U, pidBudget.plannedPages);

    planCnt = 7;
    EXPECT_EQ(-EINVAL, BuildPairPlans(inputs, 2, &context, &pidBudget, MAX_4K_PROCESSES_CNT + 1, plans, 2, &planCnt));
    EXPECT_EQ(7U, planCnt);
}

TEST_F(MigrationTest, TestApplyPairPlansForStateRebuildsMigratingMatrixOnly)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr first = {.pid = 100, .scanType = NORMAL_SCAN};
    ProcessAttr second = {.pid = 200, .scanType = NORMAL_SCAN};
    ProcessAttr grouped = {.pid = 300, .scanType = NORMAL_SCAN};
    first.state = PROC_MIGRATE;
    grouped.groupPolicy.enabled = true;
    first.next = &second;
    second.next = &grouped;
    manager.processes = &first;
    first.strategyAttr.nrMigratePages[7][7] = 7;
    second.strategyAttr.nrMigratePages[6][6] = 6;
    grouped.strategyAttr.nrMigratePages[5][5] = 5;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plans[] = {
        MakePairPlan(100, 0, 2, 0, 80, 20),
        MakePairPlan(100, 1, 3, 1, 10, 50),
    };
    plans[0].demotePages = 60;
    plans[0].swapPages = 5;
    plans[1].promotePages = 40;

    ASSERT_EQ(0, ApplyPairPlansForState(&manager, plans, 2));
    EXPECT_EQ(65U, first.strategyAttr.nrMigratePages[0][2]);
    EXPECT_EQ(5U, first.strategyAttr.nrMigratePages[2][0]);
    EXPECT_EQ(40U, first.strategyAttr.nrMigratePages[3][1]);
    EXPECT_EQ(0U, first.strategyAttr.nrMigratePages[7][7]);
    EXPECT_EQ(6U, second.strategyAttr.nrMigratePages[6][6]);
    EXPECT_EQ(5U, grouped.strategyAttr.nrMigratePages[5][5]);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

TEST_F(MigrationTest, TestApplyPairPlansForStateFailureKeepsOldMatrix)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 2;
    ProcessAttr process = {.pid = 100, .scanType = NORMAL_SCAN};
    process.state = PROC_MIGRATE;
    manager.processes = &process;
    process.strategyAttr.nrMigratePages[0][2] = 7;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));

    PairPlan plan = MakePairPlan(100, 0, 2, 0, 80, 20);
    plan.demotePages = 60;
    plan.promotePages = 1;

    EXPECT_EQ(-EINVAL, ApplyPairPlansForState(&manager, &plan, 1));
    EXPECT_EQ(7U, process.strategyAttr.nrMigratePages[0][2]);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

static int BuildDisabledPairPlanInput(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                                      PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt,
                                      bool migrateOnly)
{
    (void)manager;
    EXPECT_TRUE(migrateOnly);
    EXPECT_GE(planCap, 1U);
    EXPECT_GE(pidBudgetCap, 1U);
    plans[0] = MakePairPlan(100, 0, 1, 0, 20, 10);
    pidBudgets[0] = {.pid = 100, .maxMigratePages = 100};
    *planCnt = 1;
    *pidBudgetCnt = 1;
    return 0;
}

static int BuildLocalFreeSnapshot(bool hugePage, int nrLocalNuma, PairPlanContext *context)
{
    (void)hugePage;
    EXPECT_EQ(1, nrLocalNuma);
    *context = {};
    context->nrLocalNuma = 1;
    context->freePages[0] = 100;
    return 0;
}

static int CheckFrozenPairPlanApplied(struct ProcessManager *manager, const PairPlan plans[], size_t planCnt)
{
    (void)manager;
    (void)plans;
    EXPECT_EQ(0U, planCnt);
    return 0;
}

TEST_F(MigrationTest, TestBuildAllPairPlansSkipsDisabledRemote)
{
    ProcessManager manager = {};
    manager.nrLocalNuma = 1;
    manager.tracking.pageSize = PAGESIZE_4K;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));
    EnvAtomicSet(&g_forbiddenNodes[1], 1);
    PairPlan plans[1] = {};
    size_t planCnt = 0;

    MOCKER(CollectNodeFreeSnapshot).stubs().will(invoke(BuildLocalFreeSnapshot));
    MOCKER(BuildAllPairPlanInputsForState).stubs().will(invoke(BuildDisabledPairPlanInput));
    MOCKER(BuildPairSwapPlans).stubs().will(returnValue(0));
    MOCKER(ApplyPairPlansForState).stubs().will(invoke(CheckFrozenPairPlanApplied));

    int ret = BuildAllPairPlans(&manager, plans, 1, &planCnt);
    EnvAtomicSet(&g_forbiddenNodes[1], 0);
    ASSERT_EQ(0, ret);
    EXPECT_EQ(0U, planCnt);

    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

extern "C" int PreMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg, uint64_t *migratePages);
TEST_F(MigrationTest, TestPerformMigration)
{
    int ret;
    ProcessAttr process = {.pid = 1025};
    struct ProcessManager manager = {.processes = &process};

    MOCKER(PreMigration).stubs().will(returnValue(-ENOMEM));
    ret = PerformMigration(&manager);
    EXPECT_EQ(-ENOMEM, ret);
}

extern "C" void PrintMigSpeed(struct ProcessManager *manager, uint64_t nr, struct timeval start, struct timeval end);
extern "C" void PostMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg);
TEST_F(MigrationTest, TestPerformMigrationSecond)
{
    int ret;
    ProcessAttr process = {.pid = 1025};
    struct ProcessManager manager = {.processes = &process};

    MOCKER(PreMigration).stubs().will(returnValue(0));
    MOCKER(DoMigration).stubs().will(returnValue(0));
    MOCKER(PrintMigSpeed).stubs().will(returnValue(0));
    MOCKER(PostMigration).stubs().will(ignoreReturnValue());
    ret = PerformMigration(&manager);
    EXPECT_EQ(0, ret);
}

extern "C" int AccessIoctlAddPid(int len, struct AccessAddPidPayload *payload);
extern "C" int UpdateScanTime(ProcessAttr *process);
TEST_F(MigrationTest, TestUpdateScanTime)
{
    int ret;
    ProcessAttr process;
    process.pid = 123;
    process.numaAttr.numaNodes = 0b00010001;
    process.scanType = NORMAL_SCAN;
    process.sceneInfo.cycles.scanCycle = 4;

    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(0));
    ret = UpdateScanTime(&process);
    EXPECT_EQ(0, ret);
}

extern "C" void UpdateScene(struct ProcessManager *manager);
TEST_F(MigrationTest, TestUpdateScene)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.type = VM_TYPE;
    current.scanType = NORMAL_SCAN;
    current.sceneInfo.currScene = LIGHT_STABLE_SCENE;
    current.sceneInfo.lastScene = HEAVY_STABLE_SCENE;
    UpdateScene(&manager);
    EXPECT_EQ(current.sceneInfo.pageInfoIndex, 1);
    EXPECT_EQ(current.sceneInfo.pageInfo[1].nrHot, 0);
    EXPECT_EQ(current.sceneInfo.pageInfo[1].nrL1Guarantee, 0);
    EXPECT_EQ(current.sceneInfo.currScene, HEAVY_STABLE_SCENE);
}

extern "C" int HandleScene(struct ProcessManager *manager);
TEST_F(MigrationTest, TestHandleScene)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.sceneInfo.currScene = UNSTABLE_SCENE;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).stubs().will(returnValue(0));

    int ret = HandleScene(&manager);
    EXPECT_EQ(0, ret);
}

extern "C" uint32_t GetScanPeriodConfig(void);
extern "C" void UpdateAllProcessScanTime(struct ProcessManager *manager);
TEST_F(MigrationTest, TestUpdateAllProcessScanTime)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.sceneInfo.currScene = UNSTABLE_SCENE;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).stubs().will(returnValue(0));
    MOCKER(GetScanPeriodConfig).stubs().will(returnValue(0));

    UpdateAllProcessScanTime(&manager);
}

extern "C" int HandleScene(struct ProcessManager *manager);
TEST_F(MigrationTest, TestHandleSceneFirstScanNoPages)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.isFirstScan = true;
    current.walkPage.nrPage = 0;
    current.scanTime = 0;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());

    int ret = HandleScene(&manager);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD, current.scanTime);
}

TEST_F(MigrationTest, TestHandleSceneFirstScanWithPages)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.isFirstScan = true;
    current.walkPage.nrPage = 1;
    current.scanType = NORMAL_SCAN;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).stubs().will(returnValue(0));

    int ret = HandleScene(&manager);
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(current.isFirstScan);
}

TEST_F(MigrationTest, TestHandleSceneFirstScanUpdateScanTimeFail)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.isFirstScan = true;
    current.walkPage.nrPage = 1;
    current.scanType = NORMAL_SCAN;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).stubs().will(returnValue(-1));

    int ret = HandleScene(&manager);
    EXPECT_EQ(0, ret);
    EXPECT_TRUE(current.isFirstScan);
}

TEST_F(MigrationTest, TestUpdateAllProcessScanTimeFirstScanSkip)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    current.next = nullptr;
    current.isFirstScan = true;
    current.scanType = NORMAL_SCAN;

    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).expects(never());

    UpdateAllProcessScanTime(&manager);
}

extern "C" bool GetFileConfSwitchConfig(void);
extern "C" bool GetMigratePeriodChanged(void);
extern "C" uint32_t GetMigratePeriodConfig(void);
extern "C" bool GetScanPeriodChanged(void);
extern "C" void UpdatePeriodFromConfig(struct ProcessManager *manager);
extern "C" void IoctlUpdateUbDmaAvail(uint32_t value);
extern "C" uint32_t GetMigrateModeConfig(void);
TEST_F(MigrationTest, TestUodatePeriodFromConfig)
{
    struct ProcessManager manager = { 0 };
    manager.migPeriod = 500;
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(true));
    MOCKER(GetScanPeriodChanged).stubs().will(returnValue(false));
    MOCKER(GetMigratePeriodChanged).stubs().will(returnValue(true));
    MOCKER(GetMigratePeriodConfig).stubs().will(returnValue(1000));
    MOCKER(IoctlUpdateUbDmaAvail).stubs();
    MOCKER(GetMigrateModeConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());

    UpdatePeriodFromConfig(&manager);
    EXPECT_EQ(1000, manager.migPeriod);
}

TEST_F(MigrationTest, TestUpdatePeriodFromConfigRestoreFirstScanNoPages)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    manager.migPeriod = 500;
    current.next = nullptr;
    current.isFirstScan = true;
    current.walkPage.nrPage = 0;
    current.scanTime = 0;

    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(true));
    MOCKER(GetScanPeriodChanged).stubs().will(returnValue(false));
    MOCKER(GetMigratePeriodChanged).stubs().will(returnValue(false));
    MOCKER(IoctlUpdateUbDmaAvail).stubs();
    MOCKER(GetMigrateModeConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());

    UpdatePeriodFromConfig(&manager);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD, current.scanTime);
}

TEST_F(MigrationTest, TestUpdatePeriodFromConfigRestoreFirstScanWithPages)
{
    struct ProcessManager manager = {0};
    ProcessAttr current = {};
    manager.processes = &current;
    manager.migPeriod = 500;
    current.next = nullptr;
    current.isFirstScan = true;
    current.walkPage.nrPage = 1;
    current.scanType = NORMAL_SCAN;

    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(true));
    MOCKER(GetScanPeriodChanged).stubs().will(returnValue(false));
    MOCKER(GetMigratePeriodChanged).stubs().will(returnValue(false));
    MOCKER(IoctlUpdateUbDmaAvail).stubs();
    MOCKER(GetMigrateModeConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateScanTime).stubs().will(returnValue(0));

    UpdatePeriodFromConfig(&manager);
    EXPECT_FALSE(current.isFirstScan);
}

extern "C" void UpdateMigResult(struct MigrateMsg *mMsg, struct ProcessManager *manager);
TEST_F(MigrationTest, TestUpdateMigResultLocalToRemote)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 4;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 30;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(170, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(1U, attr.managedLocalState.accountLocalMask[0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultSubtractsIsolatedFailure)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 4;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 30;
    mMsg.migList[0].failedIsolatedNr = 20;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(150, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultSettlesPairSwapByEachDirectionSuccess)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 2;
    mMsg.migList = (struct MigList *)calloc(2, sizeof(struct MigList));
    mMsg.migList[0].successToUser = true;
    mMsg.migList[0].nr = 10;
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 4;
    mMsg.migList[0].to = 0;
    mMsg.migList[1].successToUser = true;
    mMsg.migList[1].nr = 10;
    mMsg.migList[1].pid = 123;
    mMsg.migList[1].from = 0;
    mMsg.migList[1].to = 4;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));

    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(100U, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);

    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    mMsg.migList[0].failedMigNr = 4;
    mMsg.migList[1].failedMigNr = 1;
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(103U, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultPromoteSubtractsOnlySuccessfulPages)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][1] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 5;
    mMsg.migList[0].to = 2;
    mMsg.migList[0].nr = 80;
    mMsg.migList[0].failedMigNr = 10;
    mMsg.migList[0].failedIsolatedNr = 20;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));

    UpdateMigResult(&mMsg, &manager);

    EXPECT_EQ((uint32_t)50, attr.strategyAttr.remoteNrPagesAfterMigrate[2][1]);
    EXPECT_EQ(BIT(2), attr.managedLocalState.accountLocalMask[1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultUpdatesExactDemotePair)
{
    ProcessAttr attr = {};
    attr.pid = 123;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 2;
    mMsg.migList[0].to = 5;
    mMsg.migList[0].nr = 10;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));

    UpdateMigResult(&mMsg, &manager);

    EXPECT_EQ((uint32_t)10, attr.strategyAttr.remoteNrPagesAfterMigrate[2][1]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
    EXPECT_EQ(BIT(2), attr.managedLocalState.accountLocalMask[1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultRemoteToLocalUnexpectedMigCount)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 4;
    mMsg.migList[0].to = 0;
    mMsg.migList[0].nr = 1000;
    mMsg.migList[0].failedMigNr = 30;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(0U, attr.managedLocalState.accountLocalMask[0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultRemoteToLocalExpectedMigCount)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 4;
    mMsg.migList[0].to = 0;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 30;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(30, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
}
TEST_F(MigrationTest, TestUpdateMigResultTwo)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 0;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 5;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 40;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(60, attr.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultThree)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 0;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 5;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 40;
    mMsg.migList[0].successToUser = false;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultFour)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00100001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 1000;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 4;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 1;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(1099, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultFive)
{
    ProcessAttr attr = {};
    ProcessAttr attr2 = {};
    attr.next = &attr2;
    attr.pid = 123;
    attr2.pid = 1234;
    attr.numaAttr.numaNodes = 0b00110001;
    attr2.numaAttr.numaNodes = 0b00110001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 2;
    mMsg.migList = (struct MigList *)calloc(2, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 4;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 1;
    mMsg.migList[0].successToUser = true;

    mMsg.migList[1].pid = 1234;
    mMsg.migList[1].from = 0;
    mMsg.migList[1].to = 5;
    mMsg.migList[1].nr = 100;
    mMsg.migList[1].failedMigNr = 99;
    mMsg.migList[1].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    g_processManager.processes = &attr;
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(199, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(101, attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultSix)
{
    ProcessAttr attr = {};
    attr.next = NULL;
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0b00100001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 1;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 4;
    mMsg.migList[0].to = 0;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 30;
    mMsg.migList[0].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(30, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultSeven)
{
    ProcessAttr attr = {};
    ProcessAttr attr2 = {};
    attr.next = &attr2;
    attr.pid = 123;
    attr2.pid = 1234;
    attr.numaAttr.numaNodes = 0b00110001;
    attr2.numaAttr.numaNodes = 0b00110001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 2;
    mMsg.migList = (struct MigList *)calloc(2, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 4;
    mMsg.migList[0].to = 0;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 1;
    mMsg.migList[0].successToUser = true;

    mMsg.migList[1].pid = 1234;
    mMsg.migList[1].from = 5;
    mMsg.migList[1].to = 0;
    mMsg.migList[1].nr = 100;
    mMsg.migList[1].failedMigNr = 99;
    mMsg.migList[1].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    g_processManager.processes = &attr;
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(1, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(99, attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestUpdateMigResultEight)
{
    ProcessAttr attr = {};
    ProcessAttr attr2 = {};
    attr.next = &attr2;
    attr.pid = 123;
    attr2.pid = 1234;
    attr.numaAttr.numaNodes = 0b00110001;
    attr2.numaAttr.numaNodes = 0b00110001;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 100;

    struct MigrateMsg mMsg = {};
    mMsg.cnt = 2;
    mMsg.migList = (struct MigList *)calloc(2, sizeof(struct MigList));
    mMsg.migList[0].pid = 123;
    mMsg.migList[0].from = 0;
    mMsg.migList[0].to = 4;
    mMsg.migList[0].nr = 100;
    mMsg.migList[0].failedMigNr = 1;
    mMsg.migList[0].successToUser = true;

    mMsg.migList[1].pid = 1234;
    mMsg.migList[1].from = 5;
    mMsg.migList[1].to = 0;
    mMsg.migList[1].nr = 100;
    mMsg.migList[1].failedMigNr = 99;
    mMsg.migList[1].successToUser = true;

    ProcessManager manager = {};
    manager.nrLocalNuma = 4;
    manager.processes = &attr;
    g_processManager.processes = &attr;
    UpdateMigResult(&mMsg, &manager);
    EXPECT_EQ(199, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(99, attr2.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
}

extern "C" void MigrationUpdateMigrateModeAndScanCpu(void);
extern "C" void IoctlUpdateUbDmaAvail(uint32_t value);
extern "C" void IoctlSetScanCpuRange(uint32_t cpuMin, uint32_t cpuMax);
TEST_F(MigrationTest, TestUpdateMigrateModeAndScanCpuMigrateModeChanged)
{
    MOCKER(GetMigrateModeEnableConfig).stubs().will(returnValue(true));
    MOCKER(GetMigrateModeChanged).stubs().will(returnValue(true));
    MOCKER(GetMigrateModeConfig).stubs().will(returnValue((uint32_t)2));
    MOCKER(IoctlUpdateUbDmaAvail).stubs().will(ignoreReturnValue());
    MOCKER(SetMigrateModeChanged).stubs().will(ignoreReturnValue());
    MOCKER(GetScanCpuChanged).stubs().will(returnValue(false));

    MigrationUpdateMigrateModeAndScanCpu();
}

TEST_F(MigrationTest, TestUpdateMigrateModeAndScanCpuScanCpuChanged)
{
    MOCKER(GetMigrateModeEnableConfig).stubs().will(returnValue(false));
    MOCKER(GetScanCpuChanged).stubs().will(returnValue(true));
    MOCKER(GetScanCpuMinConfig).stubs().will(returnValue((uint32_t)1));
    MOCKER(GetScanCpuMaxConfig).stubs().will(returnValue((uint32_t)3));
    MOCKER(IoctlSetScanCpuRange).stubs().will(ignoreReturnValue());
    MOCKER(SetScanCpuChanged).stubs().will(ignoreReturnValue());

    MigrationUpdateMigrateModeAndScanCpu();
}

TEST_F(MigrationTest, TestUpdateMigrateModeAndScanCpuNoChange)
{
    MOCKER(GetMigrateModeEnableConfig).stubs().will(returnValue(true));
    MOCKER(GetMigrateModeChanged).stubs().will(returnValue(false));
    MOCKER(GetScanCpuChanged).stubs().will(returnValue(false));

    MigrationUpdateMigrateModeAndScanCpu();
}

extern "C" int MigrateRemoteNuma(struct ProcessManager *manager, struct MigrateNumaIoctlMsg *msg);
TEST_F(MigrationTest, TestMigrateRemoteNumaOne)
{
    struct ProcessManager manager;
    struct MigrateNumaIoctlMsg msg = {.srcNid = 4, .destNid = 5, .count = 1, .memids = {1}};
    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl)).stubs().will(returnValue(-ENOMEM));
    int ret = MigrateRemoteNuma(&manager, &msg);
    EXPECT_EQ(-ENOMEM, ret);
}

extern "C" int CleanStrategyAttribute(struct ProcessManager *manager);
TEST_F(MigrationTest, TestCleanStrateryAttribute)
{
    struct ProcessManager manager;
    ProcessAttr current;
    manager.processes = &current;

    current.next = NULL;
    EnvMutexInit(&manager.lock);
    int ret = CleanStrategyAttribute(&manager);
    EXPECT_EQ(0, ret);
}

extern "C" void PrintMigSpeed(struct ProcessManager *manager, uint64_t nr, struct timeval start, struct timeval end);
TEST_F(MigrationTest, TestPrintMigSpeed)
{
    struct timeval start = {0};
    struct timeval end = {0};
    struct ProcessManager manager;
    manager.tracking.pageSize = PAGESIZE_4K;
    MOCKER(CalcDurationUs).stubs().will(returnValue(static_cast<long>(100)));
    PrintMigSpeed(&manager, 1000, start, end);

    manager.tracking.pageSize = PAGESIZE_2M;
    MOCKER(CalcDurationUs).stubs().will(returnValue(static_cast<long>(100)));
    PrintMigSpeed(&manager, 1000, start, end);
}

extern "C" int PreMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg, uint64_t *migratePages);
TEST_F(MigrationTest, TestPreMigration)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    uint64_t migratePages = {};

    MOCKER(InitMigrateMsg).stubs().will(returnValue(-1));
    EnvMutexInit(&manager.lock);
    int ret = PreMigration(&manager, nullptr, nullptr);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    manager.processes = &current;
    current.pid = 1;
    current.next = NULL;
    current.scanType = NORMAL_SCAN;
    current.state = PROC_IDLE;
    MOCKER(InitMigrateMsg).stubs().will(returnValue(0));
    MOCKER(NumaMigReduceDeal).stubs();
    MOCKER(BuildAllPairPlans).stubs().will(returnValue(0));
    MOCKER(BuildMigrationMsg).stubs().will(returnValue(0));
    ret = PreMigration(&manager, &mMsg, &migratePages);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestPreMigrationTwo)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    uint64_t migratePages = {};

    MOCKER(InitMigrateMsg).stubs().will(returnValue(-1));
    EnvMutexInit(&manager.lock);
    int ret = PreMigration(&manager, nullptr, nullptr);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    manager.processes = &current;
    current.pid = 2;
    current.next = NULL;
    current.scanType = NORMAL_SCAN;
    current.state = PROC_MIGRATE;
    MOCKER(InitMigrateMsg).stubs().will(returnValue(0));
    MOCKER(NumaMigReduceDeal).stubs();
    MOCKER(BuildAllPairPlans).stubs().will(returnValue(0));
    MOCKER(BuildMigrationMsg).stubs().will(returnValue(0));
    ret = PreMigration(&manager, &mMsg, &migratePages);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestPreMigrationRollsBackStateWhenPairPlanningFails)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    uint64_t migratePages = 0;

    manager.processes = &current;
    current.pid = 3;
    current.scanType = NORMAL_SCAN;
    current.state = PROC_IDLE;
    ASSERT_EQ(0, EnvMutexInit(&manager.lock));
    MOCKER(InitMigrateMsg).stubs().will(returnValue(0));
    MOCKER(BuildAllPairPlans).stubs().will(returnValue(-EIO));

    EXPECT_EQ(-EIO, PreMigration(&manager, &mMsg, &migratePages));
    EXPECT_EQ(PROC_IDLE, current.state);
    EXPECT_EQ(nullptr, mMsg.migList);
    EXPECT_EQ(0, EnvMutexDestroy(&manager.lock));
}

extern "C" void PostMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg);
static bool g_migrationResultSettled;

static void MarkMigrationResultSettled(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    (void)mMsg;
    (void)manager;
    g_migrationResultSettled = true;
}

static int CheckPendingAppliedAfterResult(ProcessAttr *attr)
{
    (void)attr;
    EXPECT_TRUE(g_migrationResultSettled);
    return 0;
}

TEST_F(MigrationTest, TestPostMigration)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    manager.processes = &current;
    current.pid = 1;
    current.next = NULL;
    current.state = PROC_MIGRATE;

    MOCKER(UpdateMigResult).stubs();
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    EnvMutexInit(&manager.lock);
    PostMigration(&manager, &mMsg);
    EXPECT_EQ(PROC_IDLE, current.state);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestPostMigrationSettlesResultBeforePendingTarget)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    manager.processes = &current;
    current.pid = 1;
    current.state = PROC_MIGRATE;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    EnvMutexInit(&manager.lock);
    g_migrationResultSettled = false;

    MOCKER(UpdateMigResult).expects(once()).will(invoke(MarkMigrationResultSettled));
    MOCKER(ApplyPendingMigrationTargets).expects(once()).will(invoke(CheckPendingAppliedAfterResult));

    PostMigration(&manager, &mMsg);

    EXPECT_TRUE(g_migrationResultSettled);
    EXPECT_EQ(PROC_IDLE, current.state);
}

TEST_F(MigrationTest, TestPostMigrationTwo)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    manager.processes = &current;
    current.pid = 1;
    current.next = NULL;
    current.state = PROC_IDLE;

    MOCKER(UpdateMigResult).stubs();
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    EnvMutexInit(&manager.lock);
    PostMigration(&manager, &mMsg);
    EXPECT_EQ(PROC_IDLE, current.state);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestPostMigrationAppliesPendingGroupedPolicy)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};

    manager.processes = &current;
    current.pid = 1;
    current.state = PROC_MIGRATE;
    current.pendingGroupPolicy.valid = true;
    mMsg.migList = (struct MigList *)calloc(1, sizeof(struct MigList));
    EnvMutexInit(&manager.lock);
    MOCKER(UpdateMigResult).stubs();
    MOCKER(ApplyPendingGroupedPolicy).expects(once()).will(returnValue(0));

    PostMigration(&manager, &mMsg);
    EXPECT_EQ(PROC_IDLE, current.state);
    free(mMsg.migList);
}

TEST_F(MigrationTest, TestPostMigrationCompensatesGroupedSwapImbalance)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};
    ActcData localPages[2] = {};

    InitSingleGroupedSwapProcess(&current, 123, 5);
    current.scanAttr.actcLen[0] = 2;
    current.scanAttr.actcData[0] = localPages;
    manager.processes = &current;
    manager.nrLocalNuma = 4;
    manager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.processes = &current;
    InitUnbalancedGroupedSwapResult(&mMsg, current.pid);
    EnvMutexInit(&manager.lock);

    MOCKER(BuildAllPidData).expects(once()).will(returnValue(0));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(reinterpret_cast<int (*)(int, unsigned long, void *)>(ioctl))
        .expects(once())
        .will(invoke(MockGroupedSwapCompIoctlSuccess));

    PostMigration(&manager, &mMsg);
    EXPECT_EQ((uint64_t)5, current.groupPolicy.groups[0].targets[0].usedPages);
    EXPECT_FALSE(current.groupSwapFrozen);
    EXPECT_EQ(PROC_IDLE, current.state);
}

TEST_F(MigrationTest, TestPostMigrationSkipsEmptyCompEntryAndFreezes)
{
    struct ProcessManager manager = {};
    ProcessAttr current = {};
    struct MigrateMsg mMsg = {};

    InitSingleGroupedSwapProcess(&current, 123, 5);
    manager.processes = &current;
    manager.nrLocalNuma = 4;
    manager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.processes = &current;
    InitUnbalancedGroupedSwapResult(&mMsg, current.pid);
    EnvMutexInit(&manager.lock);

    MOCKER(BuildAllPidData).expects(once()).will(returnValue(0));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(UpdateMigResult).expects(once()).will(invoke(CheckNoEmptyCompEntryUpdateResult));

    PostMigration(&manager, &mMsg);
    EXPECT_TRUE(current.groupSwapFrozen);
    EXPECT_EQ(PROC_IDLE, current.state);
}

extern "C" void ApplyUbBwStop(ProcessAttr *current, struct ProcessManager *manager);
extern "C" void GetUbFluxMb(void);
extern "C" int ConfigUbWatch(uint32_t duration_ms);
extern "C" UbBwRestrictType ubBwRestrict[MAX_NODES];

TEST_F(MigrationTest, TestApplyUbBwStopThresholdZero)
{
    ProcessAttr current = {};
    struct ProcessManager manager = {};
    manager.ubBwMonitor.ubBwThreshold = 0;
    manager.ubBwMonitor.currentFluxRet = 0;
    current.strategyAttr.nrMigratePages[0][1] = 100;
    current.strategyAttr.nrMigratePages[1][0] = 200;

    ApplyUbBwStop(&current, &manager);
    EXPECT_EQ(100, current.strategyAttr.nrMigratePages[0][1]);
    EXPECT_EQ(200, current.strategyAttr.nrMigratePages[1][0]);
}

TEST_F(MigrationTest, TestApplyUbBwStopFluxRetFailed)
{
    ProcessAttr current = {};
    struct ProcessManager manager = {};
    manager.ubBwMonitor.ubBwThreshold = 1000;
    manager.ubBwMonitor.currentFluxRet = -1;
    current.strategyAttr.nrMigratePages[0][1] = 100;

    ApplyUbBwStop(&current, &manager);
    EXPECT_EQ(100, current.strategyAttr.nrMigratePages[0][1]);
}

TEST_F(MigrationTest, TestApplyUbBwStopBwExceedThreshold)
{
    ProcessAttr current = {};
    struct ProcessManager manager = {};
    manager.ubBwMonitor.ubBwThreshold = 500;
    manager.ubBwMonitor.currentFluxRet = 0;
    manager.ubBwMonitor.currentFluxMb.len = 1;
    manager.ubBwMonitor.currentFluxMb.flux[0].numaId = 2;
    manager.ubBwMonitor.currentFluxMb.flux[0].readMb = 300;
    manager.ubBwMonitor.currentFluxMb.flux[0].writeMb = 300;
    current.strategyAttr.nrMigratePages[0][2] = 100;
    current.strategyAttr.nrMigratePages[2][0] = 200;
    current.strategyAttr.nrMigratePages[1][2] = 50;
    current.strategyAttr.nrMigratePages[2][1] = 60;
    current.strategyAttr.nrMigratePages[0][1] = 300;

    ApplyUbBwStop(&current, &manager);
    /* ApplyUbBwStop only sets ubBwRestrict flag; nrMigratePages cleared by strategy layer */
    EXPECT_EQ(UB_BW_SWAP_STOP, current.strategyAttr.ubBwRestrict[2]);
    EXPECT_EQ(UB_BW_NORMAL, current.strategyAttr.ubBwRestrict[0]);
    EXPECT_EQ(UB_BW_NORMAL, current.strategyAttr.ubBwRestrict[1]);
}

TEST_F(MigrationTest, TestApplyUbBwStopBwBelowThreshold)
{
    ProcessAttr current = {};
    struct ProcessManager manager = {};
    manager.ubBwMonitor.ubBwThreshold = 1000;
    manager.ubBwMonitor.currentFluxRet = 0;
    manager.ubBwMonitor.currentFluxMb.len = 1;
    manager.ubBwMonitor.currentFluxMb.flux[0].numaId = 2;
    manager.ubBwMonitor.currentFluxMb.flux[0].readMb = 300;
    manager.ubBwMonitor.currentFluxMb.flux[0].writeMb = 300;
    current.strategyAttr.nrMigratePages[0][2] = 100;

    ApplyUbBwStop(&current, &manager);
    EXPECT_EQ(100, current.strategyAttr.nrMigratePages[0][2]);
    EXPECT_EQ(UB_BW_NORMAL, current.strategyAttr.ubBwRestrict[2]);
}

TEST_F(MigrationTest, TestApplyUbBwStopMultipleNumaMixed)
{
    ProcessAttr current = {};
    struct ProcessManager manager = {};
    manager.ubBwMonitor.ubBwThreshold = 500;
    manager.ubBwMonitor.currentFluxRet = 0;
    manager.ubBwMonitor.currentFluxMb.len = 2;
    manager.ubBwMonitor.currentFluxMb.flux[0].numaId = 1;
    manager.ubBwMonitor.currentFluxMb.flux[0].readMb = 400;
    manager.ubBwMonitor.currentFluxMb.flux[0].writeMb = 200;
    manager.ubBwMonitor.currentFluxMb.flux[1].numaId = 3;
    manager.ubBwMonitor.currentFluxMb.flux[1].readMb = 100;
    manager.ubBwMonitor.currentFluxMb.flux[1].writeMb = 100;
    current.strategyAttr.nrMigratePages[0][1] = 100;
    current.strategyAttr.nrMigratePages[1][0] = 200;
    current.strategyAttr.nrMigratePages[0][3] = 50;
    current.strategyAttr.nrMigratePages[3][0] = 60;

    ApplyUbBwStop(&current, &manager);
    /* ApplyUbBwStop only sets ubBwRestrict flag; nrMigratePages cleared by strategy layer */
    EXPECT_EQ(UB_BW_SWAP_STOP, current.strategyAttr.ubBwRestrict[1]);
    EXPECT_EQ(UB_BW_NORMAL, current.strategyAttr.ubBwRestrict[3]);
}

TEST_F(MigrationTest, TestScanMigrateWorkThresholdZeroSkipQuery)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };
    manager.ubBwMonitor.ubBwThreshold = 0;
    manager.ubBwMonitor.currentFluxRet = 0;

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(StrategyConfigRead).stubs().will(ignoreReturnValue());
    MOCKER(GetUbBwThresholdConfig).stubs().will(returnValue((uint32_t)0));
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(true));
    MOCKER(SetAdaptMem).stubs().will(ignoreReturnValue());
    MOCKER(GetAdaptiveRatioEnableConfig).stubs().will(returnValue(true));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(0));
    MOCKER(UpdateScene).stubs();
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdatePeriodFromConfig).stubs().will(ignoreReturnValue());
    MOCKER(PerformMigration).stubs().will(returnValue(0));
    MOCKER(EnableTracking).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkThresholdNonZeroQueryAndConfig)
{
    int ret;
    ProcessAttr process = { .pid = 1025 };
    struct ProcessManager manager = { .processes = &process };
    manager.ubBwMonitor.ubBwThreshold = 1000;

    MOCKER(GetUbFluxMb).stubs().will(ignoreReturnValue());
    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(StrategyConfigRead).stubs().will(ignoreReturnValue());
    MOCKER(GetUbBwThresholdConfig).stubs().will(returnValue((uint32_t)1000));
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(false));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(0));
    MOCKER(UpdateScene).stubs();
    MOCKER(HandleScene).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(PerformMigration).stubs().will(returnValue(0));
    MOCKER(ConfigUbWatch).stubs().will(returnValue(0));
    MOCKER(EnableTracking).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}

TEST_F(MigrationTest, TestBuildMigrationMsgL2NodeCriticalErr)
{
    int ret;
    uint64_t pages;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    struct MigrateMsg mMsg;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[4], 0);

    MOCKER(CheckActcDataValid).stubs().will(returnValue(0));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(IsRemoteNumaCriticalErr).stubs().will(returnValue(true));
    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(-ENODEV, ret);
    GlobalMockObject::verify();
}

TEST_F(MigrationTest, TestBuildMigrationMsgL2NodeNotCriticalNotForbidden)
{
    int ret;
    uint64_t pages;
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b00010001;
    struct MigrateMsg mMsg;
    ActcData actc = {};
    process.scanAttr.actcData[0] = &actc;

    g_processManager.nrLocalNuma = 4;
    EnvAtomicSet(&g_forbiddenNodes[4], 0);

    MOCKER(CheckActcDataValid).stubs().will(returnValue(0));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(IsRemoteNumaCriticalErr).stubs().will(returnValue(false));
    MOCKER(RunStrategy).stubs().will(returnValue(-ENOENT));
    ret = BuildMigrationMsg(&process, &mMsg, &pages);
    EXPECT_EQ(-ENOENT, ret);
}

TEST_F(MigrationTest, TestScanMigrateWorkCallsUpdateRemoteNumaCriticalErr)
{
    int ret;
    ProcessAttr process = {.pid = 1025};
    struct ProcessManager manager = {.processes = &process};

    MOCKER(DisableTracking).stubs().will(returnValue(0));
    MOCKER(StrategyConfigRead).stubs().will(ignoreReturnValue());
    MOCKER(GetFileConfSwitchConfig).stubs().will(returnValue(false));
    MOCKER(CheckAndRemoveInvalidProcess).stubs();
    MOCKER(PerformMigrationPreparation).stubs().will(returnValue(0));
    MOCKER(UpdateScene).stubs();
    MOCKER(HandleScene).stubs().will(returnValue(0));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(UpdateRemoteNumaCriticalErr).stubs();
    MOCKER(PerformMigration).stubs().will(returnValue(0));
    ret = ScanMigrateWork(&manager);
    EXPECT_EQ(0, ret);
}
