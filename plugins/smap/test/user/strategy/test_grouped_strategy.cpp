/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * Description: smap grouped migration strategy unit tests
 */

#include <cstdlib>
#include <gtest/gtest.h>
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "strategy/strategy.h"
#include "strategy/grouped_strategy.h"
#include "strategy/strategy_config.h"

class GroupedStrategyTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

static void FreeMigList(struct MigList mlist[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            free(mlist[i][j].addr);
            mlist[i][j].addr = nullptr;
        }
    }
}

static void ExpectMigListEmpty(struct MigList mlist[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            EXPECT_EQ(0, mlist[i][j].nr);
        }
    }
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsDemoteByLocalLimitAndQuota)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[3] = {};

    process.pid = 100;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;

    localPages[0].addr = 0x3000;
    localPages[0].freq = 3;
    localPages[1].addr = 0x1000;
    localPages[1].freq = 1;
    localPages[2].addr = 0x2000;
    localPages[2].freq = 2;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 3;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    int ret = GroupedMigrationStrategy(&process, mlist);
    EXPECT_EQ(0, ret);
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsPromoteByLocalDeficit)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[1] = {};
    ActcData remotePages[2] = {};

    process.pid = 101;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 2;
    process.groupPolicy.groups[0].locals[0].localReservePages = 3;

    localPages[0].addr = 0x1000;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 4;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 5;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 2;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    int ret = GroupedMigrationStrategy(&process, mlist);
    EXPECT_EQ(0, ret);
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySyncRemoteUsedPagesWithoutPromote)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[1] = {};
    ActcData remotePages[2] = {};

    process.pid = 113;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 3;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 0;

    localPages[0].addr = 0x1000;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 4;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 5;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 2;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ((uint64_t)2, process.groupPolicy.groups[0].targets[0].usedPages);
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySyncRemoteUsedPagesAllowsOverQuotaWithoutPromote)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData remotePages[3] = {};

    process.pid = 114;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 3;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 1;

    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 4;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 6;
    remotePages[2].addr = 0x6000;
    remotePages[2].freq = 5;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 3;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ((uint64_t)3, process.groupPolicy.groups[0].targets[0].usedPages);
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySyncSharedTargetUsedPagesByQuotaWithoutPromote)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData remotePages[4] = {};

    process.pid = 115;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 2;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 1;
    process.groupPolicy.groups[1].localCount = 1;
    process.groupPolicy.groups[1].locals[0].nid = 1;
    process.groupPolicy.groups[1].locals[0].localReservePages = 3;
    process.groupPolicy.groups[1].targetCount = 1;
    process.groupPolicy.groups[1].targets[0].nid = 4;
    process.groupPolicy.groups[1].targets[0].quotaPages = 3;

    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 20;
    remotePages[2].addr = 0x6000;
    remotePages[2].freq = 30;
    remotePages[3].addr = 0x7000;
    remotePages[3].freq = 40;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 4;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ((uint64_t)1, process.groupPolicy.groups[0].targets[0].usedPages);
    EXPECT_EQ((uint64_t)3, process.groupPolicy.groups[1].targets[0].usedPages);
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsPromoteWhenAnyLocalBelowReserve)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData local0[1] = {};
    ActcData local1[3] = {};
    ActcData remotePages[2] = {};

    process.pid = 107;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 2;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 3;
    process.groupPolicy.groups[0].locals[1].nid = 1;
    process.groupPolicy.groups[0].locals[1].localReservePages = 1;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 2;

    process.scanAttr.actcData[0] = local0;
    process.scanAttr.actcLen[0] = 1;
    process.scanAttr.actcData[1] = local1;
    process.scanAttr.actcLen[1] = 3;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 9;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 2;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategyLocalDeficitSkipsSingleDirectionMigration)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData local0[1] = {};
    ActcData local1[3] = {};

    process.pid = 110;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 2;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 3;
    process.groupPolicy.groups[0].locals[1].nid = 1;
    process.groupPolicy.groups[0].locals[1].localReservePages = 1;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;

    process.scanAttr.actcData[0] = local0;
    process.scanAttr.actcLen[0] = 1;
    local1[0].addr = 0x2000;
    local1[0].freq = 1;
    local1[1].addr = 0x3000;
    local1[1].freq = 2;
    local1[2].addr = 0x4000;
    local1[2].freq = 3;
    process.scanAttr.actcData[1] = local1;
    process.scanAttr.actcLen[1] = 3;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsDemoteFromLocalAboveReserve)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData local0[1] = {};
    ActcData local1[3] = {};

    process.pid = 108;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 2;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[0].locals[1].nid = 1;
    process.groupPolicy.groups[0].locals[1].localReservePages = 1;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;

    local0[0].addr = 0x1000;
    local0[0].freq = 0;
    process.scanAttr.actcData[0] = local0;
    process.scanAttr.actcLen[0] = 1;
    local1[0].addr = 0x2000;
    local1[0].freq = 3;
    local1[1].addr = 0x3000;
    local1[1].freq = 1;
    local1[2].addr = 0x4000;
    local1[2].freq = 2;
    process.scanAttr.actcData[1] = local1;
    process.scanAttr.actcLen[1] = 3;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsPromoteForLargerLocalDeficit)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData remotePages[2] = {};

    process.pid = 109;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 2;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[0].locals[1].nid = 1;
    process.groupPolicy.groups[0].locals[1].localReservePages = 3;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 2;

    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 9;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 2;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsPromoteGroupDeficitOrdering)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData remote4[1] = {};
    ActcData remote5[1] = {};

    process.pid = 111;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 2;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;
    process.groupPolicy.groups[1].localCount = 1;
    process.groupPolicy.groups[1].locals[0].nid = 1;
    process.groupPolicy.groups[1].locals[0].localReservePages = 3;
    process.groupPolicy.groups[1].targetCount = 1;
    process.groupPolicy.groups[1].targets[0].nid = 5;
    process.groupPolicy.groups[1].targets[0].quotaPages = 10;
    process.groupPolicy.groups[1].targets[0].usedPages = 1;

    remote4[0].addr = 0x4000;
    remote4[0].freq = 4;
    process.scanAttr.actcData[4] = remote4;
    process.scanAttr.actcLen[4] = 1;
    remote5[0].addr = 0x5000;
    remote5[0].freq = 5;
    process.scanAttr.actcData[5] = remote5;
    process.scanAttr.actcLen[5] = 1;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestUpdateGroupedMigrationResult)
{
    ProcessAttr process = {};

    process.pid = 102;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;

    UpdateGroupedMigrationResult(&process, 0, 4, 3);
    EXPECT_EQ(3, process.groupPolicy.groups[0].targets[0].usedPages);

    UpdateGroupedMigrationResult(&process, 4, 0, 2);
    EXPECT_EQ(1, process.groupPolicy.groups[0].targets[0].usedPages);

    UpdateGroupedMigrationResult(&process, 4, 0, 2);
    EXPECT_EQ(0, process.groupPolicy.groups[0].targets[0].usedPages);

    UpdateGroupedMigrationResult(&process, 0, 0, 1);
    EXPECT_EQ(0, process.groupPolicy.groups[0].targets[0].usedPages);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySkipsLocalRebalanceAfterPromote)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData local0[4] = {};
    ActcData local1[1] = {};
    ActcData remotePages[1] = {};

    process.pid = 112;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 2;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[0].locals[1].nid = 1;
    process.groupPolicy.groups[0].locals[1].localReservePages = 3;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;

    local0[0].addr = 0x1000;
    local0[0].freq = 1;
    local0[1].addr = 0x2000;
    local0[1].freq = 2;
    local0[2].addr = 0x3000;
    local0[2].freq = 3;
    local0[3].addr = 0x4000;
    local0[3].freq = 4;
    process.scanAttr.actcData[0] = local0;
    process.scanAttr.actcLen[0] = 4;
    process.scanAttr.actcData[1] = local1;
    process.scanAttr.actcLen[1] = 1;
    remotePages[0].addr = 0x5000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    ExpectMigListEmpty(mlist);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapAfterStableRounds)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[2] = {};
    ActcData remotePages[2] = {};

    process.pid = 103;
    process.enableSwap = true;
    process.separateParam.maxMigrate = 10;
    process.separateParam.freqWt = 1;
    process.separateParam.slowThred = 1;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 2;
    process.groupPolicy.groups[0].locals[0].localReservePages = 2;

    localPages[0].addr = 0x1000;
    localPages[0].freq = 1;
    localPages[1].addr = 0x2000;
    localPages[1].freq = 2;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 2;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 8;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 2;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)100));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(GetGroupSwapRatioConfig).stubs().will(returnValue((uint32_t)5));
    MOCKER(GetGroupSwapMinRemoteFreqConfig).stubs().will(returnValue((uint32_t)0));
    MOCKER(GetGroupSwapMinFreqGainConfig).stubs().will(returnValue((uint32_t)0));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(1, mlist[4][0].nr);
    EXPECT_EQ(0x4000, mlist[4][0].addr[0]);
    EXPECT_EQ(1, mlist[0][4].nr);
    EXPECT_EQ(0x1000, mlist[0][4].addr[0]);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapUsesLocalWatermarkRatio)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[95] = {};
    ActcData remotePages[1] = {};

    process.pid = 116;
    process.enableSwap = true;
    process.separateParam.maxMigrate = 10;
    process.separateParam.freqWt = 1;
    process.separateParam.slowThred = 1;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 100;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;

    for (uint64_t i = 0; i < 95; i++) {
        localPages[i].addr = 0x1000 + i;
        localPages[i].freq = 1;
    }
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 95;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)95));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(GetGroupSwapRatioConfig).stubs().will(returnValue((uint32_t)5));
    MOCKER(GetGroupSwapMinRemoteFreqConfig).stubs().will(returnValue((uint32_t)0));
    MOCKER(GetGroupSwapMinFreqGainConfig).stubs().will(returnValue((uint32_t)0));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(1, mlist[4][0].nr);
    EXPECT_EQ(0x4000, mlist[4][0].addr[0]);
    EXPECT_EQ(1, mlist[0][4].nr);
    EXPECT_EQ(0x1000, mlist[0][4].addr[0]);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapWaitsForStableTotalPages)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[2] = {};
    ActcData remotePages[2] = {};

    process.pid = 118;
    process.enableSwap = true;
    process.separateParam.maxMigrate = 10;
    process.separateParam.freqWt = 1;
    process.separateParam.slowThred = 1;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 2;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;

    localPages[0].addr = 0x1000;
    localPages[0].freq = 1;
    localPages[1].addr = 0x2000;
    localPages[1].freq = 2;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 2;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    remotePages[1].addr = 0x5000;
    remotePages[1].freq = 8;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)100));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(GetGroupSwapRatioConfig).stubs().will(returnValue((uint32_t)5));
    MOCKER(GetGroupSwapMinRemoteFreqConfig).stubs().will(returnValue((uint32_t)0));
    MOCKER(GetGroupSwapMinFreqGainConfig).stubs().will(returnValue((uint32_t)0));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    process.scanAttr.actcLen[4] = 2;
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(1, mlist[4][0].nr);
    EXPECT_EQ(0x4000, mlist[4][0].addr[0]);
    EXPECT_EQ(1, mlist[0][4].nr);
    EXPECT_EQ(0x1000, mlist[0][4].addr[0]);

    FreeMigList(mlist);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapBelowLocalWatermarkRatio)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[94] = {};
    ActcData remotePages[1] = {};

    process.pid = 117;
    process.enableSwap = true;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].locals[0].localReservePages = 100;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;

    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 94;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)95));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapDisabled)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[1] = {};
    ActcData remotePages[1] = {};

    process.pid = 104;
    process.enableSwap = false;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;

    localPages[0].addr = 0x1000;
    localPages[0].freq = 0;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)100));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapFrozen)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[1] = {};
    ActcData remotePages[1] = {};

    process.pid = 119;
    process.enableSwap = true;
    process.groupSwapFrozen = true;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;

    localPages[0].addr = 0x1000;
    localPages[0].freq = 0;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapSkipsSharedTarget)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages0[1] = {};
    ActcData localPages1[1] = {};
    ActcData remotePages[1] = {};

    process.pid = 105;
    process.enableSwap = true;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 2;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;
    process.groupPolicy.groups[1].localCount = 1;
    process.groupPolicy.groups[1].locals[0].nid = 1;
    process.groupPolicy.groups[1].targetCount = 1;
    process.groupPolicy.groups[1].targets[0].nid = 4;
    process.groupPolicy.groups[1].targets[0].quotaPages = 10;
    process.groupPolicy.groups[1].locals[0].localReservePages = 1;

    localPages0[0].addr = 0x1000;
    localPages0[0].freq = 0;
    process.scanAttr.actcData[0] = localPages0;
    process.scanAttr.actcLen[0] = 1;
    localPages1[0].addr = 0x2000;
    process.scanAttr.actcData[1] = localPages1;
    process.scanAttr.actcLen[1] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 10;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)100));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);
}

