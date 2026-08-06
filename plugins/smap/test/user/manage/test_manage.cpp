/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user manage ut code
 * Create: 2024-09-18
 */

#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/access_ioctl.h"
#include "manage/manage.h"
#include "manage/smap_config.h"
#include "manage/thread.h"
#include "securec.h"
#include "strategy/strategy_config.h"

using namespace std;

#define BIT(i) (1U << (i))

static cpu_set_t g_fake_cpu_mask;
extern "C" struct ProcessManager g_processManager;
extern "C" uint32_t g_pageSizeNormal;
extern "C" uint32_t g_pageSizeHuge;
extern "C" RunMode g_runMode;
extern "C" void RemoteNumaInfoInit();
extern "C" int GetNodeFromCpu(int cpu);
extern "C" int RefreshManagedLocalState(ProcessAttr *attr, bool fullReplacement);
extern "C" uint32_t BuildManagedTrackingNodes(const ProcessAttr *attr);
extern "C" int RefreshManagedLocalTrackingScope(ProcessAttr *attr);
extern "C" int UpdateManagedProcessTrackingMode(ProcessAttr *attr, ScanType scanType, uint32_t scanTime,
                                                uint32_t duration);
extern "C" int SetLocalNumaByCpu(pid_t pid, uint32_t *nodeBitmap);
extern "C" int GetProcessNumaMapsObservation(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                             uint64_t numaPages[MAX_NODES]);
extern "C" int PrepareProcessManageCandidate(ProcessParam *param, PidType type, ProcessManageCandidate *candidate);
extern "C" void DiscardProcessManageCandidate(ProcessManageCandidate *candidate);
extern "C" void PublishProcessManageCandidate(ProcessManageCandidate *candidate);

class ScopedMigrationPeriod {
public:
    explicit ScopedMigrationPeriod(uint32_t period) : previous_(g_processManager.threadCtx[0]), context_()
    {
        context_.period = period;
        g_processManager.threadCtx[0] = &context_;
    }

    ~ScopedMigrationPeriod()
    {
        g_processManager.threadCtx[0] = previous_;
    }

private:
    void *previous_;
    ThreadCtx context_;
};

static int AddAffinityLocalForTest(pid_t pid, uint32_t *nodeBitmap);
static int AddCandidateResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                       uint64_t numaPages[MAX_NODES]);
static int AddEmptyCandidateResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                            uint64_t numaPages[MAX_NODES]);
static int AddUnexpectedRemoteResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                              uint64_t numaPages[MAX_NODES]);

static int fake_sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
    (void)pid;
    (void)cpusetsize;
    *mask = g_fake_cpu_mask;
    return 0;
}

class ManageTest : public ::testing::Test {
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

TEST_F(ManageTest, TestRemoteNumaInfoInit)
{
    g_processManager.remoteNumaInfo.usedInfo[0].size = 10;
    RemoteNumaInfoInit();
    EXPECT_EQ(0, g_processManager.remoteNumaInfo.usedInfo[0].size);
}

TEST_F(ManageTest, TestInitAndClearProcessTargetConfig)
{
    ProcessTargetConfig config = {};
    config.migrateMode = MIG_MEMSIZE_MODE;
    config.count = 1;
    config.targets[0].remoteNid = 4;
    config.targets[0].ratio = 50;
    config.targets[0].memSizeKB = 2048;

    InitProcessTargetConfig(&config);
    EXPECT_EQ(MIG_RATIO_MODE, config.migrateMode);
    EXPECT_EQ(0U, config.count);
    EXPECT_EQ(0, config.targets[0].remoteNid);
    EXPECT_EQ(0U, config.targets[0].ratio);
    EXPECT_EQ(0U, config.targets[0].memSizeKB);

    config.count = 1;
    config.targets[0].remoteNid = 5;
    ClearProcessTargetConfig(&config);
    EXPECT_EQ(0U, config.count);
    EXPECT_EQ(0, config.targets[0].remoteNid);

    InitProcessTargetConfig(nullptr);
    ClearProcessTargetConfig(nullptr);
}

TEST_F(ManageTest, TestCopyProcessTargetConfig)
{
    ProcessTargetConfig source = {};
    source.migrateMode = MIG_MEMSIZE_MODE;
    source.count = 2;
    source.targets[0] = {4, 0, 2048};
    source.targets[1] = {5, 0, 4096};
    ProcessTargetConfig dest = {};

    EXPECT_EQ(0, CopyProcessTargetConfig(&dest, &source));
    EXPECT_EQ(MIG_MEMSIZE_MODE, dest.migrateMode);
    EXPECT_EQ(2U, dest.count);
    EXPECT_EQ(4, dest.targets[0].remoteNid);
    EXPECT_EQ(2048U, dest.targets[0].memSizeKB);
    EXPECT_EQ(5, dest.targets[1].remoteNid);
    EXPECT_EQ(4096U, dest.targets[1].memSizeKB);

    source.targets[0].memSizeKB = 8192;
    EXPECT_EQ(2048U, dest.targets[0].memSizeKB);

    EXPECT_EQ(-EINVAL, CopyProcessTargetConfig(nullptr, &source));
    EXPECT_EQ(-EINVAL, CopyProcessTargetConfig(&dest, nullptr));

    source.count = REMOTE_NUMA_NUM + 1;
    EXPECT_EQ(-EINVAL, CopyProcessTargetConfig(&dest, &source));
    source.count = 0;
    source.migrateMode = static_cast<MigrateMode>(-1);
    EXPECT_EQ(-EINVAL, CopyProcessTargetConfig(&dest, &source));
}

TEST_F(ManageTest, TestRemoveProcessRemoteTarget)
{
    ProcessTargetConfig config = {};
    config.migrateMode = MIG_MEMSIZE_MODE;
    config.count = 2;
    config.targets[0] = {4, 0, 2048};
    config.targets[1] = {6, 0, 4096};

    EXPECT_TRUE(RemoveProcessRemoteTarget(&config, 4));
    EXPECT_EQ(1U, config.count);
    EXPECT_EQ(6, config.targets[0].remoteNid);
    EXPECT_EQ(0, config.targets[1].remoteNid);
    EXPECT_FALSE(RemoveProcessRemoteTarget(&config, 4));
    EXPECT_FALSE(RemoveProcessRemoteTarget(nullptr, 6));

    config.count = REMOTE_NUMA_NUM + 1;
    EXPECT_FALSE(RemoveProcessRemoteTarget(&config, 6));
}

TEST_F(ManageTest, TestMoveProcessRemoteTarget)
{
    ProcessTargetConfig ratioConfig = {};
    ratioConfig.migrateMode = MIG_RATIO_MODE;
    ratioConfig.count = 2;
    ratioConfig.targets[0] = {3, 60, 0};
    ratioConfig.targets[1] = {8, 40, 0};

    EXPECT_EQ(0, MoveProcessRemoteTarget(&ratioConfig, 3, 12, 60, 60));
    EXPECT_EQ(2U, ratioConfig.count);
    EXPECT_EQ(12, ratioConfig.targets[0].remoteNid);
    EXPECT_EQ(60U, ratioConfig.targets[0].ratio);
    EXPECT_EQ(8, ratioConfig.targets[1].remoteNid);

    ProcessTargetConfig memSizeConfig = {};
    memSizeConfig.migrateMode = MIG_MEMSIZE_MODE;
    memSizeConfig.count = 2;
    memSizeConfig.targets[0] = {3, 0, 8192};
    memSizeConfig.targets[1] = {12, 0, 4096};

    EXPECT_EQ(0, MoveProcessRemoteTarget(&memSizeConfig, 3, 12, 4096, 0));
    EXPECT_EQ(2U, memSizeConfig.count);
    EXPECT_EQ(4096U, memSizeConfig.targets[0].memSizeKB);
    EXPECT_EQ(8192U, memSizeConfig.targets[1].memSizeKB);
    EXPECT_EQ(-ERANGE, MoveProcessRemoteTarget(&memSizeConfig, 3, 12, 8192, 0));
    EXPECT_EQ(-ENOENT, MoveProcessRemoteTarget(&memSizeConfig, 4, 12, 1, 0));
}

TEST_F(ManageTest, TestUpdateManagedProcessTrackingModeKeepsMigrationConfig)
{
    ProcessAttr attr = {};
    attr.state = PROC_IDLE;
    attr.scanType = NORMAL_SCAN;
    attr.migrateMode = MIG_MEMSIZE_MODE;
    attr.remoteNumaCnt = 2;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 2;
    attr.targetConfig.targets[0] = {4, 0, 2048};
    attr.targetConfig.targets[1] = {6, 0, 4096};
    attr.migrateParam[0] = {4, 2048};
    attr.migrateParam[1] = {6, 4096};
    attr.numaAttr.numaNodes = BIT(0) | BIT(1) | BIT(4) | BIT(6);
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 123;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][2] = 456;

    // Migration must be explicitly disabled before changing away from NORMAL_SCAN.
    EXPECT_EQ(-EBUSY, UpdateManagedProcessTrackingMode(&attr, HAM_SCAN, 200, 60));
    attr.state = PROC_MOVE;
    EXPECT_EQ(0, UpdateManagedProcessTrackingMode(&attr, HAM_SCAN, 200, 60));
    EXPECT_EQ(HAM_SCAN, attr.scanType);
    EXPECT_EQ(PROC_MOVE, attr.state);
    EXPECT_EQ(200U, attr.scanTime);
    EXPECT_EQ(60U, attr.duration);
    EXPECT_TRUE(attr.isFirstScan);
    EXPECT_EQ(MIG_MEMSIZE_MODE, attr.migrateMode);
    EXPECT_EQ(2, attr.remoteNumaCnt);
    EXPECT_EQ(2U, attr.targetConfig.count);
    EXPECT_EQ(4, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(6, attr.targetConfig.targets[1].remoteNid);
    EXPECT_EQ(0x53U, attr.numaAttr.numaNodes);
    EXPECT_EQ(0x3U, attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(123U, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(456U, attr.strategyAttr.remoteNrPagesAfterMigrate[1][2]);

    EXPECT_EQ(0, UpdateManagedProcessTrackingMode(&attr, NORMAL_SCAN, 400, 120));
    EXPECT_EQ(NORMAL_SCAN, attr.scanType);
    EXPECT_EQ(PROC_MOVE, attr.state);
    EXPECT_EQ(400U, attr.scanTime);
    EXPECT_EQ(120U, attr.duration);
    EXPECT_EQ(2U, attr.targetConfig.count);
    EXPECT_EQ(4, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(6, attr.targetConfig.targets[1].remoteNid);
    EXPECT_EQ(123U, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(456U, attr.strategyAttr.remoteNrPagesAfterMigrate[1][2]);
}

TEST_F(ManageTest, TestUpdateManagedProcessTrackingModeRejectsMigratingProcess)
{
    ProcessAttr attr = {};
    attr.state = PROC_MIGRATE;
    attr.scanType = NORMAL_SCAN;
    attr.scanTime = 200;
    attr.duration = 60;

    EXPECT_EQ(-EBUSY, UpdateManagedProcessTrackingMode(&attr, HAM_SCAN, 400, 120));
    EXPECT_EQ(PROC_MIGRATE, attr.state);
    EXPECT_EQ(NORMAL_SCAN, attr.scanType);
    EXPECT_EQ(200U, attr.scanTime);
    EXPECT_EQ(60U, attr.duration);
}

TEST_F(ManageTest, TestUpdateManagedProcessTrackingModeRejectsActiveMigration)
{
    ProcessAttr attr = {};
    attr.state = PROC_IDLE;
    attr.scanType = NORMAL_SCAN;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {4, 0, 2048};

    EXPECT_EQ(-EBUSY, UpdateManagedProcessTrackingMode(&attr, HAM_SCAN, 400, 120));
    EXPECT_EQ(PROC_IDLE, attr.state);
    EXPECT_EQ(NORMAL_SCAN, attr.scanType);
}

TEST_F(ManageTest, TestUpdateManagedProcessTrackingModeKeepsMigrationDisabled)
{
    ProcessAttr attr = {};
    attr.state = PROC_MOVE;
    attr.scanType = NORMAL_SCAN;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {4, 0, 2048};

    EXPECT_EQ(0, UpdateManagedProcessTrackingMode(&attr, NORMAL_SCAN, 400, 120));
    EXPECT_EQ(PROC_MOVE, attr.state);
    EXPECT_EQ(0, UpdateManagedProcessTrackingMode(&attr, HAM_SCAN, 400, 120));
    EXPECT_EQ(PROC_MOVE, attr.state);
}

TEST_F(ManageTest, TestFindProcessRemoteTarget)
{
    ProcessTargetConfig config = {};
    config.count = 2;
    config.targets[0] = {4, 25, 0};
    config.targets[1] = {7, 50, 0};

    const ProcessRemoteTarget *target = FindProcessRemoteTarget(&config, 7);
    ASSERT_NE(nullptr, target);
    EXPECT_EQ(50U, target->ratio);
    EXPECT_EQ(nullptr, FindProcessRemoteTarget(&config, 6));
    EXPECT_EQ(nullptr, FindProcessRemoteTarget(nullptr, 7));

    config.count = REMOTE_NUMA_NUM + 1;
    EXPECT_EQ(nullptr, FindProcessRemoteTarget(&config, 7));
}

TEST_F(ManageTest, TestRemoteNidToIndex)
{
    int remoteIndex = -1;

    EXPECT_EQ(0, RemoteNidToIndex(4, 4, &remoteIndex));
    EXPECT_EQ(0, remoteIndex);
    EXPECT_EQ(0, RemoteNidToIndex(21, 4, &remoteIndex));
    EXPECT_EQ(REMOTE_NUMA_NUM - 1, remoteIndex);

    remoteIndex = 9;
    EXPECT_EQ(-EINVAL, RemoteNidToIndex(3, 4, &remoteIndex));
    EXPECT_EQ(9, remoteIndex);
    EXPECT_EQ(-EINVAL, RemoteNidToIndex(22, 4, &remoteIndex));
    EXPECT_EQ(-EINVAL, RemoteNidToIndex(4, 0, &remoteIndex));
    EXPECT_EQ(-EINVAL, RemoteNidToIndex(4, LOCAL_NUMA_NUM + 1, &remoteIndex));
    EXPECT_EQ(-EINVAL, RemoteNidToIndex(4, 4, nullptr));
}

TEST_F(ManageTest, TestInitProcessMigrationTargetState)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.targetConfig.count = 1;
    attr.pendingTargetConfig.count = 1;
    attr.pendingTargetConfigValid = true;
    attr.pendingIgnoreRemoteCapacity = true;
    attr.pendingTargetNumaNodes = 0x31;
    attr.managedLocalState.managedLocalMask = 0xf;
    attr.managedLocalState.accountLocalMask[0] = 0x1;

    InitProcessMigrationTargetState(&attr);
    EXPECT_EQ(123, attr.pid);
    EXPECT_EQ(0U, attr.targetConfig.count);
    EXPECT_EQ(0U, attr.pendingTargetConfig.count);
    EXPECT_FALSE(attr.pendingTargetConfigValid);
    EXPECT_FALSE(attr.pendingIgnoreRemoteCapacity);
    EXPECT_EQ(0U, attr.pendingTargetNumaNodes);
    EXPECT_EQ(0U, attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(0U, attr.managedLocalState.accountLocalMask[0]);

    InitProcessMigrationTargetState(nullptr);
}

TEST_F(ManageTest, TestRefreshManagedLocalStatePeriodicExpansion)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.numaAttr.numaNodes = BIT(0) | BIT(1);
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    g_processManager.nrLocalNuma = 4;
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).expects(once()).will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).expects(once()).will(returnValue(2));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_EQ(BIT(2), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(1) | BIT(2), attr.managedLocalState.managedLocalMask);
}

TEST_F(ManageTest, TestRefreshManagedLocalStatePeriodicUsesCachedAffinity)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.affinityLocalMask = BIT(1);
    attr.managedLocalState.affinityValid = true;
    attr.managedLocalState.affinitySampled = true;
    attr.walkPage.nrPages[2] = 10;
    g_processManager.nrLocalNuma = 4;
    ScopedMigrationPeriod refreshPeriod(DEFAULT_MIGRATE_PERIOD);
    MOCKER(SetLocalNumaByCpu).expects(never());

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_EQ(BIT(1), attr.managedLocalState.affinityLocalMask);
    EXPECT_EQ(BIT(1) | BIT(2), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(1) | BIT(2), attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(DEFAULT_MIGRATE_PERIOD, attr.managedLocalState.affinityRefreshElapsedMs);
}

TEST_F(ManageTest, TestRefreshManagedLocalStatePeriodicRefreshesDueAffinity)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.affinityLocalMask = BIT(0);
    attr.managedLocalState.affinityRefreshElapsedMs =
        MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS - DEFAULT_MIGRATE_PERIOD;
    attr.managedLocalState.affinityValid = true;
    attr.managedLocalState.affinitySampled = true;
    g_processManager.nrLocalNuma = 4;
    ScopedMigrationPeriod refreshPeriod(DEFAULT_MIGRATE_PERIOD);
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).expects(once()).will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).expects(once()).will(returnValue(3));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_EQ(BIT(3), attr.managedLocalState.affinityLocalMask);
    EXPECT_EQ(BIT(3), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(0U, attr.managedLocalState.affinityRefreshElapsedMs);
}

TEST_F(ManageTest, TestRefreshManagedLocalStateAffinityFailureKeepsCache)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.managedLocalState.managedLocalMask = BIT(1);
    attr.managedLocalState.affinityLocalMask = BIT(1);
    attr.managedLocalState.affinityRefreshElapsedMs = MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS;
    attr.managedLocalState.affinityValid = true;
    attr.managedLocalState.affinitySampled = true;
    g_processManager.nrLocalNuma = 4;
    ScopedMigrationPeriod refreshPeriod(DEFAULT_MIGRATE_PERIOD);
    MOCKER(sched_getaffinity).expects(once()).will(returnValue(-1));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_EQ(BIT(1), attr.managedLocalState.affinityLocalMask);
    EXPECT_EQ(BIT(1), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(BIT(1), attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(0U, attr.managedLocalState.affinityRefreshElapsedMs);
}

TEST_F(ManageTest, TestRefreshManagedLocalStateAffinityFailureBacksOff)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.walkPage.nrPages[2] = 10;
    g_processManager.nrLocalNuma = 4;
    ScopedMigrationPeriod refreshPeriod(DEFAULT_MIGRATE_PERIOD);
    MOCKER(sched_getaffinity).expects(once()).will(returnValue(-1));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_TRUE(attr.managedLocalState.affinitySampled);
    EXPECT_FALSE(attr.managedLocalState.affinityValid);
    EXPECT_EQ(BIT(2), attr.managedLocalState.observedLocalMask);

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, false));
    EXPECT_EQ(BIT(2), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(DEFAULT_MIGRATE_PERIOD, attr.managedLocalState.affinityRefreshElapsedMs);
}

TEST_F(ManageTest, TestRefreshManagedLocalStateFullReplacementShrinks)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.numaAttr.numaNodes = BIT(0) | BIT(1);
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    g_processManager.nrLocalNuma = 4;
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).expects(once()).will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).expects(once()).will(returnValue(0));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, true));
    EXPECT_EQ(BIT(0), attr.managedLocalState.observedLocalMask);
    EXPECT_EQ(BIT(0), attr.managedLocalState.managedLocalMask);
}

TEST_F(ManageTest, TestRefreshManagedLocalStateKeepsAccountLocal)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.managedLocalState.managedLocalMask = BIT(1);
    attr.strategyAttr.remoteNrPagesAfterMigrate[3][2] = 10;
    g_processManager.nrLocalNuma = 4;
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).expects(once()).will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).expects(once()).will(returnValue(0));

    EXPECT_EQ(0, RefreshManagedLocalState(&attr, true));
    EXPECT_EQ(BIT(3), attr.managedLocalState.accountLocalMask[2]);
    EXPECT_EQ(BIT(0) | BIT(3), attr.managedLocalState.managedLocalMask);
}

TEST_F(ManageTest, TestBuildManagedTrackingNodes)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    attr.numaAttr.numaNodes = BIT(1) | BIT(4);
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(2);
    attr.managedLocalState.accountLocalMask[2] = BIT(2);
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {5, 25, 0};

    EXPECT_EQ(BIT(0) | BIT(2) | BIT(4) | BIT(5) | BIT(6), BuildManagedTrackingNodes(&attr));
    EXPECT_EQ(0U, BuildManagedTrackingNodes(nullptr));
}

TEST_F(ManageTest, TestBuildManagedTrackingNodesKeepsOmittedRemoteWithResidentPages)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    attr.numaAttr.numaNodes = BIT(4);
    attr.walkPage.nrPage = 1;
    attr.walkPage.nrPages[4] = 1;

    EXPECT_EQ(BIT(4), BuildManagedTrackingNodes(&attr));
}

static int RefreshPeriodicManagedLocalCandidate(ProcessAttr *attr, bool fullReplacement)
{
    EXPECT_FALSE(fullReplacement);
    attr->managedLocalState.managedLocalMask = BIT(0) | BIT(2);
    attr->managedLocalState.observedLocalMask = BIT(2);
    attr->managedLocalState.residentLocalMask = BIT(2);
    return 0;
}

static int RefreshUnchangedManagedLocalCandidate(ProcessAttr *attr, bool fullReplacement)
{
    EXPECT_FALSE(fullReplacement);
    attr->managedLocalState.managedLocalMask = BIT(0);
    attr->managedLocalState.residentLocalMask = BIT(0);
    return 0;
}

static int CheckManagedTrackingPayload(int len, struct AccessAddPidPayload *payload)
{
    EXPECT_EQ(1, len);
    EXPECT_EQ(123, payload[0].pid);
    EXPECT_EQ(100U, payload[0].scanTime);
    EXPECT_EQ(NORMAL_SCAN, payload[0].type);
    EXPECT_EQ(BIT(0) | BIT(2) | BIT(4), payload[0].numaNodes);
    return 0;
}

TEST_F(ManageTest, TestRefreshManagedLocalTrackingScopePublishesAfterTracking)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    attr.pid = 123;
    attr.scanType = NORMAL_SCAN;
    attr.scanTime = 100;
    attr.numaAttr.numaNodes = BIT(0) | BIT(4);
    attr.managedLocalState.managedLocalMask = BIT(0);

    MOCKER(RefreshManagedLocalState).expects(once()).will(invoke(RefreshPeriodicManagedLocalCandidate));
    MOCKER(AccessIoctlAddPid).expects(once()).will(invoke(CheckManagedTrackingPayload));

    EXPECT_EQ(0, RefreshManagedLocalTrackingScope(&attr));
    EXPECT_EQ(BIT(0) | BIT(2), attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(2) | BIT(4), attr.numaAttr.numaNodes);
}

TEST_F(ManageTest, TestRefreshManagedLocalTrackingFailureKeepsActiveState)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    attr.pid = 123;
    attr.scanType = NORMAL_SCAN;
    attr.scanTime = 100;
    attr.numaAttr.numaNodes = BIT(0) | BIT(4);
    attr.managedLocalState.managedLocalMask = BIT(0);

    MOCKER(RefreshManagedLocalState).expects(once()).will(invoke(RefreshPeriodicManagedLocalCandidate));
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(-EIO));

    EXPECT_EQ(-EIO, RefreshManagedLocalTrackingScope(&attr));
    EXPECT_EQ(BIT(0), attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(4), attr.numaAttr.numaNodes);
}

TEST_F(ManageTest, TestRefreshManagedLocalTrackingScopeSkipsUnchangedBitmap)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    attr.pid = 123;
    attr.scanType = NORMAL_SCAN;
    attr.scanTime = 100;
    attr.numaAttr.numaNodes = BIT(0) | BIT(4);
    attr.managedLocalState.managedLocalMask = BIT(0);

    MOCKER(RefreshManagedLocalState).expects(once()).will(invoke(RefreshUnchangedManagedLocalCandidate));
    MOCKER(AccessIoctlAddPid).expects(never());

    EXPECT_EQ(0, RefreshManagedLocalTrackingScope(&attr));
    EXPECT_EQ(BIT(0), attr.managedLocalState.residentLocalMask);
    EXPECT_EQ(BIT(0) | BIT(4), attr.numaAttr.numaNodes);
}

TEST_F(ManageTest, TestConfigureMigrationTargetsStagesWhileMigrating)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_MIGRATE;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {4, 25, 0};
    attr.migrateMode = MIG_RATIO_MODE;
    attr.remoteNumaCnt = 1;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 25;
    attr.numaAttr.numaNodes = 0x11;
    attr.managedLocalState.managedLocalMask = 0x1;

    ProcessTargetConfig config = {};
    config.migrateMode = MIG_MEMSIZE_MODE;
    config.count = 1;
    config.targets[0] = {5, 0, 4096};
    g_processManager.nrLocalNuma = 4;
    MOCKER(SetLocalNumaByCpu).expects(never());
    MOCKER(GetProcessNumaMapsObservation).expects(never());

    int ret = ConfigureMigrationTargets(&attr, &config);
    EXPECT_EQ(0, ret);
    EXPECT_TRUE(attr.pendingTargetConfigValid);
    EXPECT_FALSE(attr.pendingIgnoreRemoteCapacity);
    EXPECT_EQ(4, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(MIG_RATIO_MODE, attr.migrateMode);
    EXPECT_EQ(25, attr.strategyAttr.initRemoteMemRatio[0][0]);
    EXPECT_EQ(0x1U, attr.managedLocalState.managedLocalMask);
    EXPECT_EQ(5, attr.pendingTargetConfig.targets[0].remoteNid);
    EXPECT_EQ(0x31U, attr.pendingTargetNumaNodes);
}

TEST_F(ManageTest, TestApplyPendingMigrationTargets)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.state = PROC_MIGRATE;
    attr.numaAttr.numaNodes = 0x1;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {4, 25, 0};
    attr.pendingTargetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.pendingTargetConfig.count = 1;
    attr.pendingTargetConfig.targets[0] = {5, 0, 4096};
    attr.pendingTargetConfigValid = true;
    attr.pendingIgnoreRemoteCapacity = true;
    attr.pendingTargetNumaNodes = 0x21;

    g_processManager.nrLocalNuma = 4;
    g_pageSizeNormal = PAGESIZE_4K;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddEmptyCandidateResidentForTest));
    MOCKER(GetPidNumaPagesFromNumaMaps).expects(never());
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(0));
    MOCKER(SyncAllProcessConfig).expects(once()).will(returnValue(0));

    int ret = ApplyPendingMigrationTargets(&attr);
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(attr.pendingTargetConfigValid);
    EXPECT_EQ(0U, attr.pendingTargetConfig.count);
    EXPECT_EQ(MIG_MEMSIZE_MODE, attr.targetConfig.migrateMode);
    EXPECT_EQ(5, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(MIG_MEMSIZE_MODE, attr.migrateMode);
    EXPECT_EQ(1, attr.remoteNumaCnt);
    EXPECT_TRUE(attr.ignoreRemoteCapacity);
    EXPECT_FALSE(attr.pendingIgnoreRemoteCapacity);
    EXPECT_EQ(0x21U, attr.numaAttr.numaNodes);
    EXPECT_EQ(0U, attr.pendingTargetNumaNodes);
}

TEST_F(ManageTest, TestPendingTrackingFailureKeepsActiveTarget)
{
    ProcessAttr attr = {};
    attr.pid = 123;
    attr.numaAttr.numaNodes = 0x11;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {4, 25, 0};
    attr.pendingTargetConfig.migrateMode = MIG_RATIO_MODE;
    attr.pendingTargetConfig.count = 1;
    attr.pendingTargetConfig.targets[0] = {5, 30, 0};
    attr.pendingTargetConfigValid = true;
    attr.pendingTargetNumaNodes = 0x31;
    g_processManager.nrLocalNuma = 4;
    g_pageSizeNormal = PAGESIZE_4K;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddEmptyCandidateResidentForTest));
    MOCKER(GetPidNumaPagesFromNumaMaps).expects(never());
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(-EIO));

    EXPECT_EQ(-EIO, ApplyPendingMigrationTargets(&attr));
    EXPECT_TRUE(attr.pendingTargetConfigValid);
    EXPECT_EQ(4, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(5, attr.pendingTargetConfig.targets[0].remoteNid);
    EXPECT_EQ(0x11U, attr.numaAttr.numaNodes);
    EXPECT_EQ(0x31U, attr.pendingTargetNumaNodes);
}

extern "C" errno_t memset_s(void *dest, size_t destMax, int c, size_t count);
extern "C" int ProcessManagerInit(uint32_t pageType);
extern "C" int EnvMutexInit(EnvMutex *mutex);
TEST_F(ManageTest, TestProcessManagerInit)
{
    uint32_t period;
    int ret = 0;
    uint32_t pageType = PAGETYPE_NORMAL;
    MOCKER(memset_s).stubs().will(returnValue(1));
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestProcessManagerInitTwo)
{
    uint32_t period;
    int ret = 0;
    uint32_t pageType = PAGETYPE_HUGE;
    MOCKER(EnvMutexInit).stubs().will(returnValue(0));
    g_processManager.threadCtx[0] = (void *)&period;
    g_processManager.processes = (ProcessAttr *)&period;
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(nullptr, g_processManager.processes);
}

TEST_F(ManageTest, TestLoadMangerNrProcessNum)
{
    g_processManager.nr[PROCESS_TYPE] = 1;
    int ret = LoadMangerNrProcessNum();
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestLoadMangerNrVmNum)
{
    g_processManager.nr[VM_TYPE] = 1;
    int ret = LoadMangerNrVmNum();
    EXPECT_EQ(1, ret);
}

extern "C" int sscanf_s(const char *buffer, const char *format, ...);
extern "C" int snprintf_s(char *strDest, unsigned long destMax, unsigned long count, const char *format, ...);
extern "C" int access(const char *__name, int __type);
TEST_F(ManageTest, TestPidIsValid)
{
    bool ret;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(access).stubs().will(returnValue(0));
    ret = PidIsValid(1);
    EXPECT_EQ(ret, true);
    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    ret = PidIsValid(1);
    EXPECT_EQ(ret, false);
}

extern "C" int snprintf_s(char *strDest, unsigned long destMax, unsigned long count, const char *format, ...);
extern "C" FILE *fopen(const char *__restrict __filename, const char *__restrict __modes);
extern "C" char *fgets(char *__restrict __s, int __n, FILE *__restrict __stream);
extern "C" int fclose(FILE *__stream);
extern "C" int strncmp(const char *cs, const char *ct, size_t count);
extern "C" int IsQemuTask(pid_t pid);
TEST_F(ManageTest, TestIsQemuTaskPath)
{
    int ret;
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    ret = IsQemuTask(1);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    ret = IsQemuTask(1);
    EXPECT_EQ(-1, ret);
}

TEST_F(ManageTest, TestIsQemuTaskFile)
{
    int ret;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    static FILE fake_file;
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    MOCKER(fgets).stubs().will(returnValue(static_cast<char *>(nullptr)));
    MOCKER(fclose).stubs().will(returnValue(0));
    ret = IsQemuTask(1);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    char buf[] = "1";
    MOCKER(fgets).stubs().will(returnValue(&buf[0]));
    MOCKER(fclose).stubs().will(returnValue(0));
    ret = IsQemuTask(1);
    EXPECT_EQ(0, ret);
}

extern "C" void LinkedListRemove(ProcessAttr **remove, ProcessAttr **head);
extern "C" void FreeProceccesAttr(ProcessAttr *attr);
TEST_F(ManageTest, TestLinkedListRemoveInputNull)
{
    ProcessAttr *remove = nullptr;
    ProcessAttr *head = nullptr;
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, remove);
    EXPECT_EQ(nullptr, head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, remove);
    free(head);

    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head = nullptr;
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, head);
    free(remove);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = nullptr;
    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, head->next);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = nullptr;
    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    LinkedListRemove(&head, &head);
    EXPECT_EQ(nullptr, head);
}

TEST_F(ManageTest, TestLinkedListRemove)
{
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    ProcessAttr *head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    ProcessAttr *middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    ProcessAttr *tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    LinkedListRemove(&middle, &head);
    EXPECT_EQ(middle, nullptr);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    LinkedListRemove(&tail, &head);
    EXPECT_EQ(tail, nullptr);
    EXPECT_EQ(middle->next, nullptr);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    ProcessAttr *attr = head;
    LinkedListRemove(&attr, &head);
    EXPECT_EQ(head, middle);
    free(head);
}

extern "C" int CheckPid(pid_t pid);
TEST_F(ManageTest, TestCheckPid)
{
    int ret;
    pid_t pid;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(PidIsValid).stubs().will(returnValue(false));
    ret = CheckPid(pid);
    EXPECT_EQ(-ESRCH, ret);

    GlobalMockObject::verify();
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(PidIsValid).stubs().will(returnValue(true));
    MOCKER(IsQemuTask).stubs().will(returnValue((int)PROCESS_TYPE));
    ret = CheckPid(pid);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" ProcessAttr *GetProcessAttr(pid_t pid);
TEST_F(ManageTest, TestGetProcessAttr)
{
    ProcessAttr *ret, current;
    g_processManager.processes = &current;
    current.pid = 123;
    ret = GetProcessAttr(123);
    EXPECT_EQ(123, ret->pid);
}

extern "C" ProcessAttr *GetProcessAttrLocked(pid_t pid);
TEST_F(ManageTest, TestGetProcessAttrLocked)
{
    ProcessAttr pid1 = {.pid = 1};
    ProcessAttr pid2 = {.pid = 2, .next = &pid1};
    g_processManager.processes = &pid2;
    ProcessAttr *ret = GetProcessAttrLocked(pid1.pid);
    EXPECT_EQ(pid1.pid, ret->pid);
    g_processManager.processes = nullptr;
}

extern "C" int ParseMmapType(pid_t pid, MmapType *mmapType);
extern "C" int ReadCmdlineByPid(pid_t pid, char *buf, int len);

/* cmdline 无 share 字段(非 shared 虚机)：各参数以 \0 分隔 */
static const char CMD_PRIVATE[] =
    "qemu-system-aarch64\0-name guest=vm1\0-object memory-backend-file,id=ram0,size=2G,mem-path=/dev/hugepages\0";
/* cmdline 含 JSON 形式 \"share\":true(shared 虚机) */
static const char CMD_SHARED[] =
    "qemu-system-aarch64\0-name guest=vm1\0-object {\"qom-type\":\"memory-backend-file\",\"share\":true}\0";

static int MockReadCmdlinePrivate(pid_t pid, char *buf, int len)
{
    (void)pid;
    return memcpy_s(buf, len, CMD_PRIVATE, sizeof(CMD_PRIVATE) - 1) == EOK ? 0 : -EINVAL;
}

static int MockReadCmdlineShared(pid_t pid, char *buf, int len)
{
    (void)pid;
    return memcpy_s(buf, len, CMD_SHARED, sizeof(CMD_SHARED) - 1) == EOK ? 0 : -EINVAL;
}

static int MockReadCmdlineFail(pid_t pid, char *buf, int len)
{
    (void)pid;
    (void)buf;
    (void)len;
    return -EIO;
}

TEST_F(ManageTest, TestParseMmapTypeFailed)
{
    MmapType mmapType = (MmapType)0;
    MOCKER(ReadCmdlineByPid).stubs().will(invoke(MockReadCmdlineFail));
    int ret = ParseMmapType(1, &mmapType);
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ(MMAP_SHARED, mmapType); /* 读失败保守置 SHARED */
}

TEST_F(ManageTest, TestParseMmapTypePrivate)
{
    MmapType mmapType = (MmapType)0;
    MOCKER(ReadCmdlineByPid).stubs().will(invoke(MockReadCmdlinePrivate));
    int ret = ParseMmapType(1, &mmapType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(MMAP_PARIVATE, mmapType);
}

TEST_F(ManageTest, TestParseMmapTypeShared)
{
    MmapType mmapType = (MmapType)0;
    MOCKER(ReadCmdlineByPid).stubs().will(invoke(MockReadCmdlineShared));
    int ret = ParseMmapType(1, &mmapType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(MMAP_SHARED, mmapType);
}

extern "C" int VMPreprocess(pid_t pid, ProcessAttr *attr);
TEST_F(ManageTest, TestVMProcessNormal)
{
    pid_t pid = 1;
    ProcessAttr attr = {};
    int ret;

    /* 非 VM_TYPE，直接返回 0 */
    MOCKER(GetPidType).stubs().will(returnValue(0));
    ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    /* VM_TYPE，ParseMmapType 成功 */
    MOCKER(GetPidType).stubs().will(returnValue(1));
    MOCKER(ParseMmapType).stubs().will(returnValue(0));
    ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestVMProcessParseMmapTypeFailed)
{
    pid_t pid = 1;
    ProcessAttr attr = {};

    /* VM_TYPE 但 ParseMmapType 失败，仍返回 0（保守策略） */
    MOCKER(GetPidType).stubs().will(returnValue(1));
    MOCKER(ParseMmapType).stubs().will(returnValue(-EINVAL));

    int ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(0, ret);
}

extern "C" int SetProcessConfig(ProcessAttr *attr, ProcessParam *param);
TEST_F(ManageTest, TestSetProcessConfig)
{
    g_processManager.nrLocalNuma = 4;
    ProcessAttr attr = {};
    attr.state = PROC_IDLE;
    ProcessParam param = {
        .pid = 1,
        .count = 1,
    };
    param.numaParam[0].nid = 4;
    param.numaParam[0].ratio = 50;
    attr.numaAttr.numaNodes = 1;
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddEmptyCandidateResidentForTest));
    int ret = SetProcessConfig(&attr, &param);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(attr.pid, 1);
    EXPECT_EQ(1U, attr.targetConfig.count);
    EXPECT_EQ(4, attr.targetConfig.targets[0].remoteNid);
    EXPECT_EQ(attr.strategyAttr.initRemoteMemRatio[0][0], 50);
    EXPECT_EQ(attr.numaAttr.numaNodes, 17);
}

extern "C" FILE *OpenNumaMaps(pid_t pid);
TEST_F(ManageTest, TestOpenNumaMaps)
{
    int pid = 1;
    MOCKER(fopen).stubs().will(returnValue(reinterpret_cast<FILE *>(0x1234)));

    FILE *ret = OpenNumaMaps(pid);
    EXPECT_NE(ret, nullptr);
}

extern "C" int GetPidNumaPagesFromNumaMaps(pid_t pid, uint64_t numaPages[MAX_NODES], bool onlyHuge);
TEST_F(ManageTest, TestGetPidNumaPagesFromNumaMapsOpenFailure)
{
    uint64_t numaPages[MAX_NODES] = {0};
    MOCKER(OpenNumaMaps).expects(once()).will(returnValue(static_cast<FILE *>(nullptr)));
    int ret = GetPidNumaPagesFromNumaMaps(1234, numaPages, false);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" bool IsNumaMapLineHuge(char *line);
TEST_F(ManageTest, TestIsNumaMapLineHuge)
{
    char line[] = "abcdesgsasdfskernelpagesize_kB=2048";
    bool ret = IsNumaMapLineHuge(line);
    EXPECT_EQ(ret, true);
}

extern "C" void SetLocalByNumaMaps(char *line, uint32_t *nodeBitmap, bool hugeFlag);
TEST_F(ManageTest, TestSetLocalByNumaMaps)
{
    char line[] = "00100000 N0=1 N2=3 kernelpagesize_kB=2048";
    uint32_t nodeBitmap = 0;
    SetLocalByNumaMaps(line, &nodeBitmap, true);
    EXPECT_EQ(nodeBitmap, BIT(0) | BIT(2));
}

extern "C" int SetProcessLocalNuma(pid_t pid, uint32_t *nodeBitmap, bool hugeFlag);
extern "C" int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
TEST_F(ManageTest, TestSetProcessLocalNuma)
{
    int pid = 1;
    uint32_t nodeBitmap = 0;
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    CPU_SET(2, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).stubs().will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).stubs().will(returnValue(1)).then(returnValue(2));
    int ret = SetProcessLocalNuma(pid, &nodeBitmap, true);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(nodeBitmap, BIT(1) | BIT(2));
}

static int AddAffinityLocalForTest(pid_t pid, uint32_t *nodeBitmap)
{
    (void)pid;
    *nodeBitmap |= BIT(0);
    return 0;
}

static int AddCandidateResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                       uint64_t numaPages[MAX_NODES])
{
    (void)pid;
    EXPECT_FALSE(hugeFlag);
    *residentLocalMask |= BIT(2);
    numaPages[2] = 10;
    return 0;
}