TEST_F(GroupedStrategyTest, TestGroupedStrategySwapRequiresHotColdGap)
{
    ProcessAttr process = {};
    struct MigList mlist[MAX_NODES][MAX_NODES] = {};
    ActcData localPages[1] = {};
    ActcData remotePages[1] = {};

    process.pid = 106;
    process.enableSwap = true;
    process.separateParam.maxMigrate = 10;
    process.separateParam.freqWt = 1;
    process.separateParam.slowThred = 1;
    process.groupPolicy.enabled = true;
    process.groupPolicy.groupCount = 1;
    process.groupPolicy.groups[0].localCount = 1;
    process.groupPolicy.groups[0].locals[0].nid = 0;
    process.groupPolicy.groups[0].targetCount = 1;
    process.groupPolicy.groups[0].targets[0].nid = 4;
    process.groupPolicy.groups[0].targets[0].quotaPages = 10;
    process.groupPolicy.groups[0].targets[0].usedPages = 1;
    process.groupPolicy.groups[0].locals[0].localReservePages = 1;

    localPages[0].addr = 0x1000;
    localPages[0].freq = 8;
    process.scanAttr.actcData[0] = localPages;
    process.scanAttr.actcLen[0] = 1;
    remotePages[0].addr = 0x4000;
    remotePages[0].freq = 9;
    process.scanAttr.actcData[4] = remotePages;
    process.scanAttr.actcLen[4] = 1;

    MOCKER(GetGroupSwapLocalWatermarkRatioConfig).stubs().will(returnValue((uint32_t)100));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)10));
    MOCKER(GetGroupSwapRatioConfig).stubs().will(returnValue((uint32_t)5));
    MOCKER(GetGroupSwapMinRemoteFreqConfig).stubs().will(returnValue((uint32_t)0));
    MOCKER(GetGroupSwapMinFreqGainConfig).stubs().will(returnValue((uint32_t)0));

    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, GroupedMigrationStrategy(&process, mlist));
    EXPECT_EQ(0, mlist[4][0].nr);
    EXPECT_EQ(0, mlist[0][4].nr);
}