static int AddEmptyCandidateResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                            uint64_t numaPages[MAX_NODES])
{
    (void)pid;
    (void)hugeFlag;
    (void)residentLocalMask;
    (void)numaPages;
    return 0;
}

static int AddUnexpectedRemoteResidentForTest(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                              uint64_t numaPages[MAX_NODES])
{
    (void)pid;
    (void)hugeFlag;
    *residentLocalMask |= BIT(0);
    numaPages[0] = 10;
    numaPages[5] = 1;
    return 0;
}

static ProcessParam InitCandidateTest(ProcessAttr *active, int nrLocalNuma = 4)
{
    active->pid = 123;
    active->type = PROCESS_TYPE;
    g_processManager.processes = active;
    g_processManager.nrLocalNuma = nrLocalNuma;
    return ProcessParam{
        .pid = active->pid,
        .scanType = NORMAL_SCAN,
        .count = 0,
    };
}

TEST_F(ManageTest, TestSetProcessLocalNumaRejectsUnavailableObservations)
{
    uint32_t nodeBitmap = 0;
    MOCKER(SetLocalNumaByCpu).expects(once()).will(returnValue(-EIO));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(returnValue(-EINVAL));

    EXPECT_EQ(-EIO, SetProcessLocalNuma(123, &nodeBitmap, false));
    EXPECT_EQ(0U, nodeBitmap);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateSamplesOnce)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active);
    param.ignoreRemoteCapacity = true;
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddCandidateResidentForTest));
    MOCKER(GetPidNumaPagesFromNumaMaps).expects(never());
    MOCKER(SyncAllProcessConfig).expects(once()).will(returnValue(0));

    ASSERT_EQ(0, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    ASSERT_NE(nullptr, candidate.prepared);
    EXPECT_EQ(0U, active.managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(2), candidate.prepared->managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(0) | BIT(2), candidate.prepared->numaAttr.numaNodes);
    EXPECT_TRUE(candidate.prepared->ignoreRemoteCapacity);

    PublishProcessManageCandidate(&candidate);
    EXPECT_EQ(BIT(0) | BIT(2), active.managedLocalState.managedLocalMask);
    EXPECT_TRUE(active.ignoreRemoteCapacity);
    EXPECT_EQ(nullptr, candidate.prepared);
}

TEST_F(ManageTest, TestPublishProcessManageCandidateStagesCapacityBypass)
{
    ProcessAttr active = {};
    ProcessAttr *prepared = (ProcessAttr *)calloc(1, sizeof(ProcessAttr));
    ASSERT_NE(nullptr, prepared);
    active.pid = 123;
    prepared->pid = active.pid;
    prepared->pendingTargetConfig.migrateMode = MIG_MEMSIZE_MODE;
    prepared->pendingTargetConfig.count = 1;
    prepared->pendingTargetConfig.targets[0] = {4, 0, 2048};
    prepared->pendingTargetConfigValid = true;
    prepared->pendingIgnoreRemoteCapacity = true;
    prepared->pendingTargetNumaNodes = BIT(0) | BIT(4);
    ProcessManageCandidate candidate = {
        .active = &active,
        .prepared = prepared,
        .isPending = true,
    };

    MOCKER(SyncAllProcessConfig).expects(once()).will(returnValue(0));

    PublishProcessManageCandidate(&candidate);
    EXPECT_TRUE(active.pendingTargetConfigValid);
    EXPECT_TRUE(active.pendingIgnoreRemoteCapacity);
    EXPECT_EQ(4, active.pendingTargetConfig.targets[0].remoteNid);
    EXPECT_EQ(BIT(0) | BIT(4), active.pendingTargetNumaNodes);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateAffinityFailureUsesResident)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active);
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(returnValue(-EIO));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddCandidateResidentForTest));

    ASSERT_EQ(0, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    ASSERT_NE(nullptr, candidate.prepared);
    EXPECT_EQ(BIT(2), candidate.prepared->managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(2), candidate.prepared->numaAttr.numaNodes);
    DiscardProcessManageCandidate(&candidate);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateResidentFailureUsesAffinity)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active);
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(returnValue(-EIO));

    ASSERT_EQ(0, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    ASSERT_NE(nullptr, candidate.prepared);
    EXPECT_EQ(BIT(0), candidate.prepared->managedLocalState.managedLocalMask);
    EXPECT_EQ(BIT(0), candidate.prepared->numaAttr.numaNodes);
    DiscardProcessManageCandidate(&candidate);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateEmptyObservationUsesAllLocal)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active, 3);
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(returnValue(0));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddEmptyCandidateResidentForTest));

    ASSERT_EQ(0, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    ASSERT_NE(nullptr, candidate.prepared);
    EXPECT_EQ(0x7U, candidate.prepared->managedLocalState.managedLocalMask);
    EXPECT_EQ(0x7U, candidate.prepared->numaAttr.numaNodes);
    DiscardProcessManageCandidate(&candidate);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateRejectsNoObservation)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active);
    active.numaAttr.numaNodes = BIT(0);
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(returnValue(-EIO));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(returnValue(-EINVAL));

    EXPECT_EQ(-EIO, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    EXPECT_EQ(nullptr, candidate.prepared);
    EXPECT_EQ(BIT(0), active.numaAttr.numaNodes);
}

TEST_F(ManageTest, TestPrepareProcessManageCandidateRejectsUnexpectedRemote)
{
    ProcessAttr active = {};
    ProcessParam param = InitCandidateTest(&active);
    param.count = 1;
    param.numaParam[0].nid = 4;
    param.numaParam[0].migrateMode = MIG_RATIO_MODE;
    ProcessManageCandidate candidate = {};

    MOCKER(CheckPid).expects(once()).will(returnValue(0));
    MOCKER(SetLocalNumaByCpu).expects(once()).will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).expects(once()).will(invoke(AddUnexpectedRemoteResidentForTest));

    EXPECT_EQ(-EINVAL, PrepareProcessManageCandidate(&param, PROCESS_TYPE, &candidate));
    EXPECT_EQ(nullptr, candidate.prepared);
}

extern "C" int ProcessAddManage(ProcessParam *param, uint32_t *nodeBitmap);
extern "C" int IsQemuTask(pid_t pid);
TEST_F(ManageTest, TestProcessAddManageResetPidConfig)
{
    int ret;
    pid_t pid = 123;
    uint32_t localNodeBitmap = 1;
    ProcessAttr mockProcess = {};
    mockProcess.pid = pid;
    mockProcess.duration = 100;
    mockProcess.scanTime = 50;
    mockProcess.numaAttr.numaNodes = 31;

    g_processManager.processes = &mockProcess;
    g_processManager.nrLocalNuma = 4;
    ProcessParam param = {
        .pid = pid,
        .scanTime = 100,
        .duration = 100,
        .count = 1,
    };
    param.numaParam[0].nid = 5;
    param.numaParam[0].ratio = 50;
    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&mockProcess));
    MOCKER(SetLocalNumaByCpu).stubs().will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).stubs().will(invoke(AddEmptyCandidateResidentForTest));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);

    mockProcess.numaAttr.numaNodes = 47;
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(mockProcess.scanTime, g_processManager.processes->scanTime);
    EXPECT_EQ(mockProcess.duration, g_processManager.processes->duration);
    EXPECT_EQ(50., g_processManager.processes->initLocalMemRatio);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestProcessAddManageNewPid)
{
    int ret;
    ProcessParam param = {
        .pid = 123,
        .scanTime = 50,
        .duration = 1,
        .scanType = NORMAL_SCAN,
        .count = 1,
    };
    param.numaParam[0].nid = 4;
    param.numaParam[0].ratio = 50;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    g_processManager.nrLocalNuma = 4;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(0));
    MOCKER(GetPidNrPages).stubs().will(returnValue(0x100));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(SetLocalNumaByCpu).stubs().will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).stubs().will(invoke(AddEmptyCandidateResidentForTest));

    ret = ProcessAddManage(&param, nullptr);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD, g_processManager.processes->scanTime); // 首次扫描使用DEFAULT_SCAN_PERIOD
    EXPECT_EQ(param.duration, g_processManager.processes->duration);
    EXPECT_EQ(50, g_processManager.processes->initLocalMemRatio);
    EXPECT_EQ(0x11, g_processManager.processes->numaAttr.numaNodes);

    // when scanType is HAM_SCAN/STATISTIC_SCAN, state should be set to PROC_MOVE
    // when nodeBitmap is not null, local numanodes should be updated
    uint32_t localNodeBitmap = 1;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    param.scanType = HAM_SCAN;
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD,
              g_processManager.processes->scanTime); // 首次扫描使用DEFAULT_SCAN_PERIOD
    EXPECT_EQ(param.duration, g_processManager.processes->duration);
    EXPECT_EQ(50, g_processManager.processes->initLocalMemRatio);
    EXPECT_EQ(0x11, g_processManager.processes->numaAttr.numaNodes);
    EXPECT_EQ(PROC_MOVE, g_processManager.processes->state);
}

TEST_F(ManageTest, TestProcessAddManageNewPidFailed)
{
    int ret;
    ProcessParam param = {
        .pid = 123,
        .scanType = NORMAL_SCAN,
        .count = 1,
    };
    param.numaParam[0].nid = 1;
    param.numaParam[0].ratio = 50;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(-EINVAL));
    ret = ProcessAddManage(&param, nullptr);
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
}

static void FillPolicyForManageTest(GroupMigrationPolicy *policy)
{
    policy->enabled = true;
    policy->groupCount = 1;
    policy->groups[0].localCount = 1;
    policy->groups[0].locals[0].nid = 0;
    policy->groups[0].locals[0].localReservePages = 2;
    policy->groups[0].targetCount = 1;
    policy->groups[0].targets[0].nid = 4;
    policy->groups[0].targets[0].quotaPages = 8;
    policy->groups[0].targets[0].usedPages = 3;
}

extern "C" int ProcessAddGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);
TEST_F(ManageTest, TestProcessAddGroupedManageNewPid)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));

    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(0, ret);
    ASSERT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_EQ(1234, g_processManager.processes->pid);
    EXPECT_EQ((uint32_t)0x11, g_processManager.processes->numaAttr.numaNodes);
    EXPECT_TRUE(g_processManager.processes->groupPolicy.enabled);
    EXPECT_EQ((uint64_t)2, g_processManager.processes->groupPolicy.groups[0].locals[0].localReservePages);
    EXPECT_EQ((uint64_t)3, g_processManager.processes->groupPolicy.groups[0].targets[0].usedPages);

    free(g_processManager.processes);
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
}

TEST_F(ManageTest, TestProcessAddGroupedManageUpdateExistingPid)
{
    ProcessAttr current = {};
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);
    policy.groups[0].targets[0].usedPages = 6;

    current.pid = 1234;
    current.next = nullptr;
    current.pendingGroupPolicy.valid = true;
    g_processManager.processes = &current;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));

    int ret = ProcessAddGroupedManage(1234, 0x21, &policy);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1234, current.pid);
    EXPECT_EQ((uint32_t)0x21, current.numaAttr.numaNodes);
    EXPECT_TRUE(current.groupPolicy.enabled);
    EXPECT_FALSE(current.pendingGroupPolicy.valid);
    EXPECT_EQ((uint64_t)6, current.groupPolicy.groups[0].targets[0].usedPages);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestProcessAddGroupedManageRejectsInvalidInputs)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    MOCKER(CheckPid).stubs().will(returnValue(-EINVAL));
    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(CheckPid).stubs().will(returnValue(0));
    ret = ProcessAddGroupedManage(1234, 0x11, nullptr);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    policy.enabled = false;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" int ProcessSetPendingGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);
TEST_F(ManageTest, TestProcessSetPendingGroupedManage)
{
    ProcessAttr current = {};
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    current.pid = 1234;
    current.state = PROC_MIGRATE;
    current.groupPolicy.enabled = true;
    g_processManager.processes = &current;

    int ret = ProcessSetPendingGroupedManage(1234, 0x31, &policy);
    EXPECT_EQ(0, ret);
    EXPECT_TRUE(current.pendingGroupPolicy.valid);
    EXPECT_EQ((uint32_t)0x31, current.pendingGroupPolicy.nodeBitmap);
    EXPECT_EQ((uint64_t)3, current.pendingGroupPolicy.policy.groups[0].targets[0].usedPages);

    policy.groups[0].targets[0].usedPages = 6;
    ret = ProcessSetPendingGroupedManage(1234, 0x41, &policy);
    EXPECT_EQ(0, ret);
    EXPECT_EQ((uint32_t)0x41, current.pendingGroupPolicy.nodeBitmap);
    EXPECT_EQ((uint64_t)6, current.pendingGroupPolicy.policy.groups[0].targets[0].usedPages);

    current.state = PROC_IDLE;
    ret = ProcessSetPendingGroupedManage(1234, 0x31, &policy);
    EXPECT_EQ(-EINVAL, ret);
    g_processManager.processes = nullptr;
}

static int FillPendingGroupedNumaPages(pid_t pid, uint64_t numaPages[MAX_NODES], bool onlyHuge)
{
    (void)pid;
    (void)onlyHuge;
    numaPages[4] = 2;
    return 0;
}

extern "C" int ApplyPendingGroupedPolicy(ProcessAttr *attr);
TEST_F(ManageTest, TestApplyPendingGroupedPolicy)
{
    ProcessAttr current = {};
    GroupMigrationPolicy active = {};
    GroupMigrationPolicy pending = {};
    FillPolicyForManageTest(&active);
    FillPolicyForManageTest(&pending);
    pending.groups[0].targets[0].usedPages = 6;
    pending.groups[0].swapCandidateRounds = 3;

    current.pid = 1234;
    current.groupPolicy = active;
    current.pendingGroupPolicy.valid = true;
    current.pendingGroupPolicy.nodeBitmap = 0x31;
    current.pendingGroupPolicy.policy = pending;
    g_processManager.nrLocalNuma = 4;
    MOCKER(GetPidNumaPagesFromNumaMaps).expects(once()).will(invoke(FillPendingGroupedNumaPages));
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(0));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));

    int ret = ApplyPendingGroupedPolicy(&current);
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(current.pendingGroupPolicy.valid);
    EXPECT_EQ((uint32_t)0x31, current.numaAttr.numaNodes);
    EXPECT_EQ((uint64_t)2, current.groupPolicy.groups[0].targets[0].usedPages);
    EXPECT_EQ((uint8_t)0, current.groupPolicy.groups[0].swapCandidateRounds);
}

TEST_F(ManageTest, TestProcessAddGroupedManageRejectsLimitAndPreprocessFailure)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = MAX_2M_PROCESSES_CNT;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(-ENOMEM));
    ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-ENOMEM, ret);
    EXPECT_EQ(nullptr, g_processManager.processes);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
}

extern "C" void CalcActcStats(ProcessAttr *attr);
extern "C" uint32_t GetRemoteHotThreshold(void);
TEST_F(ManageTest, TestCalcActcStats)
{
    ProcessAttr attr = {};
    ActcData data[3] = {};

    data[0].freq = 0;
    data[1].freq = 3;
    data[1].isWhiteListPage = true;
    data[2].freq = 10;
    attr.scanAttr.actcData[4] = data;
    attr.scanAttr.actcLen[4] = 3;
    attr.scanAttr.actCount[1].pageNum = 99;
    MOCKER(GetRemoteHotThreshold).stubs().will(returnValue((uint32_t)5));

    CalcActcStats(&attr);
    EXPECT_EQ((uint64_t)0, attr.scanAttr.actCount[1].pageNum);
    EXPECT_EQ((uint64_t)3, attr.scanAttr.actCount[4].pageNum);
    EXPECT_EQ((uint8_t)10, attr.scanAttr.actCount[4].freqMax);
    EXPECT_EQ((uint8_t)0, attr.scanAttr.actCount[4].freqMin);
    EXPECT_EQ((uint64_t)2, attr.scanAttr.actCount[4].freqNum);
    EXPECT_EQ((uint64_t)13, attr.scanAttr.actCount[4].freqSum);
    EXPECT_EQ((uint32_t)1, attr.scanAttr.actCount[4].freqZero);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actCount[4].remoteHotNum);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actCount[4].whiteNum);
}

extern "C" int DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf);
TEST_F(ManageTest, TestDistributeActcData)
{
    ProcessAttr attr = {};
    struct ProcessMemBitmap pmb = {};
    ActcData oldData[1] = {};
    ActcData buf[3] = {};

    attr.pid = 1234;
    attr.scanAttr.actcData[0] = (ActcData *)malloc(sizeof(ActcData));
    ASSERT_NE(nullptr, attr.scanAttr.actcData[0]);
    attr.scanAttr.actcData[0][0] = oldData[0];
    pmb.nrPages[0] = 2;
    pmb.nrPages[2] = 1;

    int ret = DistributeActcData(&attr, &pmb, buf);
    EXPECT_EQ(0, ret);
    ASSERT_NE(nullptr, attr.scanAttr.actcData[0]);
    ASSERT_NE(nullptr, attr.scanAttr.actcData[2]);
    EXPECT_EQ((uint64_t)2, attr.scanAttr.actcLen[0]);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actcLen[2]);

    free(attr.scanAttr.actcData[0]);
    free(attr.scanAttr.actcData[2]);
    attr.scanAttr.actcData[0] = nullptr;
    attr.scanAttr.actcData[2] = nullptr;
}

TEST_F(ManageTest, TestCheckAndRemoveInvalidProcess)
{
    ProcessAttr attr = {.pid = 1025};
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = &attr;
    g_processManager.nr[VM_TYPE] = 2;
    MOCKER(PidIsValid).stubs().will(returnValue(false));
    MOCKER(AccessIoctlRemovePid).expects(once()).will(returnValue(0));
    MOCKER(LinkedListRemove).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    CheckAndRemoveInvalidProcess();
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
}

TEST_F(ManageTest, TestCheckAndRemoveInvalidProcessTwo)
{
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = nullptr;
    CheckAndRemoveInvalidProcess();
    EXPECT_EQ(0, g_processManager.remoteNumaInfo.usedInfo[0].used);
}

extern "C" void CalRemoteMemUsed(void);
TEST_F(ManageTest, TestCalRemoteMemUsed)
{
    ProcessAttr attr;
    attr.next = nullptr;
    g_processManager.processes = &attr;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes->walkPage.nrPages[4] = 1;
    CalRemoteMemUsed();
    EXPECT_EQ(1, g_processManager.remoteNumaInfo.usedInfo[0].used);
    g_processManager.processes = nullptr;
}

extern "C" void RemoveManagedProcess(int nr, pid_t *pidArr);
extern "C" struct ProcessManager g_processManager;
extern "C" PidType GetPidType(struct ProcessManager *manager);
TEST_F(ManageTest, TestRemoveManagedProcessInvalidPid)
{
    pid_t pid = 123;
    ProcessAttr mockProcess;
    mockProcess.pid = pid;
    mockProcess.next = nullptr;

    g_processManager.processes = &mockProcess;
    g_processManager.nr[PROCESS_TYPE] = 1;
    pid_t pidArr[1] = {1};

    MOCKER(GetPidType).stubs().will(returnValue(PROCESS_TYPE));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(1, g_processManager.nr[PROCESS_TYPE]);
}

TEST_F(ManageTest, TestRemoveManagedProcessValidPid)
{
    pid_t pid = 123;
    ProcessAttr mockProcess;
    mockProcess.pid = pid;
    mockProcess.next = nullptr;
    g_processManager.processes = &mockProcess;
    pid_t pidArr[1] = {pid};
    int ret;

    g_processManager.nr[VM_TYPE] = 1;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
    EXPECT_EQ(g_processManager.processes, nullptr);

    GlobalMockObject::verify();
    g_processManager.processes = &mockProcess;
    g_processManager.nr[PROCESS_TYPE] = 1;
    MOCKER(GetPidType).stubs().will(returnValue(PROCESS_TYPE));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(0, g_processManager.nr[PROCESS_TYPE]);
    EXPECT_EQ(g_processManager.processes, nullptr);
}

extern "C" void RemoveAllManagedProcess(void);
TEST_F(ManageTest, TestRemoveAllManagedProcess)
{
    int ret;

    pid_t pid = 123;

    EnvMutexInit(&g_processManager.lock);

    ProcessAttr *mockProcess = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    mockProcess->pid = pid;
    mockProcess->next = nullptr;

    MOCKER(AccessIoctlRemoveAllPid).stubs().will(returnValue(0));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    g_processManager.processes = mockProcess;
    RemoveAllManagedProcess();
    EXPECT_EQ(g_processManager.processes, nullptr);
    EXPECT_EQ(g_processManager.nr[VM_TYPE], 0);
    EXPECT_EQ(g_processManager.nr[PROCESS_TYPE], 0);

    g_processManager.processes = nullptr;
    RemoveAllManagedProcess();
    EXPECT_EQ(g_processManager.processes, nullptr);
    EXPECT_EQ(g_processManager.nr[VM_TYPE], 0);
    EXPECT_EQ(g_processManager.nr[PROCESS_TYPE], 0);
}

extern "C" int DestroyProcessManager();
TEST_F(ManageTest, TestDestroyProcessManager)
{
    int ret;
    MOCKER(memset_s).stubs().will(ignoreReturnValue());
    ret = DestroyProcessManager();
    EXPECT_EQ(0, ret);
}

extern "C" struct ProcessManager *GetProcessManager(void);
TEST_F(ManageTest, TestGetProcessManager)
{
    struct ProcessManager *ret = GetProcessManager();
    EXPECT_EQ(&g_processManager, ret);
}

extern "C" void ResetActcData(ActcData *actcData[], int len);
TEST_F(ManageTest, TestResetActcData)
{
    int len = 10;
    ActcData **data = (ActcData **)malloc(sizeof(ActcData *) * len);
    for (int i = 0; i < len; i++) {
        data[i] = (ActcData *)malloc(sizeof(ActcData));
    }
    ResetActcData(data, len);
    EXPECT_EQ(data[0], nullptr);
    for (int i = 0; i < len; i++) {
        free(data[i]);
    }
    free(data);
}

extern "C" errno_t memcpy_s(void *dest, size_t numberOfElements, const void *src, size_t count);

TEST_F(ManageTest, TestFreeProceccesAttr)
{
    ProcessAttr *attr = nullptr;

    MOCKER(ResetActcData).expects(never());
    FreeProceccesAttr(attr);
    GlobalMockObject::verify();

    attr = (ProcessAttr *)malloc(sizeof(*attr));
    ASSERT_NE(nullptr, attr);
    MOCKER(ResetActcData).stubs().will(ignoreReturnValue());
    FreeProceccesAttr(attr);
}

extern "C" unsigned long ProcessSmapsFile(pid_t pid, const char *targetLinePrefix, size_t prefixLength, size_t divisor);
TEST_F(ManageTest, TestProcessSmapsFile)
{
    int pid = 1234;
    char targetLinePrefix[] = "test";
    size_t prefixLength = 0;
    size_t divisor = 1;
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1))
        .then(returnValue(0));
    unsigned long ret = ProcessSmapsFile(pid, targetLinePrefix, prefixLength, divisor);
    EXPECT_EQ(ret, 0);
    static FILE fake_file;
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    char buf[] = "1";
    MOCKER(fgets).stubs().will(returnValue(&buf[0])).then(returnValue((static_cast<char *>(nullptr))));
    MOCKER(fclose).stubs().will(returnValue(1));
    MOCKER((int (*)(char const *, char const *, void *))sscanf_s).stubs().will(returnValue(0));
    ret = ProcessSmapsFile(pid, targetLinePrefix, prefixLength, divisor);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetNormalPageCount(pid_t pid);
TEST_F(ManageTest, TestGetNormalPageCount)
{
    int pid = 1234;
    unsigned long ret = GetNormalPageCount(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetHugePageCount(pid_t pid);
TEST_F(ManageTest, TestGetHugePageCount)
{
    int pid = 1234;
    unsigned long ret = GetHugePageCount(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetPidNrPages(pid_t pid);
TEST_F(ManageTest, TestGetPidNrPages)
{
    int pid = 1234;
    unsigned long ret = GetPidNrPages(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" int GetNodeFromCpu(int cpu);
TEST_F(ManageTest, TestGetNodeFromCpu)
{
    int cpu = 1234;
    int ret = GetNodeFromCpu(cpu);
    EXPECT_EQ(ret, -EINVAL);
    MOCKER(access).stubs().will(returnValue(0));
    ret = GetNodeFromCpu(cpu);
    EXPECT_EQ(ret, 0);
}

#ifdef CPU_ISSET
#undef CPU_ISSET
#endif
#define CPU_ISSET(x, y) ((x) == 0 ? 1 : 0)
TEST_F(ManageTest, TestGetNumaNodesForPid)
{
    int pid = 1234;
    int node = 0;
    MOCKER(sched_getaffinity).stubs().will(returnValue(-1)).then(returnValue(0));
    MOCKER(access).stubs().will(returnValue(-1)).then(returnValue(0));
    int ret = GetNumaNodesForPid(pid, &node);
    EXPECT_EQ(ret, -EINVAL);
    ret = GetNumaNodesForPid(pid, &node);
    EXPECT_EQ(ret, 0);
}

extern "C" bool IsHugeAligned(uint64_t addr);
TEST_F(ManageTest, TestIsHugeAligned)
{
    uint64_t addr = 0;
    bool ret = IsHugeAligned(addr);
    EXPECT_EQ(ret, true);
}

TEST_F(ManageTest, TestIsHugePageRange)
{
    const char *line = "hugepage";
    int ret = IsHugePageRange(line);
    EXPECT_EQ(ret, 1);
}

extern "C" ssize_t read(int fd, void *buf, size_t count);

extern "C" int open(const char *pathname, int flags);

extern "C" void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len);
TEST_F(ManageTest, TestSetPidNrPages)
{
    ProcessAttr *attr = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    size_t *nrPages = (size_t *)malloc(sizeof(size_t) * 2);
    nrPages[0] = 1;
    nrPages[1] = 1;
    SetPidNrPages(attr, nrPages, 2);
    EXPECT_EQ(1, attr->walkPage.nrPages[0]);
    EXPECT_EQ(1, attr->walkPage.nrPages[1]);
    EXPECT_EQ(2, attr->walkPage.nrPage);
    free(attr);
    free(nrPages);
}

extern "C" void ClearRemoteMemUsed();
TEST_F(ManageTest, TestClearRemoteMemUsed)
{
    g_processManager.remoteNumaInfo.usedInfo[1].used = 10;
    ClearRemoteMemUsed();
    EXPECT_EQ(0, g_processManager.remoteNumaInfo.usedInfo[1].used);
}

extern "C" uint64_t MBToPage(uint64_t size);
TEST_F(ManageTest, TestCalcRemoteBorrowPages)
{
    uint32_t ret;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = MBToPage(100);
    EXPECT_EQ(25600, ret);

    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = MBToPage(100);
    EXPECT_EQ(50, ret);
}

extern "C" void NoAccountAlloc(int remoteNid, ProcessAttr *attr);
TEST_F(ManageTest, TestNoAccountAlloc)
{
    ProcessAttr attr;
    attr.walkPage.nrPages[1] = 1;
    attr.numaAttr.numaNodes = 0b00010001;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    NoAccountAlloc(1, &attr);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[0][0]);
}

TEST_F(ManageTest, TestNoAccountAllocLocalNumaLen2)
{
    ProcessAttr attr;
    attr.walkPage.nrPages[2] = 2;
    attr.numaAttr.numaNodes = 0b00010011;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    NoAccountAlloc(2, &attr);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[0][0]);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[1][0]);
}

extern "C" void CalRemotePerLocalWithAccount(int j, ProcessAttr *attr);
TEST_F(ManageTest, TestCalRemotePerLocalWithAccount)
{
    uint32_t ret;
    ProcessAttr attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 0;
    attr.strategyAttr.allocRemoteNrPages[0][0] = 0;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    MOCKER(InAttrL1).stubs().will(returnValue(true));
    CalRemotePerLocalWithAccount(2, &attr);
    EXPECT_EQ(0, attr.strategyAttr.allocRemoteNrPages[0][0]);
}

extern "C" void CalNrPagesLocalTotalPerPid(ProcessAttr *attr);
TEST_F(ManageTest, TestCalNrPagesLocalTotalPerPid)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 2;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 4;
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][0] = 6;
    attr.strategyAttr.remoteNrPagesAfterMigrate[3][0] = 8;

    attr.walkPage.nrPages[0] = 2;
    attr.walkPage.nrPages[1] = 4;
    attr.walkPage.nrPages[2] = 6;
    attr.walkPage.nrPages[3] = 8;

    CalNrPagesLocalTotalPerPid(&attr);
    EXPECT_EQ(2, attr.strategyAttr.nrPagesPerLocalNuma[0]);
    EXPECT_EQ(4, attr.strategyAttr.nrPagesPerLocalNuma[1]);
    EXPECT_EQ(6, attr.strategyAttr.nrPagesPerLocalNuma[2]);
    EXPECT_EQ(8, attr.strategyAttr.nrPagesPerLocalNuma[3]);
}

TEST_F(ManageTest, TestCalNrPagesLocalTotalPerPidTwo)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 2;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][2] = 4;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][3] = 6;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][4] = 8;

    attr.walkPage.nrPages[0] = 1;
    attr.walkPage.nrPages[1] = 2;
    attr.walkPage.nrPages[2] = 3;
    attr.walkPage.nrPages[3] = 4;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    CalNrPagesLocalTotalPerPid(&attr);
    EXPECT_EQ(1, attr.strategyAttr.nrPagesPerLocalNuma[0]);
    EXPECT_EQ(2, attr.strategyAttr.nrPagesPerLocalNuma[1]);
    EXPECT_EQ(3, attr.strategyAttr.nrPagesPerLocalNuma[2]);
    EXPECT_EQ(4, attr.strategyAttr.nrPagesPerLocalNuma[3]);
}

extern "C" void CalNrPagesLocalTotal(void);
TEST_F(ManageTest, TestCalNrPagesLocalTotal)
{
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;
    g_processManager.processes->strategyAttr.remoteNrPagesAfterMigrate[0][0] = 2;
    g_processManager.processes->walkPage.nrPages[0] = 2;
    CalNrPagesLocalTotal();
    EXPECT_EQ(2, g_processManager.processes->strategyAttr.nrPagesPerLocalNuma[0]);
}

extern "C" void CalRemoteNumaAllocPerPid(int i, int j, uint32_t tmpNrPagesToUse,
                                         uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM]);
TEST_F(ManageTest, TestCalRemoteNumaAllocPerPid)
{
    uint32_t tmp[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};

    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nr[0] = 100;
    tmp[0][0] = 100;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;
    attr.strategyAttr.l2RemoteMemRatio[0][0] = 20;

    CalRemoteNumaAllocPerPid(0, 0, 100, tmp);

    EXPECT_EQ(50, attr.strategyAttr.l2RemoteMemRatio[0][0]);
}

extern "C" void CalRemoteNumaSizeAllocPerNuma(void);
TEST_F(ManageTest, TestCalRemoteNumaSizeAllocPerNuma)
{
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.remoteNumaInfo.privateSize[0][0] = 300;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;

    CalRemoteNumaSizeAllocPerNuma();
    EXPECT_EQ(50, g_processManager.processes->strategyAttr.l2RemoteMemRatio[0][0]);
}

extern "C" void CalcMigrateNrPagesPerPIDMuiltNuma(void);
TEST_F(ManageTest, TestCalcMigrateNrPagesPerPIDMuiltNuma)
{
    g_runMode = WATERLINE_MODE;
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 300;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;

    CalcMigrateNrPagesPerPIDMuiltNuma();
    EXPECT_EQ(0, g_processManager.processes->strategyAttr.l2RemoteMemRatio[0][0]);
}

TEST_F(ManageTest, TestSetRemoteNumaInfo)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
        }
    }
    g_processManager.nrLocalNuma = 4;
    ASSERT_EQ(0, borrowMem->usedInfo[1].size);
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(0));
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(50, borrowMem->usedInfo[1].size);
    borrowMem->usedInfo[1].size = 0;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100 << 8, borrowMem->usedInfo[1].size);
    borrowMem->usedInfo[1].size = 0;
}

TEST_F(ManageTest, TestSetRemoteNumaInfoError)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
        }
    }
    g_processManager.nrLocalNuma = 4;
    ASSERT_EQ(0, borrowMem->usedInfo[1].size);
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(-EINVAL));
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(-EBADF, ret);
    EXPECT_EQ(0, borrowMem->usedInfo[1].size);
}

TEST_F(ManageTest, TestSetRemoteNumaInfoShared)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
            borrowMem->sharedSize[j] = 0;
            borrowMem->privateSize[i][j] = 0;
        }
    }
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(0));
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = SetRemoteNumaInfo(NUMA_NO_NODE, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, borrowMem->sharedSize[1]);
    EXPECT_EQ(50, borrowMem->usedInfo[1].size);
    borrowMem->sharedSize[1] = 0;
    borrowMem->usedInfo[1].size = 0;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = SetRemoteNumaInfo(NUMA_NO_NODE, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, borrowMem->sharedSize[1]);
    EXPECT_EQ(100 << 8, borrowMem->usedInfo[1].size);
    borrowMem->sharedSize[1] = 0;
    borrowMem->usedInfo[1].size = 0;
}

TEST_F(ManageTest, TestSetRemoteNumaInfoInvalidDestNid)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        borrowMem->usedInfo[j].size = 0;
        borrowMem->sharedSize[j] = 0;
    }
    ret = SetRemoteNumaInfo(0, 2, 100);
    EXPECT_EQ(-EBADF, ret);
    EXPECT_EQ(0, borrowMem->privateSize[0][2]);
}

TEST_F(ManageTest, TestSetRemoteNumaInfoInvalidSrcNid)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        borrowMem->usedInfo[j].size = 0;
    }
    ret = SetRemoteNumaInfo(5, 5, 100);
    EXPECT_EQ(-EBADF, ret);
}

extern "C" bool CheckBorrowUsed(int destNid);
TEST_F(ManageTest, TestCheckBorrowUsed)
{
    bool ret;
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.usedInfo[0].used = 10;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 5;
    MOCKER(EnvMsleep).stubs();
    ret = CheckBorrowUsed(4);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestCheckBorrowUsedTwo)
{
    bool ret;
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo.usedInfo[0].used = 1;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 5;
    g_processManager.remoteNumaInfo.usedInfo[0].ifUsedFreshed = false;
    MOCKER(EnvMsleep).stubs();
    ret = CheckBorrowUsed(1);
    EXPECT_EQ(false, ret);
}

extern "C" bool CheckPrivateBorrowUsed(int destNid);
TEST_F(ManageTest, CheckPrivateBorrowUsedWithoutNuma)
{
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo = {};
    bool ret = CheckPrivateBorrowUsed(2);
    EXPECT_EQ(false, ret);
}

extern "C" bool CheckPrivateBorrowUsed(int destNid);
TEST_F(ManageTest, CheckPrivateBorrowUsed)
{
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo.privateUsedInfo[0][1].ifUsedFreshed = true;
    MOCKER(EnvMsleep).stubs();
    bool ret = CheckPrivateBorrowUsed(2);
    EXPECT_EQ(true, ret);
}

TEST_F(ManageTest, TestCheckReadyMigrateBack)
{
    bool ret;
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes->numaAttr.numaNodes = 1;
    g_processManager.remoteNumaInfo.sharedSize[1] = 1;
    EnvMutexInit(&g_processManager.lock);
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(true));
    MOCKER(CheckPrivateBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(true, ret);

    GlobalMockObject::verify();
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(false));
    MOCKER(CheckPrivateBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestCheckReadyMigrateBackTwo)
{
    bool ret;
    g_processManager.processes = nullptr;
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(true, ret);
}

extern "C" int IsPidArrInState(pid_t *pidArr, int len, enum ProcessState state);
extern "C" int IsPidArrayStateChangeReady(pid_t *pidArr, int len, int enable);
TEST_F(ManageTest, TestIsPidArrInStateInvalid)
{
    ProcessAttr pid = {.state = PROC_IDLE};
    pid_t pidArr[] = {1, 2};

    int ret = IsPidArrayStateChangeReady(nullptr, 2, 1);
    EXPECT_EQ(-EINVAL, ret);
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue((ProcessAttr *)nullptr)).then(returnValue(&pid));
    ret = IsPidArrayStateChangeReady(pidArr, 2, 1);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsPidArrInStateNormal)
{
    ProcessAttr pid1 = {.state = PROC_MIGRATE};
    ProcessAttr pid2 = {.state = PROC_MOVE};
    pid_t pidArr[] = {1, 2};

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    int ret = IsPidArrayStateChangeReady(pidArr, 2, 0);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    pid2.state = PROC_IDLE;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    ret = IsPidArrayStateChangeReady(pidArr, 2, 1);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsPidArrInState)
{
    ProcessAttr pid1 = {.state = PROC_MOVE};
    ProcessAttr pid2 = {.state = PROC_MOVE};
    pid_t pidArr[] = {1, 2};

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    int ret = IsPidArrInState(pidArr, 2, PROC_MOVE);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    pid2.state = PROC_IDLE;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    ret = IsPidArrInState(pidArr, 2, PROC_MOVE);
    EXPECT_EQ(0, ret);
}

extern "C" void SetPidArrState(pid_t *pidArr, int len, enum ProcessState state, int enable);
TEST_F(ManageTest, TestSetPidArrState)
{
    ProcessAttr pid1 = {.state = PROC_IDLE};
    ProcessAttr pid2 = {.state = PROC_IDLE};
    pid_t pidArr[] = {1, 2};

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    SetPidArrState(pidArr, 2, PROC_MOVE, 0);
    EXPECT_EQ(PROC_MOVE, pid1.state);
    EXPECT_EQ(PROC_MOVE, pid2.state);
}

TEST_F(ManageTest, TestChangePidRemoteByNuma)
{
    int srcNid = 4;
    int destNid = 6;
    int anotherNid = 5;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.numaAttr.numaNodes = 0b01010000;
    pid2.numaAttr.numaNodes = 0b00110000;
    pid1.next = &pid2;
    g_processManager.processes = &pid1;
    g_processManager.nrLocalNuma = 4;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetCurrentMaxNrPid).stubs().will(returnValue(2));
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(EINVAL));
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(0));
    ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByNumaTwo)
{
    g_processManager.processes = nullptr;
    int srcNid = 4;
    int destNid = 6;
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByNumaSyncAllProcessConfigFail)
{
    int srcNid = 4;
    int destNid = 6;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.numaAttr.numaNodes = 0b01010000;
    pid2.numaAttr.numaNodes = 0b00110000;
    pid1.next = &pid2;
    g_processManager.processes = &pid1;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetCurrentMaxNrPid).stubs().will(returnValue(2));
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).expects(once()).will(returnValue(-EBADF));
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByPid)
{
    int srcNid = 4;
    int destNid = 6;
    int pidLen = 1;
    struct MigPidRemoteNumaIoctlMsg msg = {
        .pidCnt = 1,
    };
    msg.migResArray = (int *)calloc(1, sizeof(int));
    msg.payloads = (struct MigPayload *)malloc(sizeof(struct MigPayload));
    ProcessAttr pid1 = {};
    pid1.pid = 100;

    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.processes = &pid1;
    msg.payloads[0].pid = pid1.pid;
    msg.payloads[0].srcNid = 4;
    msg.payloads[0].destNid = 6;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(0));
    int ret = ChangePidRemoteByPid(&msg);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(EBADF));
    ret = ChangePidRemoteByPid(&msg);
    EXPECT_EQ(EBADF, ret);
    free(msg.migResArray);
    free(msg.payloads);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableInvalid)
{
    pid_t pidArr[] = {1};

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(-EINVAL));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableRetryFail)
{
    pid_t pidArr[] = {1};

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(-ETIMEDOUT, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableRetrySuccess)
{
    pid_t pidArr[] = {1};

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(0)).then(returnValue(1));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    MOCKER(SetPidArrState).expects(once());
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableNormal)
{
    pid_t pidArr[] = {1};

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(1));
    MOCKER(SetPidArrState).expects(once());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateEnableNormal)
{
    pid_t pidArr[] = {1};

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(1));
    MOCKER(SetPidArrState).expects(once());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, ENABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedInvalid)
{
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(0);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedFail)
{
    int destNid = 4;
    ProcessAttr pid1 = {};
    pid1.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedSuccess)
{
    int destNid = 4;
    int anotherNid = 5;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.state = PROC_MOVE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid2.next = &pid1;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    g_processManager.processes = &pid2;
    ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedInvalid)
{
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(0);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedFail)
{
    int destNid = 4;
    ProcessAttr pid1;
    pid1.state = PROC_MIGRATE;
    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccess)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessTwo)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 5;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(-22, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessThree)
{
    int destNid = 7;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 2;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessFour)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_BACK;
    pid2.state = PROC_BACK;
    pid1.numaAttr.numaNodes = 0b00010001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(0, ret);
}

extern "C" bool MigOutIsDone(ProcessAttr *attr, bool *isMultiNumaPid);
const int NR_PAGES_L1 = 5;
const int NR_PAGE = 10;
const pid_t PID = 123;
TEST_F(ManageTest, TestMigOutIsDoneSuccess)
{
    bool ret;
    pid_t pid = PID;
    bool isMultiNumaPid = false;
    ProcessAttr attr = {};
    attr.walkPage.nrPages[0] = NR_PAGES_L1;
    attr.walkPage.nrPages[4] = NR_PAGES_L1;
    attr.walkPage.nrPage = NR_PAGE;
    attr.pid = PID;
    attr.migrateMode = MIG_MEMSIZE_MODE;
    attr.migrateParam[0].nid = 4;
    attr.migrateParam[0].memSize = 10240;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = nullptr;
    ret = MigOutIsDone(&attr, &isMultiNumaPid);
    EXPECT_EQ(false, ret);

    attr.numaAttr.numaNodes = 0b00010001;
    attr.remoteNumaCnt = 1;
    g_processManager.processes = &attr;
    ret = MigOutIsDone(&attr, &isMultiNumaPid);
    EXPECT_EQ(true, ret);
}

TEST_F(ManageTest, TestMigOutIsDoneSingleRemoteUsesRemotePages)
{
    bool isMultiNumaPid = false;
    ProcessAttr attr = {};

    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.nrLocalNuma = 4;
    attr.pid = PID;
    attr.migrateMode = MIG_MEMSIZE_MODE;
    attr.numaAttr.numaNodes = 0b00010001;
    attr.remoteNumaCnt = 1;
    attr.migrateParam[0].nid = 4;
    attr.migrateParam[0].memSize = 1024 * 1000;
    attr.walkPage.nrPages[0] = 200;
    attr.walkPage.nrPages[4] = 0;
    attr.walkPage.nrPage = 1000;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 500;

    EXPECT_TRUE(MigOutIsDone(&attr, &isMultiNumaPid));
    EXPECT_FALSE(isMultiNumaPid);

    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 499;
    attr.walkPage.nrPages[4] = 499;
    EXPECT_FALSE(MigOutIsDone(&attr, &isMultiNumaPid));
}

// Helper function to convert KB to pages for testing
static uint64_t KBToPages(uint64_t kb, uint32_t pageSize)
{
    return kb * KIB / pageSize;
}

// Global NUMA page snapshot for single-remote configuration tests.
static uint64_t g_testPagesPerNuma[MAX_NODES] = {0};

extern "C" int SetSingleRemoteNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma,
                                         const uint64_t pagesPerNuma[MAX_NODES]);
extern "C" void MigratePagesToRemote(ProcessAttr *attr, int l2Index, const uint64_t pagesPerNuma[MAX_NODES],
                                     uint64_t pagesToMigrate);
extern "C" void RecallPagesFromRemote(ProcessAttr *attr, int l2Index, uint64_t pagesToRecall);

/*
 * Test SetSingleRemoteNumaConfig: First migration (no existing pages on remote)
 * Scenario: Process with 3GB memory, first migration set to 2GB
 * Expected: memSize[0][0] should be 2GB (pages based on local NUMA 0)
 */
TEST_F(ManageTest, TestSetSingleRemoteNumaConfig_FirstMigration)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000001; // L1: NUMA 0

    ProcessParam param = {};
    param.count = 1;
    param.numaParam[0].nid = 4;                 // Remote NUMA 4 (l2Index = 0)
    param.numaParam[0].memSize = 2 * GIB / KIB; // 2GB in KB
    param.numaParam[0].ratio = 50;

    memset(g_testPagesPerNuma, 0, sizeof(g_testPagesPerNuma));
    g_testPagesPerNuma[0] = KBToPages(3 * GIB / KIB, PAGESIZE_4K); // Local NUMA 0 has 3GB (in pages)
    g_testPagesPerNuma[4] = 0;                                     // Remote NUMA has 0 pages

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    SetSingleRemoteNumaConfig(&attr, &param, 4, g_testPagesPerNuma);

    // First migration: 2GB target, 0 existing -> should allocate 2GB
    uint64_t expectedPages = KBToPages(2 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedMemSize = expectedPages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedMemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(4, attr.migrateParam[0].nid);
    EXPECT_EQ(2 * GIB / KIB, attr.migrateParam[0].memSize);
}

/*
 * Test SetSingleRemoteNumaConfig: Migration size increased (positive migration)
 * Scenario: Remote already has 2GB, user sets new target to 3GB
 * Expected: memSize records 3GB target, remoteNrPagesAfterMigrate records 2GB existing pages
 */
TEST_F(ManageTest, TestSetSingleRemoteNumaConfig_IncreaseMigration)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000001; // L1: NUMA 0

    // Simulate existing migration: remote already has 2GB
    uint64_t existingPages = KBToPages(2 * GIB / KIB, PAGESIZE_4K);
    attr.strategyAttr.memSize[0][0] = existingPages * (PAGESIZE_4K / KIB);

    ProcessParam param = {};
    param.count = 1;
    param.numaParam[0].nid = 4;                 // Remote NUMA 4 (l2Index = 0)
    param.numaParam[0].memSize = 3 * GIB / KIB; // New target: 3GB in KB
    param.numaParam[0].ratio = 50;

    memset(g_testPagesPerNuma, 0, sizeof(g_testPagesPerNuma));
    g_testPagesPerNuma[0] = KBToPages(1 * GIB / KIB, PAGESIZE_4K); // Local NUMA 0 now has 1GB
    g_testPagesPerNuma[4] = existingPages;                         // Remote NUMA has 2GB (already migrated)

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    SetSingleRemoteNumaConfig(&attr, &param, 4, g_testPagesPerNuma);

    // Increase: 3GB target - 2GB existing = 1GB to add
    // Local NUMA 0 has 1GB, so can add 1GB
    uint64_t expectedPages = KBToPages(3 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedMemSize = expectedPages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedMemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ((uint32_t)existingPages, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(3 * GIB / KIB, attr.migrateParam[0].memSize);
}

/*
 * Test SetSingleRemoteNumaConfig: Migration size unchanged (no operation)
 * Scenario: Remote already has 2GB, user sets new target to 2GB
 * Expected: memSize should remain unchanged
 */
TEST_F(ManageTest, TestSetSingleRemoteNumaConfig_NoChange)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000001; // L1: NUMA 0

    // Simulate existing migration: remote already has 2GB
    uint64_t existingPages = KBToPages(2 * GIB / KIB, PAGESIZE_4K);
    attr.strategyAttr.memSize[0][0] = existingPages * (PAGESIZE_4K / KIB);

    ProcessParam param = {};
    param.count = 1;
    param.numaParam[0].nid = 4;                 // Remote NUMA 4 (l2Index = 0)
    param.numaParam[0].memSize = 2 * GIB / KIB; // New target: 2GB in KB (same as existing)
    param.numaParam[0].ratio = 50;

    memset(g_testPagesPerNuma, 0, sizeof(g_testPagesPerNuma));
    g_testPagesPerNuma[0] = KBToPages(1 * GIB / KIB, PAGESIZE_4K); // Local NUMA 0 has 1GB
    g_testPagesPerNuma[4] = existingPages;                         // Remote NUMA has 2GB

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    SetSingleRemoteNumaConfig(&attr, &param, 4, g_testPagesPerNuma);

    // No change: memSize should remain the same
    uint64_t expectedMemSize = existingPages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedMemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ((uint32_t)existingPages, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(2 * GIB / KIB, attr.migrateParam[0].memSize);
}

/*
 * Test SetSingleRemoteNumaConfig: Migration size decreased (negative migration / recall)
 * Scenario: Remote already has 2GB, user sets new target to 1GB
 * Expected: memSize should decrease by 1GB (using -=)
 */
TEST_F(ManageTest, TestSetSingleRemoteNumaConfig_DecreaseMigration)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000001; // L1: NUMA 0

    // Simulate existing migration: remote already has 2GB
    uint64_t existingPages = KBToPages(2 * GIB / KIB, PAGESIZE_4K);
    attr.strategyAttr.memSize[0][0] = existingPages * (PAGESIZE_4K / KIB);

    ProcessParam param = {};
    param.count = 1;
    param.numaParam[0].nid = 4;                 // Remote NUMA 4 (l2Index = 0)
    param.numaParam[0].memSize = 1 * GIB / KIB; // New target: 1GB in KB (less than existing 2GB)
    param.numaParam[0].ratio = 25;

    memset(g_testPagesPerNuma, 0, sizeof(g_testPagesPerNuma));
    g_testPagesPerNuma[0] = KBToPages(1 * GIB / KIB, PAGESIZE_4K); // Local NUMA 0 has 1GB
    g_testPagesPerNuma[4] = existingPages;                         // Remote NUMA has 2GB

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    SetSingleRemoteNumaConfig(&attr, &param, 4, g_testPagesPerNuma);

    // Decrease: 2GB existing - 1GB target = 1GB to recall
    // memSize should decrease from 2GB to 1GB
    uint64_t expectedPages = KBToPages(1 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedMemSize = expectedPages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedMemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ((uint32_t)existingPages, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ(1 * GIB / KIB, attr.migrateParam[0].memSize);
}

/*
 * Test RecallPagesFromRemote: Recall from multi-local NUMA in reverse order
 * Scenario: NUMA0 has 2GB, NUMA1 has 1GB on remote, recall 2GB
 * Expected: Recall from NUMA1 first (1GB), then from NUMA0 (1GB), remaining: NUMA0 has 1GB
 */
TEST_F(ManageTest, TestRecallPagesFromRemote_MultiLocalNuma)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000011; // L1: NUMA 0 and NUMA 1

    // Setup existing allocation: NUMA0->2GB, NUMA1->1GB on remote
    uint64_t pagesNuma0 = KBToPages(2 * GIB / KIB, PAGESIZE_4K);
    uint64_t pagesNuma1 = KBToPages(1 * GIB / KIB, PAGESIZE_4K);
    attr.strategyAttr.memSize[0][0] = pagesNuma0 * (PAGESIZE_4K / KIB); // NUMA0: 2GB
    attr.strategyAttr.memSize[1][0] = pagesNuma1 * (PAGESIZE_4K / KIB); // NUMA1: 1GB

    uint64_t pagesPerNuma[MAX_NODES] = {0};
    pagesPerNuma[0] = KBToPages(1 * GIB / KIB, PAGESIZE_4K);
    pagesPerNuma[1] = KBToPages(1 * GIB / KIB, PAGESIZE_4K);

    // Recall 2GB: should recall from NUMA1 first (reverse order), then NUMA0
    uint64_t pagesToRecall = KBToPages(2 * GIB / KIB, PAGESIZE_4K);

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    RecallPagesFromRemote(&attr, 0, pagesToRecall);

    // After recall: NUMA1 should have 0, NUMA0 should have 1GB (2GB - 1GB)
    uint64_t expectedNuma0Pages = KBToPages(1 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedNuma0MemSize = expectedNuma0Pages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedNuma0MemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ(0, attr.strategyAttr.memSize[1][0]);
}

/*
 * Test MigratePagesToRemote: Forward allocation order
 * Scenario: NUMA0 has 3GB, NUMA1 has 2GB, migrate 4GB
 * Expected: Allocate from NUMA0 first (3GB), then NUMA1 (1GB)
 */
TEST_F(ManageTest, TestMigratePagesToRemote_MultiLocalNuma)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.numaAttr.numaNodes = 0b00000011; // L1: NUMA 0 and NUMA 1

    uint64_t pagesPerNuma[MAX_NODES] = {0};
    pagesPerNuma[0] = KBToPages(3 * GIB / KIB, PAGESIZE_4K); // NUMA0: 3GB
    pagesPerNuma[1] = KBToPages(2 * GIB / KIB, PAGESIZE_4K); // NUMA1: 2GB

    uint64_t pagesToMigrate = KBToPages(4 * GIB / KIB, PAGESIZE_4K); // Migrate 4GB

    MOCKER(IsHugeMode).stubs().will(returnValue(false));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InAttrL1).stubs().will(returnValue(true));

    MigratePagesToRemote(&attr, 0, pagesPerNuma, pagesToMigrate);

    // After migration: NUMA0 should have 3GB, NUMA1 should have 1GB
    uint64_t expectedNuma0Pages = KBToPages(3 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedNuma1Pages = KBToPages(1 * GIB / KIB, PAGESIZE_4K);
    uint64_t expectedNuma0MemSize = expectedNuma0Pages * (PAGESIZE_4K / KIB);
    uint64_t expectedNuma1MemSize = expectedNuma1Pages * (PAGESIZE_4K / KIB);
    EXPECT_EQ(expectedNuma0MemSize, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ(expectedNuma1MemSize, attr.strategyAttr.memSize[1][0]);
}

extern "C" uint32_t g_pageSizeNormal;

TEST_F(ManageTest, TestHugePageToKB)
{
    g_pageSizeHuge = PAGESIZE_2M;
    uint64_t ret;

    ret = HugePageToKB(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = HugePageToKB(1);
    EXPECT_EQ((uint64_t)2048, ret); // 2MB = 2048KB

    ret = HugePageToKB(5);
    EXPECT_EQ((uint64_t)10240, ret); // 5 * 2048 = 10240KB
}

TEST_F(ManageTest, TestNormalPageToKB)
{
    g_pageSizeNormal = PAGESIZE_4K;
    uint64_t ret;

    ret = NormalPageToKB(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = NormalPageToKB(1);
    EXPECT_EQ((uint64_t)4, ret); // 4KB page → 4KB

    ret = NormalPageToKB(256);
    EXPECT_EQ((uint64_t)1024, ret); // 256 * 4 = 1024KB
}

TEST_F(ManageTest, TestKBToPageNormal)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    uint64_t ret = KBToPage(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = KBToPage(4);
    EXPECT_EQ((uint64_t)1, ret); // 4KB memory → 1 normal page

    ret = KBToPage(1024);
    EXPECT_EQ((uint64_t)256, ret); // 1024KB / 4KB = 256 pages
}

TEST_F(ManageTest, TestKBToPageHuge)
{
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;

    uint64_t ret = KBToPage(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = KBToPage(2048);
    EXPECT_EQ((uint64_t)1, ret); // 2048KB → 1 huge page

    ret = KBToPage(4096);
    EXPECT_EQ((uint64_t)2, ret); // 4096KB / 2048KB = 2 huge pages
}

TEST_F(ManageTest, TestPageToKBNormal)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_4K;

    uint64_t ret = PageToKB(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = PageToKB(1);
    EXPECT_EQ((uint64_t)4, ret); // 1 normal page = 4KB

    ret = PageToKB(256);
    EXPECT_EQ((uint64_t)1024, ret); // 256 * 4KB = 1024KB
}

TEST_F(ManageTest, TestPageToKBHuge)
{
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;

    uint64_t ret = PageToKB(0);
    EXPECT_EQ((uint64_t)0, ret);

    ret = PageToKB(1);
    EXPECT_EQ((uint64_t)2048, ret); // 1 huge page = 2048KB

    ret = PageToKB(5);
    EXPECT_EQ((uint64_t)10240, ret); // 5 * 2048KB = 10240KB
}

extern "C" void AllocBorrowPagesForMemsize(ProcessAttr *attr,
                                           uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                           uint32_t availSharedPages[REMOTE_NUMA_NUM]);

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizeInvalidL2IndexLow)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 1; // l2Index = 1 - 4 = -3 < 0, triggers early return

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    // Should return early without crash or modification
    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);
    EXPECT_EQ((uint64_t)0, attr.strategyAttr.memSize[0][0]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizeInvalidL2IndexHigh)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 30; // l2Index = 30 - 4 = 26 >= REMOTE_NUMA_NUM(18)

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);
    EXPECT_EQ((uint64_t)0, attr.strategyAttr.memSize[0][0]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizeNoLocalNuma)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 4;       // l2Index = 0
    attr.migrateParam[0].memSize = 400; // 400KB → 100 pages (4K)
    attr.numaAttr.numaNodes = 0;        // No local NUMA set

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    availPrivatePages[0][0] = 100;
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);

    // All local NUMAs skipped, nothing allocated
    EXPECT_EQ((uint64_t)0, attr.strategyAttr.memSize[0][0]);
    EXPECT_EQ((uint32_t)100, availPrivatePages[0][0]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizePrivateOnly)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 4;         // remote NUMA 4, l2Index = 0
    attr.migrateParam[0].memSize = 400;   // 400KB → 100 pages (4K)
    attr.numaAttr.numaNodes = 0b00000001; // L1: node 0
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 200;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    availPrivatePages[0][0] = 150; // More than memSize needs
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);

    // nrLeft=100, nrUsed=MIN(200,150)=150, nrLeft(100) < nrUsed(150)
    // So: availPrivatePages[0][0] -= 100 → 50, memSize[0][0] = PageToKB(100) = 400
    EXPECT_EQ((uint32_t)50, availPrivatePages[0][0]);
    EXPECT_EQ((uint64_t)400, attr.strategyAttr.memSize[0][0]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizePrivateAndShared)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 4;         // l2Index = 0
    attr.migrateParam[0].memSize = 1000;  // 1000KB → 250 pages (4K)
    attr.numaAttr.numaNodes = 0b00000001; // L1: node 0
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 300;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    availPrivatePages[0][0] = 100; // Private insufficient
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};
    availSharedPages[0] = 200;

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);

    // Step 1 - Private: nrPages=300, nrUsed=MIN(300,100)=100
    //   nrLeft(250)>=nrUsed(100): nrLeft=150, availPrivatePages[0][0]=0, memSize=PageToKB(100)=400
    // Step 2 - Shared: nrPages=200, nrUsed=MIN(200,200)=200
    //   nrLeft(150)<nrUsed(200): availSharedPages[0]-=150→50, memSize+=PageToKB(150)=1000
    EXPECT_EQ((uint32_t)0, availPrivatePages[0][0]);
    EXPECT_EQ((uint32_t)50, availSharedPages[0]);
    EXPECT_EQ((uint64_t)1000, attr.strategyAttr.memSize[0][0]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizeSkipNonLocal)
{
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 5;         // l2Index = 1
    attr.migrateParam[0].memSize = 400;   // 100 pages
    attr.numaAttr.numaNodes = 0b00000010; // L1: node 1 only (not node 0)
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 200;
    attr.strategyAttr.nrPagesPerLocalNuma[1] = 200;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    availPrivatePages[1][1] = 200;
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);

    // i=0: InAttrL1(attr,0)=false → memSize[0][1]=0
    // i=1: InAttrL1(attr,1)=true → nrLeft=100, nrUsed=MIN(200,200)=200
    //   nrLeft(100)<nrUsed(200): availPrivatePages[1][1]-=100→100, memSize[1][1]=PageToKB(100)=400
    EXPECT_EQ((uint64_t)0, attr.strategyAttr.memSize[0][1]);
    EXPECT_EQ((uint64_t)400, attr.strategyAttr.memSize[1][1]);
    EXPECT_EQ((uint32_t)100, availPrivatePages[1][1]);
}

TEST_F(ManageTest, TestAllocBorrowPagesForMemsizeHugePage)
{
    g_pageSizeHuge = PAGESIZE_2M;
    g_pageSizeNormal = PAGESIZE_4K;
    g_processManager.tracking.pageSize = PAGESIZE_2M; // Huge mode
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.migrateParam[0].nid = 4;         // l2Index = 0
    attr.migrateParam[0].memSize = 2048;  // 2048KB = 1 huge page worth
    attr.numaAttr.numaNodes = 0b00000001; // L1: node 0
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};
    availPrivatePages[0][0] = 5;
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = {0};

    AllocBorrowPagesForMemsize(&attr, availPrivatePages, availSharedPages);

    // KBToPage(2048) in huge mode = 2048 / 2048 = 1 page (floor division)
    // nrLeft=1, nrPages=10, nrUsed=MIN(10,5)=5
    // nrLeft(1)<nrUsed(5): availPrivatePages[0][0]-=1→4, memSize[0][0]=PageToKB(1)=2048
    EXPECT_EQ((uint32_t)4, availPrivatePages[0][0]);
    EXPECT_EQ((uint64_t)2048, attr.strategyAttr.memSize[0][0]);
}

extern "C" void CalibratePairAccount(ProcessAttr *attr);

TEST_F(ManageTest, TestCalibratePairAccountNoChange)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 50;
    attr.walkPage.nrPages[4] = 150; // remotePages(150) == wp->nrPages[4](150), skip

    CalibratePairAccount(&attr);

    // No change: values stay as-is
    EXPECT_EQ((uint32_t)100, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)50, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountSingleLocal)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr.walkPage.nrPages[4] = 50; // actual pages dropped from 100 to 50

    CalibratePairAccount(&attr);

    // arrLen=1, last(only) gets nrLeft=50
    EXPECT_EQ((uint32_t)50, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountMultiLocal)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    // Two local NUMAs contribute to remote NUMA 4 (l2Index=0)
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 100;
    attr.walkPage.nrPages[4] = 150; // actual dropped from 200 to 150

    CalibratePairAccount(&attr);

    // remotePages=200, arr=[0,1], nrLeft=150
    // i=0: ratio=100/200=0.5, nrChunk=150*0.5=75, nrLeft=75
    // i=1 (last): gets nrLeft=75
    EXPECT_EQ((uint32_t)75, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)75, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountSkipZeroEntries)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    // Local NUMA 0 has 0 entries — excluded from arr[]
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 0;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 100;
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][0] = 100;
    attr.walkPage.nrPages[4] = 300; // actual increased from 200 to 300

    CalibratePairAccount(&attr);

    // remotePages=200, arr=[1,2], nrLeft=300
    // i=0 (arr[0]=1): ratio=100/200=0.5, nrChunk=300*0.5=150, nrLeft=150
    // i=1 (arr[1]=2, last): gets nrLeft=150
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]); // unchanged (was 0)
    EXPECT_EQ((uint32_t)150, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)150, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountMultiRemote)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    // l2Index=0: remotePages=100, actual=100 → skip
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr.walkPage.nrPages[4] = 100;
    // l2Index=1: remotePages=200, actual=100 → calibrate
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 200;
    attr.walkPage.nrPages[5] = 100; // l2Index=1, remote=5

    CalibratePairAccount(&attr);

    // l2Index=0: unchanged (remotePages==actual)
    EXPECT_EQ((uint32_t)100, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    // l2Index=1: arrLen=1, last gets nrLeft=100
    EXPECT_EQ((uint32_t)100, attr.strategyAttr.remoteNrPagesAfterMigrate[0][1]);
}

TEST_F(ManageTest, TestCalibratePairAccountToZero)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 100;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 50;
    attr.walkPage.nrPages[4] = 0; // all pages gone from remote NUMA 4

    CalibratePairAccount(&attr);

    // remotePages=150, arr=[0,1], nrLeft=0
    // i=0: ratio=100/150≈0.667, nrChunk=0*0.667=0, nrLeft=0
    // i=1 (last): gets nrLeft=0
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountThreeLocals)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    // Three local NUMAs with different proportions
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 10; // 10%
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 30; // 30%
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][0] = 60; // 60%
    attr.walkPage.nrPages[4] = 200;                         // actual doubled from 100 to 200

    CalibratePairAccount(&attr);

    // remotePages=100, arr=[0,1,2], nrLeft=200
    // i=0 (arr[0]=0): ratio=10/100=0.1, nrChunk=200*0.1=20, nrLeft=180
    // i=1 (arr[1]=1): ratio=30/100=0.3, nrChunk=200*0.3=60, nrLeft=120
    // i=2 (arr[2]=2, last): gets nrLeft=120
    EXPECT_EQ((uint32_t)20, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)60, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)120, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountStableRemainderOrder)
{
    g_processManager.nrLocalNuma = 4;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 1;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 1;
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][0] = 1;
    attr.walkPage.nrPages[4] = 2;

    CalibratePairAccount(&attr);

    EXPECT_EQ((uint32_t)1, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)1, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
    EXPECT_EQ((uint32_t)2, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] +
                               attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] +
                               attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountRebuildsByManagedResidentPages)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.privateSize[0][0] = 0;
    g_processManager.remoteNumaInfo.privateSize[1][0] = 0;
    g_processManager.remoteNumaInfo.privateSize[2][0] = 0;
    g_processManager.remoteNumaInfo.sharedSize[0] = 0;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1) | BIT(2);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.walkPage.nrPages[0] = 1;
    attr.walkPage.nrPages[1] = 2;
    attr.walkPage.nrPages[4] = 5;

    CalibratePairAccount(&attr);

    EXPECT_EQ((uint32_t)2, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)3, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
    EXPECT_EQ(BIT(0) | BIT(1), attr.managedLocalState.accountLocalMask[0]);
}

TEST_F(ManageTest, TestCalibratePairAccountEvenlyRebuildsWithoutLocalPages)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.privateSize[0][0] = 0;
    g_processManager.remoteNumaInfo.privateSize[2][0] = 0;
    g_processManager.remoteNumaInfo.sharedSize[0] = 0;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(2);
    attr.walkPage.nrPages[4] = 5;

    CalibratePairAccount(&attr);

    EXPECT_EQ((uint32_t)3, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)2, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
    EXPECT_EQ((uint32_t)5, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] +
                               attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] +
                               attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
}

TEST_F(ManageTest, TestCalibratePairAccountKeepsActualPagesAfterCapacityRemoved)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.privateSize[3][0] = 0;
    g_processManager.remoteNumaInfo.sharedSize[0] = 0;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.managedLocalState.managedLocalMask = BIT(3);
    attr.walkPage.nrPages[4] = 7;

    CalibratePairAccount(&attr);

    EXPECT_EQ((uint32_t)7, attr.strategyAttr.remoteNrPagesAfterMigrate[3][0]);
    EXPECT_EQ(BIT(3), attr.managedLocalState.accountLocalMask[0]);
}

TEST_F(ManageTest, TestCalibratePairAccountUsesEligibleCapacityPair)
{
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.privateSize[0][0] = 0;
    g_processManager.remoteNumaInfo.privateSize[1][0] = 128;
    g_processManager.remoteNumaInfo.privateSize[2][0] = 0;
    g_processManager.remoteNumaInfo.sharedSize[0] = 0;

    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1) | BIT(2);
    attr.walkPage.nrPages[4] = 6;

    CalibratePairAccount(&attr);

    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[0][0]);
    EXPECT_EQ((uint32_t)6, attr.strategyAttr.remoteNrPagesAfterMigrate[1][0]);
    EXPECT_EQ((uint32_t)0, attr.strategyAttr.remoteNrPagesAfterMigrate[2][0]);
    EXPECT_EQ(BIT(1), attr.managedLocalState.accountLocalMask[0]);
    g_processManager.remoteNumaInfo.privateSize[1][0] = 0;
}

struct PairRequestResult {
    int ret;
    PairTarget targets[LOCAL_NUMA_NUM * REMOTE_NUMA_NUM];
    PairRequestSummary summary;
    size_t targetCnt;

    uint32_t Find(int localNid, int remoteNid) const
    {
        for (size_t i = 0; i < targetCnt; i++) {
            if (targets[i].localNid == localNid && targets[i].remoteNid == remoteNid) {
                return targets[i].requestedPages;
            }
        }
        return 0;
    }

    uint64_t RemoteTotal(int remoteNid) const
    {
        uint64_t total = 0;
        for (size_t i = 0; i < targetCnt; i++) {
            if (targets[i].remoteNid == remoteNid) {
                total += targets[i].requestedPages;
            }
        }
        return total;
    }

    uint64_t LocalTotal(int localNid) const
    {
        uint64_t total = 0;
        for (size_t i = 0; i < targetCnt; i++) {
            if (targets[i].localNid == localNid) {
                total += targets[i].requestedPages;
            }
        }
        return total;
    }
};

static PairRequestResult RunBuildPairRequestedTargets(const ProcessAttr &attr, const PairRequestContext &context)
{
    PairRequestResult result = {};
    result.ret = BuildPairRequestedTargets(&attr, &context, result.targets, LOCAL_NUMA_NUM * REMOTE_NUMA_NUM,
                                           &result.targetCnt, &result.summary);
    return result;
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsRatioOneByOne)
{
    ProcessAttr attr = {};
    attr.pid = 100;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {1, 50, 0};
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.residentLocalMask = BIT(0);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[0] = 80;
    attr.walkPage.nrPages[1] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 1;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(100U, result.summary.managedTotalPages);
    EXPECT_EQ(50U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(50U, result.summary.effectiveRemotePages[0]);
    ASSERT_EQ(1U, result.targetCnt);
    EXPECT_EQ(100, result.targets[0].pid);
    EXPECT_EQ(0, result.targets[0].localNid);
    EXPECT_EQ(1, result.targets[0].remoteNid);
    EXPECT_EQ(50U, result.targets[0].requestedPages);
    EXPECT_EQ(0U, result.targets[0].targetPages);
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsMemsizeOneByOne)
{
    ProcessAttr attr = {};
    attr.pid = 107;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {1, 0, 160};
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.residentLocalMask = BIT(0);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[0] = 80;
    attr.walkPage.nrPages[1] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 1;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(40U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(40U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(40U, result.Find(0, 1));
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsMemsizeTwoByOne)
{
    ProcessAttr attr = {};
    attr.pid = 101;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {2, 0, 320};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0) | BIT(1);
    attr.walkPage.nrPages[0] = 40;
    attr.walkPage.nrPages[1] = 60;
    attr.walkPage.nrPages[2] = 30;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 10;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(130U, result.summary.managedTotalPages);
    EXPECT_EQ(80U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(80U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(30U, result.Find(0, 2));
    EXPECT_EQ(50U, result.Find(1, 2));
    EXPECT_EQ(80U, result.RemoteTotal(2));
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsRatioTwoByOne)
{
    ProcessAttr attr = {};
    attr.pid = 108;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {2, 50, 0};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0) | BIT(1);
    attr.walkPage.nrPages[0] = 40;
    attr.walkPage.nrPages[1] = 60;
    attr.walkPage.nrPages[2] = 30;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 10;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(65U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(65U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(24U, result.Find(0, 2));
    EXPECT_EQ(41U, result.Find(1, 2));
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsRatioTwoByTwo)
{
    ProcessAttr attr = {};
    attr.pid = 102;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 2;
    attr.targetConfig.targets[0] = {2, 50, 0};
    attr.targetConfig.targets[1] = {3, 25, 0};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.managedLocalState.accountLocalMask[1] = BIT(1);
    attr.walkPage.nrPages[0] = 60;
    attr.walkPage.nrPages[1] = 40;
    attr.walkPage.nrPages[2] = 20;
    attr.walkPage.nrPages[3] = 10;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][1] = 10;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    context.capacityLocalMask[1] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(130U, result.summary.managedTotalPages);
    EXPECT_EQ(65U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(32U, result.summary.requestedRemotePages[1]);
    EXPECT_EQ(65U, result.RemoteTotal(2));
    EXPECT_EQ(32U, result.RemoteTotal(3));
    EXPECT_EQ(47U, result.Find(0, 2));
    EXPECT_EQ(18U, result.Find(1, 2));
    EXPECT_EQ(14U, result.Find(0, 3));
    EXPECT_EQ(18U, result.Find(1, 3));
    EXPECT_LE(result.LocalTotal(0), 80U);
    EXPECT_LE(result.LocalTotal(1), 50U);
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsReroutesAsymmetricCandidates)
{
    ProcessAttr attr = {};
    attr.pid = 111;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 2;
    attr.targetConfig.targets[0] = {2, 50, 0};
    attr.targetConfig.targets[1] = {3, 50, 0};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.walkPage.nrPages[0] = 100;
    attr.walkPage.nrPages[1] = 100;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    context.capacityLocalMask[1] = BIT(0);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(100U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(100U, result.summary.effectiveRemotePages[1]);
    EXPECT_EQ(100U, result.RemoteTotal(2));
    EXPECT_EQ(100U, result.RemoteTotal(3));
    EXPECT_EQ(0U, result.Find(0, 2));
    EXPECT_EQ(100U, result.Find(1, 2));
    EXPECT_EQ(100U, result.Find(0, 3));
    EXPECT_EQ(0U, result.Find(1, 3));
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsMemsizeTwoByTwoScales)
{
    ProcessAttr attr = {};
    attr.pid = 103;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 2;
    attr.targetConfig.targets[0] = {2, 0, 320};
    attr.targetConfig.targets[1] = {3, 0, 320};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[0] = 40;
    attr.walkPage.nrPages[1] = 40;
    attr.walkPage.nrPages[2] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    context.capacityLocalMask[1] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(100U, result.summary.managedTotalPages);
    EXPECT_EQ(80U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(80U, result.summary.requestedRemotePages[1]);
    EXPECT_EQ(55U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(45U, result.summary.effectiveRemotePages[1]);
    EXPECT_EQ(55U, result.RemoteTotal(2));
    EXPECT_EQ(45U, result.RemoteTotal(3));
    EXPECT_LE(result.LocalTotal(0), 60U);
    EXPECT_LE(result.LocalTotal(1), 40U);
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsShrinksExistingPairs)
{
    ProcessAttr attr = {};
    attr.pid = 104;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {2, 0, 200};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0) | BIT(1);
    attr.walkPage.nrPages[2] = 100;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 60;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 40;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(30U, result.Find(0, 2));
    EXPECT_EQ(20U, result.Find(1, 2));
    EXPECT_EQ(50U, result.RemoteTotal(2));
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsDeletedRemoteEmitsZeroPair)
{
    ProcessAttr attr = {};
    attr.pid = 109;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 0;
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[1] = 25;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 25;

    PairRequestContext context = {};
    context.nrLocalNuma = 1;
    context.pageSizeKB = 4;
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    ASSERT_EQ(1U, result.targetCnt);
    EXPECT_EQ(0U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(0U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(0U, result.targets[0].requestedPages);
    EXPECT_EQ(0, result.targets[0].localNid);
    EXPECT_EQ(1, result.targets[0].remoteNid);
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsNoCapacityKeepsRequest)
{
    ProcessAttr attr = {};
    attr.pid = 110;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {1, 50, 0};
    attr.managedLocalState.managedLocalMask = BIT(0);
    attr.managedLocalState.residentLocalMask = BIT(0);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[0] = 80;
    attr.walkPage.nrPages[1] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;

    PairRequestContext context = {};
    context.nrLocalNuma = 1;
    context.pageSizeKB = 4;
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(50U, result.summary.requestedRemotePages[0]);
    EXPECT_EQ(50U, result.summary.effectiveRemotePages[0]);
    EXPECT_EQ(0U, result.RemoteTotal(1));
    EXPECT_EQ(50U, result.summary.unassignedRequestedPages[0]);
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsNewLocalKeepsAccount)
{
    ProcessAttr attr = {};
    attr.pid = 105;
    attr.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {2, 0, 240};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    attr.walkPage.nrPages[0] = 20;
    attr.walkPage.nrPages[1] = 40;
    attr.walkPage.nrPages[2] = 40;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 40;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    PairRequestResult first = RunBuildPairRequestedTargets(attr, context);
    PairRequestResult second = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, first.ret);
    ASSERT_EQ(0, second.ret);
    EXPECT_EQ(60U, first.RemoteTotal(2));
    EXPECT_GE(first.Find(0, 2), 40U);
    EXPECT_GT(first.Find(1, 2), 0U);
    ASSERT_EQ(first.targetCnt, second.targetCnt);
    EXPECT_EQ(first.summary.managedTotalPages, second.summary.managedTotalPages);
    for (size_t i = 0; i < first.targetCnt; i++) {
        EXPECT_EQ(first.targets[i].pid, second.targets[i].pid);
        EXPECT_EQ(first.targets[i].localNid, second.targets[i].localNid);
        EXPECT_EQ(first.targets[i].remoteNid, second.targets[i].remoteNid);
        EXPECT_EQ(first.targets[i].requestedPages, second.targets[i].requestedPages);
        EXPECT_EQ(first.targets[i].targetPages, second.targets[i].targetPages);
    }
}

TEST_F(ManageTest, TestBuildPairRequestedTargetsSkipsEmptyLocal)
{
    ProcessAttr attr = {};
    attr.pid = 106;
    attr.targetConfig.migrateMode = MIG_RATIO_MODE;
    attr.targetConfig.count = 1;
    attr.targetConfig.targets[0] = {2, 50, 0};
    attr.managedLocalState.managedLocalMask = BIT(0) | BIT(1);
    attr.managedLocalState.residentLocalMask = BIT(0);
    attr.walkPage.nrPages[0] = 40;

    PairRequestContext context = {};
    context.nrLocalNuma = 2;
    context.pageSizeKB = 4;
    context.capacityLocalMask[0] = BIT(0) | BIT(1);
    PairRequestResult result = RunBuildPairRequestedTargets(attr, context);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(20U, result.Find(0, 2));
    EXPECT_EQ(0U, result.Find(1, 2));
}

struct AllPairTargetResult {
    int ret;
    PairTarget targets[32];
    size_t targetCnt;

    uint32_t Requested(pid_t pid, int localNid, int remoteNid) const
    {
        for (size_t i = 0; i < targetCnt; i++) {
            if (targets[i].pid == pid && targets[i].localNid == localNid && targets[i].remoteNid == remoteNid) {
                return targets[i].requestedPages;
            }
        }
        return 0;
    }

    uint32_t Target(pid_t pid, int localNid, int remoteNid) const
    {
        for (size_t i = 0; i < targetCnt; i++) {
            if (targets[i].pid == pid && targets[i].localNid == localNid && targets[i].remoteNid == remoteNid) {
                return targets[i].targetPages;
            }
        }
        return 0;
    }
};

static void InitPairTargetManager(ProcessManager *manager, int nrLocalNuma)
{
    *manager = {};
    manager->nrLocalNuma = nrLocalNuma;
    manager->tracking.pageSize = PAGESIZE_4K;
    EnvMutexInit(&manager->lock);
    EnvMutexInit(&manager->remoteNumaInfo.lock);
}

static void DestroyPairTargetManager(ProcessManager *manager)
{
    EnvMutexDestroy(&manager->remoteNumaInfo.lock);
    EnvMutexDestroy(&manager->lock);
}

static void InitRatioPairProcess(ProcessAttr *attr, pid_t pid, int nrLocalNuma, uint32_t local0Pages,
                                 uint32_t local1Pages, uint32_t ratio)
{
    *attr = {};
    attr->pid = pid;
    attr->scanType = NORMAL_SCAN;
    attr->targetConfig.migrateMode = MIG_RATIO_MODE;
    attr->targetConfig.count = 1;
    attr->targetConfig.targets[0] = {nrLocalNuma, ratio, 0};
    attr->walkPage.nrPages[0] = local0Pages;
    attr->walkPage.nrPages[1] = local1Pages;
    if (local0Pages > 0) {
        attr->managedLocalState.managedLocalMask |= BIT(0);
        attr->managedLocalState.residentLocalMask |= BIT(0);
    }
    if (nrLocalNuma > 1 && local1Pages > 0) {
        attr->managedLocalState.managedLocalMask |= BIT(1);
        attr->managedLocalState.residentLocalMask |= BIT(1);
    }
}

static AllPairTargetResult RunBuildAllPairTargets(ProcessManager *manager)
{
    AllPairTargetResult result = {};
    result.ret = BuildAllPairTargets(manager, result.targets, sizeof(result.targets) / sizeof(result.targets[0]),
                                     &result.targetCnt);
    return result;
}

TEST_F(ManageTest, TestBuildAllPairTargetsSyncBypassesRemoteCapacity)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);

    ProcessAttr syncPid;
    ProcessAttr normalPid;
    InitRatioPairProcess(&syncPid, 100, 1, 800, 0, 50);
    InitRatioPairProcess(&normalPid, 200, 1, 800, 0, 50);
    syncPid.ignoreRemoteCapacity = true;
    syncPid.next = &normalPid;
    manager.processes = &syncPid;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);
    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(400U, result.Requested(100, 0, 1));
    EXPECT_EQ(400U, result.Target(100, 0, 1));
    EXPECT_EQ(0U, result.Requested(200, 0, 1));
    EXPECT_EQ(0U, result.Target(200, 0, 1));

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsPrivateFairAndStable)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.privateSize[0][0] = 1;

    ProcessAttr lowPid;
    ProcessAttr middlePid;
    ProcessAttr highPid;
    InitRatioPairProcess(&lowPid, 100, 1, 800, 0, 50);
    InitRatioPairProcess(&middlePid, 200, 1, 800, 0, 50);
    InitRatioPairProcess(&highPid, 300, 1, 800, 0, 50);
    highPid.next = &lowPid;
    lowPid.next = &middlePid;
    manager.processes = &highPid;

    AllPairTargetResult first = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, first.ret);
    ASSERT_EQ(3U, first.targetCnt);
    EXPECT_EQ(100, first.targets[0].pid);
    EXPECT_EQ(200, first.targets[1].pid);
    EXPECT_EQ(300, first.targets[2].pid);
    EXPECT_EQ(400U, first.Requested(100, 0, 1));
    EXPECT_EQ(86U, first.Target(100, 0, 1));
    EXPECT_EQ(85U, first.Target(200, 0, 1));
    EXPECT_EQ(85U, first.Target(300, 0, 1));

    middlePid.next = &highPid;
    highPid.next = &lowPid;
    lowPid.next = nullptr;
    manager.processes = &middlePid;
    AllPairTargetResult second = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(first.Target(100, 0, 1), second.Target(100, 0, 1));
    EXPECT_EQ(first.Target(200, 0, 1), second.Target(200, 0, 1));
    EXPECT_EQ(first.Target(300, 0, 1), second.Target(300, 0, 1));
    EXPECT_EQ(0U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(0U, manager.remoteNumaInfo.privateUsedInfo[0][0].used);
    EXPECT_EQ(256U, manager.remoteNumaInfo.usedInfo[0].size);
    EXPECT_TRUE(manager.remoteNumaInfo.usedInfo[0].ifUsedFreshed);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsSharedAcrossLocalsAndPids)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 2);
    manager.remoteNumaInfo.sharedSize[0] = 2;

    ProcessAttr firstPid;
    ProcessAttr secondPid;
    InitRatioPairProcess(&firstPid, 100, 2, 400, 400, 50);
    InitRatioPairProcess(&secondPid, 200, 2, 400, 400, 50);
    secondPid.next = &firstPid;
    manager.processes = &secondPid;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, result.ret);
    ASSERT_EQ(4U, result.targetCnt);
    EXPECT_EQ(200U, result.Requested(100, 0, 2));
    EXPECT_EQ(200U, result.Requested(100, 1, 2));
    EXPECT_EQ(128U, result.Target(100, 0, 2));
    EXPECT_EQ(128U, result.Target(100, 1, 2));
    EXPECT_EQ(128U, result.Target(200, 0, 2));
    EXPECT_EQ(128U, result.Target(200, 1, 2));
    EXPECT_EQ(0U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(0U, manager.remoteNumaInfo.privateUsedInfo[0][0].used);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsNoModePriority)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.sharedSize[0] = 2;

    ProcessAttr ratioPid;
    ProcessAttr memsizePid;
    InitRatioPairProcess(&ratioPid, 100, 1, 800, 0, 50);
    InitRatioPairProcess(&memsizePid, 200, 1, 800, 0, 0);
    memsizePid.targetConfig.migrateMode = MIG_MEMSIZE_MODE;
    memsizePid.targetConfig.targets[0].ratio = 0;
    memsizePid.targetConfig.targets[0].memSizeKB = 1600;
    memsizePid.next = &ratioPid;
    manager.processes = &memsizePid;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, result.ret);
    EXPECT_EQ(400U, result.Requested(100, 0, 1));
    EXPECT_EQ(400U, result.Requested(200, 0, 1));
    EXPECT_EQ(256U, result.Target(100, 0, 1));
    EXPECT_EQ(256U, result.Target(200, 0, 1));

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsPrivateThenShared)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.privateSize[0][0] = 1;
    manager.remoteNumaInfo.sharedSize[0] = 1;

    ProcessAttr firstPid;
    ProcessAttr secondPid;
    InitRatioPairProcess(&firstPid, 100, 1, 800, 0, 50);
    InitRatioPairProcess(&secondPid, 200, 1, 800, 0, 50);
    secondPid.next = &firstPid;
    manager.processes = &secondPid;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, result.ret);
    EXPECT_EQ(256U, result.Target(100, 0, 1));
    EXPECT_EQ(256U, result.Target(200, 0, 1));
    EXPECT_EQ(0U, manager.remoteNumaInfo.privateUsedInfo[0][0].used);
    EXPECT_EQ(0U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(512U, manager.remoteNumaInfo.usedInfo[0].size);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsKeepsResidentBaseline)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.privateSize[0][0] = 1;

    ProcessAttr residentPid;
    ProcessAttr newPid;
    InitRatioPairProcess(&residentPid, 100, 1, 500, 0, 50);
    residentPid.walkPage.nrPages[1] = 300;
    residentPid.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 300;
    residentPid.managedLocalState.accountLocalMask[0] = BIT(0);
    InitRatioPairProcess(&newPid, 200, 1, 800, 0, 50);
    newPid.next = &residentPid;
    manager.processes = &newPid;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, result.ret);
    EXPECT_EQ(400U, result.Requested(100, 0, 1));
    EXPECT_EQ(256U, result.Target(100, 0, 1));
    EXPECT_EQ(0U, result.Target(200, 0, 1));
    EXPECT_EQ(300U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(300U, manager.remoteNumaInfo.privateUsedInfo[0][0].used);
    EXPECT_EQ(256U, manager.remoteNumaInfo.usedInfo[0].size);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsCapacityRecovery)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);

    ProcessAttr attr;
    InitRatioPairProcess(&attr, 100, 1, 80, 0, 50);
    attr.walkPage.nrPages[1] = 20;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 20;
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    manager.processes = &attr;

    AllPairTargetResult noCapacity = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, noCapacity.ret);
    EXPECT_EQ(0U, noCapacity.Requested(100, 0, 1));
    EXPECT_EQ(0U, noCapacity.Target(100, 0, 1));
    EXPECT_EQ(20U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(0U, manager.remoteNumaInfo.usedInfo[0].size);

    manager.remoteNumaInfo.privateSize[0][0] = 1;
    AllPairTargetResult restored = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(0, restored.ret);
    EXPECT_EQ(50U, restored.Requested(100, 0, 1));
    EXPECT_EQ(50U, restored.Target(100, 0, 1));
    EXPECT_EQ(20U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(256U, manager.remoteNumaInfo.usedInfo[0].size);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsReroutesRequestFromIneligibleAccount)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 2);
    manager.remoteNumaInfo.privateSize[0][0] = 2;

    ProcessAttr attr;
    InitRatioPairProcess(&attr, 100, 2, 800, 0, 50);
    attr.managedLocalState.managedLocalMask |= BIT(1);
    attr.managedLocalState.accountLocalMask[0] = BIT(1);
    attr.walkPage.nrPages[2] = 200;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 200;
    manager.processes = &attr;

    AllPairTargetResult result = RunBuildAllPairTargets(&manager);

    ASSERT_EQ(0, result.ret);
    EXPECT_EQ(500U, result.Requested(100, 0, 2));
    EXPECT_EQ(500U, result.Target(100, 0, 2));
    EXPECT_EQ(0U, result.Requested(100, 1, 2));
    EXPECT_EQ(0U, result.Target(100, 1, 2));
    EXPECT_EQ(200U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(200U, manager.remoteNumaInfo.privateUsedInfo[1][0].used);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsCapacityShrinkAndRestore)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);

    ProcessAttr attr;
    InitRatioPairProcess(&attr, 100, 1, 800, 0, 50);
    manager.processes = &attr;

    manager.remoteNumaInfo.privateSize[0][0] = 2;
    AllPairTargetResult full = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(400U, full.Requested(100, 0, 1));
    EXPECT_EQ(400U, full.Target(100, 0, 1));

    manager.remoteNumaInfo.privateSize[0][0] = 1;
    AllPairTargetResult shrunk = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(400U, shrunk.Requested(100, 0, 1));
    EXPECT_EQ(256U, shrunk.Target(100, 0, 1));

    manager.remoteNumaInfo.privateSize[0][0] = 2;
    AllPairTargetResult restored = RunBuildAllPairTargets(&manager);
    EXPECT_EQ(400U, restored.Requested(100, 0, 1));
    EXPECT_EQ(400U, restored.Target(100, 0, 1));

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairPlanInputsKeepsTargetAndActualSnapshot)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.privateSize[0][0] = 1;

    ProcessAttr attr;
    InitRatioPairProcess(&attr, 100, 1, 800, 0, 50);
    attr.walkPage.nrPage = 1100;
    attr.walkPage.nrPages[1] = 300;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 300;
    attr.managedLocalState.accountLocalMask[0] = BIT(0);
    manager.processes = &attr;

    PairPlan plans[LOCAL_NUMA_NUM * REMOTE_NUMA_NUM] = {};
    PairPidBudget pidBudgets[1] = {};
    size_t planCnt = 0;
    size_t pidBudgetCnt = 0;
    ASSERT_EQ(0, BuildAllPairPlanInputs(&manager, plans, LOCAL_NUMA_NUM * REMOTE_NUMA_NUM, &planCnt, pidBudgets, 1,
                                        &pidBudgetCnt));
    ASSERT_EQ(1U, planCnt);
    EXPECT_EQ(100, plans[0].pid);
    EXPECT_EQ(0, plans[0].localNid);
    EXPECT_EQ(1, plans[0].remoteNid);
    EXPECT_EQ(0, plans[0].remoteIndex);
    EXPECT_EQ(256U, plans[0].targetPages);
    EXPECT_EQ(300U, plans[0].actualPages);
    ASSERT_EQ(1U, pidBudgetCnt);
    EXPECT_EQ(100, pidBudgets[0].pid);
    EXPECT_EQ(1100U, pidBudgets[0].maxMigratePages);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

TEST_F(ManageTest, TestBuildAllPairTargetsFailureDoesNotPublishUsage)
{
    ProcessManager manager;
    InitPairTargetManager(&manager, 1);
    manager.remoteNumaInfo.privateSize[0][0] = 1;
    manager.remoteNumaInfo.usedInfo[0].used = 7;
    manager.remoteNumaInfo.privateUsedInfo[0][0].used = 5;

    ProcessAttr attr;
    InitRatioPairProcess(&attr, 100, 1, 800, 0, 50);
    manager.processes = &attr;

    PairTarget target = {};
    size_t targetCnt = 1;
    int ret = BuildAllPairTargets(&manager, &target, 0, &targetCnt);
    EXPECT_EQ(-ENOSPC, ret);
    EXPECT_EQ(0U, targetCnt);
    EXPECT_EQ(7U, manager.remoteNumaInfo.usedInfo[0].used);
    EXPECT_EQ(5U, manager.remoteNumaInfo.privateUsedInfo[0][0].used);

    manager.processes = nullptr;
    DestroyPairTargetManager(&manager);
}

extern "C" void SetRunMode(RunMode runMode);
TEST_F(ManageTest, TestSetRunMode)
{
    SetRunMode(WATERLINE_MODE);
    EXPECT_EQ(WATERLINE_MODE, g_runMode);
    SetRunMode(MEM_POOL_MODE);
    EXPECT_EQ(MEM_POOL_MODE, g_runMode);
}

extern "C" PidType GetPidType(struct ProcessManager *manager);
TEST_F(ManageTest, TestGetPidType)
{
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_pageSizeNormal = PAGESIZE_4K;
    PidType ret = GetPidType(&g_processManager);
    EXPECT_EQ(PROCESS_TYPE, ret);

    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_pageSizeHuge = PAGESIZE_2M;
    ret = GetPidType(&g_processManager);
    EXPECT_EQ(VM_TYPE, ret);
}

extern "C" uint32_t GetNormalPageSize(void);
TEST_F(ManageTest, TestGetNormalPageSize)
{
    g_pageSizeNormal = PAGESIZE_4K;
    uint32_t ret = GetNormalPageSize();
    EXPECT_EQ(PAGESIZE_4K, ret);
}

extern "C" uint32_t GetHugePageSize(void);
TEST_F(ManageTest, TestGetHugePageSize)
{
    g_pageSizeHuge = PAGESIZE_2M;
    uint32_t ret = GetHugePageSize();
    EXPECT_EQ(PAGESIZE_2M, ret);
}

extern "C" uint32_t GetPageSize(void);
TEST_F(ManageTest, TestGetPageSize)
{
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    uint32_t ret = GetPageSize();
    EXPECT_EQ(PAGESIZE_4K, ret);
}

extern "C" int GetNrLocalNuma(void);
TEST_F(ManageTest, TestGetNrLocalNuma)
{
    g_processManager.nrLocalNuma = 4;
    int ret = GetNrLocalNuma();
    EXPECT_EQ(4, ret);
}

extern "C" bool IsHugeMode(void);
TEST_F(ManageTest, TestIsHugeModeTrue)
{
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_pageSizeHuge = PAGESIZE_2M;
    bool ret = IsHugeMode();
    EXPECT_EQ(true, ret);
}

TEST_F(ManageTest, TestIsHugeModeFalse)
{
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_pageSizeHuge = PAGESIZE_2M;
    bool ret = IsHugeMode();
    EXPECT_EQ(false, ret);
}

extern "C" void LinkedListAdd(ProcessAttr **head, ProcessAttr **add);
TEST_F(ManageTest, TestLinkedListAdd)
{
    ProcessAttr *head = nullptr;
    ProcessAttr *node1 = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    node1->pid = 1;
    node1->next = nullptr;
    LinkedListAdd(&head, &node1);
    EXPECT_EQ(head, node1);

    ProcessAttr *node2 = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    node2->pid = 2;
    node2->next = nullptr;
    LinkedListAdd(&head, &node2);
    EXPECT_EQ(head, node2);
    EXPECT_EQ(node2->next, node1);

    free(node1);
    free(node2);
}

extern "C" bool IsMemoryLow(pid_t pid);
TEST_F(ManageTest, TestIsMemoryLowFalse)
{
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = nullptr;
    bool ret = IsMemoryLow(9999);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestIsMemoryLowTrue)
{
    ProcessAttr attr = {};
    attr.pid = 1234;
    attr.isLowMem = true;
    attr.next = nullptr;
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = &attr;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&attr));
    bool ret = IsMemoryLow(1234);
    EXPECT_EQ(true, ret);
    g_processManager.processes = nullptr;
}

extern "C" int AddProcess(ProcessParam *param, PidType type, uint32_t *nodeBitmap);
TEST_F(ManageTest, TestAddProcessNormal)
{
    ProcessParam param = {};
    param.pid = 123;
    param.scanTime = 50;
    param.duration = 1;
    param.count = 1;
    param.numaParam[0].nid = 4;
    param.numaParam[0].ratio = 50;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(0));
    MOCKER(GetPidNrPages).stubs().will(returnValue(0x100));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(SetLocalNumaByCpu).stubs().will(invoke(AddAffinityLocalForTest));
    MOCKER(GetProcessNumaMapsObservation).stubs().will(invoke(AddEmptyCandidateResidentForTest));
    int ret = AddProcess(&param, VM_TYPE, nullptr);
    EXPECT_EQ(0, ret);
    EXPECT_NE(nullptr, g_processManager.processes);
    free(g_processManager.processes);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestSetLocalNumaByCpu)
{
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    uint32_t nodeBitmap = 0;
    MOCKER(sched_getaffinity).stubs().will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).stubs().will(returnValue(0));
    int ret = SetLocalNumaByCpu(1, &nodeBitmap);
    EXPECT_EQ(0, ret);
}

extern "C" bool IsRemoteNidValid(int nid);
TEST_F(ManageTest, TestIsRemoteNidValid)
{
    g_processManager.nrLocalNuma = 4;
    bool ret = IsRemoteNidValid(4);
    EXPECT_EQ(true, ret);
    ret = IsRemoteNidValid(0);
    EXPECT_EQ(false, ret);
}

extern "C" int InitGroupedUsedPages(pid_t pid, GroupMigrationPolicy *policy, const uint64_t numaPages[MAX_NODES]);
TEST_F(ManageTest, TestInitGroupedUsedPages)
{
    pid_t pid = 1234;
    GroupMigrationPolicy policy = {};
    uint64_t numaPages[MAX_NODES] = {0};
    policy.enabled = true;
    policy.groupCount = 1;
    policy.groups[0].targetCount = 1;
    policy.groups[0].targets[0].nid = 4;
    policy.groups[0].targets[0].quotaPages = 10; // quotaPages must be set
    numaPages[4] = 5;                            // residentPages <= quotaPages
    g_processManager.nrLocalNuma = 4;
    int ret = InitGroupedUsedPages(pid, &policy, numaPages);
    EXPECT_EQ(0, ret);
    EXPECT_EQ((uint64_t)5, policy.groups[0].targets[0].usedPages);
}

// === Additional manage.c tests for uncovered getter/setter and auto_remove ===

extern "C" RunMode GetRunMode(void);
TEST_F(ManageTest, TestGetRunMode)
{
    g_runMode = WATERLINE_MODE;
    RunMode ret = GetRunMode();
    EXPECT_EQ(WATERLINE_MODE, ret);
}

extern "C" bool IsHugeMode(void);
TEST_F(ManageTest, TestGetCurrentMaxNrPid4K)
{
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    int ret = GetCurrentMaxNrPid();
    EXPECT_EQ(MAX_4K_PROCESSES_CNT, ret);
}

TEST_F(ManageTest, TestGetCurrentMaxNrPid2M)
{
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    int ret = GetCurrentMaxNrPid();
    EXPECT_EQ(MAX_2M_PROCESSES_CNT, ret);
    g_processManager.tracking.pageSize = PAGESIZE_4K;
}

extern "C" ProcessAttr *GetProcessAttr(pid_t pid);
TEST_F(ManageTest, TestGetProcessAttrFound)
{
    ProcessAttr p1 = {.pid = 1, .next = nullptr};
    ProcessAttr p2 = {.pid = 2, .next = nullptr};
    p1.next = &p2;
    g_processManager.processes = &p1;
    ProcessAttr *ret = GetProcessAttr(1);
    EXPECT_EQ(1, ret->pid);

    ret = GetProcessAttr(2);
    EXPECT_EQ(2, ret->pid);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestGetProcessAttrNotFound)
{
    ProcessAttr p1 = {.pid = 1, .next = nullptr};
    g_processManager.processes = &p1;
    ProcessAttr *ret = GetProcessAttr(3);
    EXPECT_EQ(nullptr, ret);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestGetProcessAttrNullList)
{
    g_processManager.processes = nullptr;
    ProcessAttr *ret = GetProcessAttr(1);
    EXPECT_EQ(nullptr, ret);
}

TEST_F(ManageTest, TestIsMemoryLowPathFailed2)
{
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    bool ret = IsMemoryLow(123);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestIsMemoryLowFileOpenFailed2)
{
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    bool ret = IsMemoryLow(123);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestIsMemoryLowNormal2)
{
    static FILE fake_file;
    char buf[] = "100";
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    MOCKER(fgets).stubs().will(returnValue(&buf[0])).then(returnValue(static_cast<char *>(nullptr)));
    MOCKER(fclose).stubs().will(returnValue(0));
    MOCKER((int (*)(char const *, char const *, void *))sscanf_s).stubs().will(returnValue(1));
    bool ret = IsMemoryLow(123);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestSetLocalNumaByCpuAffinityFailed)
{
    MOCKER(sched_getaffinity).stubs().will(returnValue(-1));
    uint32_t nodeBitmap = 0;
    int ret = SetLocalNumaByCpu(1, &nodeBitmap);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" void LinkedListAdd(ProcessAttr **head, ProcessAttr **add);
extern "C" void FreeProceccesAttr(ProcessAttr *attr);
TEST_F(ManageTest, TestFreeProceccesAttrNull)
{
    FreeProceccesAttr(nullptr);
    // Should not crash
}

extern "C" void SetAdaptMem(bool enable);
extern "C" bool g_adaptLocalMem;
TEST_F(ManageTest, TestSetAdaptMemEnable)
{
    SetAdaptMem(true);
    EXPECT_EQ(true, g_adaptLocalMem);
}

TEST_F(ManageTest, TestSetAdaptMemDisable)
{
    SetAdaptMem(false);
    EXPECT_EQ(false, g_adaptLocalMem);
}

extern "C" int CheckPid(pid_t pid);
TEST_F(ManageTest, TestCheckPidValid)
{
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(PidIsValid).stubs().will(returnValue(true));
    MOCKER(IsQemuTask).stubs().will(returnValue((int)VM_TYPE));
    int ret = CheckPid(1);
    EXPECT_EQ(0, ret);
}

extern "C" bool IsMultiNumaVm(ProcessAttr *process);
TEST_F(ManageTest, TestIsMultiNumaVmSingleRemote)
{
    ProcessAttr attr = {};
    attr.type = VM_TYPE;
    attr.remoteNumaCnt = 1;
    attr.numaAttr.numaNodes = 0b00000001;
    g_processManager.nrLocalNuma = 4;
    bool ret = IsMultiNumaVm(&attr);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestIsMultiNumaVmMultiRemote)
{
    ProcessAttr attr = {};
    attr.type = VM_TYPE;
    attr.remoteNumaCnt = 2;
    bool ret = IsMultiNumaVm(&attr);
    EXPECT_EQ(true, ret);
}

TEST_F(ManageTest, TestIsMultiNumaVmProcessType)
{
    ProcessAttr attr = {};
    attr.type = PROCESS_TYPE;
    attr.remoteNumaCnt = 2;
    bool ret = IsMultiNumaVm(&attr);
    EXPECT_EQ(false, ret);
}

extern "C" int DestroyProcessManager();
TEST_F(ManageTest, TestDestroyProcessManagerWithProcesses)
{
    ProcessAttr attr = {};
    attr.pid = 1;
    attr.next = nullptr;
    g_processManager.processes = &attr;
    EnvMutexInit(&g_processManager.lock);
    EnvMutexInit(&g_processManager.threadLock);
    MOCKER(AccessIoctlRemoveAllPid).stubs().will(returnValue(0));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    int ret = DestroyProcessManager();
    EXPECT_EQ(0, ret);
}

extern "C" int GetNrLocalNuma(void);
extern "C" uint32_t GetNormalPageSize(void);
extern "C" uint32_t GetHugePageSize(void);
extern "C" uint32_t GetPageSize(void);
extern "C" int AddProcess(ProcessParam *param, PidType type, uint32_t *nodeBitmap);
TEST_F(ManageTest, TestAddProcessLimitReached)
{
    uint32_t savedNr = g_processManager.nr[VM_TYPE];
    g_processManager.nr[VM_TYPE] = MAX_2M_PROCESSES_CNT;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ProcessParam param = {};
    param.pid = 123;
    param.count = 1;
    int ret = AddProcess(&param, VM_TYPE, nullptr);
    EXPECT_EQ(-EINVAL, ret);
    g_processManager.nr[VM_TYPE] = savedNr;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
}

/* ====== Coverage-boosting tests for manage.c ====== */

extern "C" bool IsZeroRemoteTargetConfig(ProcessParam *param);
TEST_F(ManageTest, TestIsZeroRemoteTargetConfigNull)
{
    bool ret = IsZeroRemoteTargetConfig(nullptr);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestIsZeroRemoteTargetConfigZeroCount)
{
    ProcessParam param = {.count = 0};
    bool ret = IsZeroRemoteTargetConfig(&param);
    EXPECT_EQ(true, ret);
}

extern "C" uint8_t g_criticalErrNodes[REMOTE_NUMA_BITS];
extern "C" void UpdateRemoteNumaCriticalErr(void);
extern "C" bool IsRemoteNumaCriticalErr(int nid);

TEST_F(ManageTest, TestIsRemoteNumaCriticalErrNidBelowLocal)
{
    g_processManager.nrLocalNuma = 4;
    bool ret = IsRemoteNumaCriticalErr(2);
    EXPECT_FALSE(ret);
}

TEST_F(ManageTest, TestIsRemoteNumaCriticalErrNidOutOfRange)
{
    g_processManager.nrLocalNuma = 4;
    bool ret = IsRemoteNumaCriticalErr(4 + REMOTE_NUMA_BITS);
    EXPECT_FALSE(ret);
}

TEST_F(ManageTest, TestIsRemoteNumaCriticalErrNotCritical)
{
    g_processManager.nrLocalNuma = 4;
    g_criticalErrNodes[0] = 0;
    bool ret = IsRemoteNumaCriticalErr(4);
    EXPECT_FALSE(ret);
}

TEST_F(ManageTest, TestIsRemoteNumaCriticalErrIsCritical)
{
    g_processManager.nrLocalNuma = 4;
    g_criticalErrNodes[0] = 1;
    bool ret = IsRemoteNumaCriticalErr(4);
    EXPECT_TRUE(ret);
    g_criticalErrNodes[0] = 0;
}

TEST_F(ManageTest, TestUpdateRemoteNumaCriticalErr)
{
    g_processManager.nrLocalNuma = 4;
    memset(g_criticalErrNodes, 0, sizeof(g_criticalErrNodes));
    UpdateRemoteNumaCriticalErr();
    UpdateRemoteNumaCriticalErr();
    UpdateRemoteNumaCriticalErr();
}

TEST_F(ManageTest, TestProcessManagerInitCriticalErrNodesFailed)
{
    uint32_t pageType = PAGETYPE_NORMAL;
    int ret;
    MOCKER(memset_s).stubs().will(returnValue(EOK)).then(returnValue(1));
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(-1, ret);
    GlobalMockObject::verify();
}
