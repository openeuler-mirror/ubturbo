/*
 * Description: smap5.0 user separate strategy ut code
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include "manage/manage.h"
#include "manage/node_matrix.h"
#include "strategy/strategy.h"
#include "strategy/separate_strategy.h"
#include "strategy/strategy_config.h"
#include "smap_user_log.h"

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

// Test-only compat: old compile-time NUMA constants (8 local + 18 remote)
#define OLD_REMOTE_NUMA_NUM 18
#define OLD_REMOTE_NUMA_BITS 18
#define OLD_LOCAL_NUMA_NUM 8
#define OLD_LOCAL_NUMA_BITS 8
#define OLD_MAX_NODES (OLD_LOCAL_NUMA_NUM + OLD_REMOTE_NUMA_NUM)

const int DEFAULT_FREQ_WT_2M = 1;
const int DEFAULT_SLOW_THRED_2M = 2 * DEFAULT_FREQ_WT_2M;
const int MAX_MIGRATE_DIVISOR_2M = 2;

using namespace std;

/* cap=1024 时 mlist 全矩阵约 50MB，超出栈预算；gtest 用例串行执行，共用 BSS 数组 */
static struct MigList g_testMlist[SMAP_MAX_NUMNODES][SMAP_MAX_NUMNODES];

static struct MigList *TestMlist(void)
{
    (void)memset(g_testMlist, 0, sizeof(g_testMlist));
    return &g_testMlist[0][0];
}

typedef struct NumaInfo {
    int localNid;
    int remoteNid;
} NumaInfo;

class SeparateStrategyTest : public ::testing::Test {
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

extern "C" int InitSeparateParam(ProcessAttr *process);
TEST_F(SeparateStrategyTest, TestInitSeparateParam)
{
    ProcessAttr process;
    MOCKER(GetSlowThresholdConfig).stubs().will(returnValue(40));
    InitSeparateParam(&process);
    EXPECT_EQ(DEFAULT_FREQ_WT_2M, process.separateParam.freqWt);
    EXPECT_EQ(40, process.separateParam.slowThred);
}

extern "C" bool ShouldMigrate(ProcessAttr *process);
TEST_F(SeparateStrategyTest, TestShouldMigrateOne)
{
    ProcessAttr process;
    process.separateParam.maxMigrate = 0;
    bool ret = ShouldMigrate(&process);
    EXPECT_FALSE(ret);
}

TEST_F(SeparateStrategyTest, TestShouldMigrateTwo)
{
    ProcessAttr process;
    process.separateParam.maxMigrate = 1;
    process.walkPage.nrPage = 0;
    bool ret = ShouldMigrate(&process);
    EXPECT_FALSE(ret);
}

TEST_F(SeparateStrategyTest, TestShouldMigrateThree)
{
    ProcessAttr process;
    process.separateParam.maxMigrate = 1;
    process.walkPage.nrPage = 1;
    bool ret = ShouldMigrate(&process);
    EXPECT_TRUE(ret);
}

extern "C" int SeparateStrategy(ProcessAttr *process, struct MigList *mlist);
void SeparateStrategyinit(ActcData actcData1[2], ActcData actcData2[4], ProcessAttr *process)
{
    actcData1[0].freq = 0;
    actcData1[1].freq = 2;

    actcData2[0].freq = 5;
    actcData2[1].freq = 2;
    actcData2[2].freq = 3;
    actcData2[3].freq = 1;

    process->scanAttr.actCount[0].freqNum = 50;
    process->scanAttr.actCount[0].freqMax = 100;

    process->scanAttr.actCount[4].freqNum = 100;
    process->scanAttr.actCount[4].freqMax = 200;
}

TEST_F(SeparateStrategyTest, TestPeriodConfig)
{
    /* config file is RPM-installed at a fixed path; read must not crash with or without it */
    StrategyConfigRead();
}

TEST_F(SeparateStrategyTest, TestShouldMigrate)
{
    ProcessAttr process = {};
    process.numaAttr.numaNodes = 0b10011001;
    process.separateParam.maxMigrate = 0;
    bool ret = ShouldMigrate(&process);
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    EXPECT_EQ(false, ret);

    process.separateParam.maxMigrate = 3;
    process.walkPage.nrPage = 0;
    ret = ShouldMigrate(&process);
    EXPECT_EQ(false, ret);

    process.walkPage.nrPage = 3;
    ret = ShouldMigrate(&process);
    EXPECT_EQ(true, ret);
}

extern "C" void FreeMlist(struct MigList *mlist);
TEST_F(SeparateStrategyTest, TestFreeMlist)
{
    struct MigList *mlist = TestMlist();
    NM_AT(mlist, 0, 0).addr = (uint64_t *)malloc(sizeof(uint64_t));
    FreeMlist(mlist);
    EXPECT_EQ(NULL, NM_AT(mlist, 0, 0).addr);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KShouldMigrate)
{
    ProcessAttr process;
    struct MigList *mlist = TestMlist();
    process.separateParam.freqWt = 0;

    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalcMaxMigrate).stubs().will(ignoreReturnValue());
    MOCKER(ShouldMigrate).stubs().will(returnValue(false));
    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
}

extern "C" int BuildSelectKMlistAddr(ProcessAttr *process, struct MigList *mlist,
    uint32_t numaOffset[SMAP_MAX_NUMNODES], int from, int to, SelectionMode mode);
TEST_F(SeparateStrategyTest, TestSeparateStrategy4KAbnormal)
{
    ProcessAttr process;
    struct MigList *mlist = TestMlist();
    process.separateParam.freqWt = 0;

    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalcMaxMigrate).stubs().will(ignoreReturnValue());
    MOCKER(ShouldMigrate).stubs().will(returnValue(true));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    process.numaAttr.numaNodes = 0b01100011;
    MOCKER(BuildSelectKMlistAddr).stubs().will(returnValue(-ENOMEM));
    MOCKER(FreeMlist).stubs().will(ignoreReturnValue());
    /* SeparateStrategy 按 for_each_remote_nid 遍历远端，需注册位图中的 L2 节点 5/6 */
    g_remoteNids[0] = 5;
    g_remoteNids[1] = 6;
    g_nrRemote = 2;
    int ret = SeparateStrategy(&process, mlist);
    g_nrRemote = 0;

    EXPECT_EQ(-ENOMEM, ret);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KNormal)
{
    ProcessAttr process;
    struct MigList *mlist = TestMlist();
    process.separateParam.freqWt = 0;
    process.numaAttr.numaNodes = 0b01100011;

    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalcMaxMigrate).stubs().will(ignoreReturnValue());
    MOCKER(ShouldMigrate).stubs().will(returnValue(true));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    MOCKER(BuildSelectKMlistAddr).stubs().will(returnValue(0));
    MOCKER(FreeMlist).stubs().will(ignoreReturnValue());
    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
    FreeProcessAttrArrays(&process);
}

void BuildActcData(int nid, const uint16_t data[], int size, ScanAttribute *scanAttribute)
{
    scanAttribute->actcLen[nid] = size;
    scanAttribute->actCount[nid].freqZero = 0;
    ActcData *actcData = (ActcData *)calloc(size, sizeof(ActcData));
    for (int i = 0; i < size; i++) {
        uint16_t freq = data[i];
        actcData[i].freq = freq;
        scanAttribute->actCount[nid].freqMax = std::max(scanAttribute->actCount[nid].freqMax, (actc_t)freq);
        scanAttribute->actCount[nid].freqMin = std::min(scanAttribute->actCount[nid].freqMin, (actc_t)freq);
        if (freq == 0) {
            scanAttribute->actCount[nid].freqZero++;
        } else {
            scanAttribute->actCount[nid].freqNum++;
        }
        scanAttribute->actCount[nid].freqSum += freq;
    }
    scanAttribute->actcData[nid] = actcData;
}

void initializeMigList(struct MigList *mlist)
{
    for (int i = 0; i < SMAP_MAX_NUMNODES; ++i) {
        for (int j = 0; j < SMAP_MAX_NUMNODES; ++j) {
            NM_AT(mlist, i, j).from = i;
            NM_AT(mlist, i, j).to = j;
        }
    }
}

extern "C" int PairMigrationStrategy(ProcessAttr *process, struct MigList *mlist);
static void BuildPairActcData(int nid, const uint16_t data[], int size, ScanAttribute *scanAttribute)
{
    BuildActcData(nid, data, size, scanAttribute);
    for (int i = 0; i < size; i++) {
        int bucket = std::min((int)data[i], FREQ_BUCKETS_SIZE - 1);
        scanAttribute->actCount[nid].freqBuckets[bucket]++;
    }
}

TEST_F(SeparateStrategyTest, TestPairMigrationStrategyBuilds2x2NetMigration)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    const uint16_t local0[] = {1, 4, 2};
    const uint16_t local1[] = {0, 5};
    const uint16_t remote2[] = {3, 8, 7};
    const uint16_t remote3[] = {9, 6};
    BuildPairActcData(0, local0, 3, &process.scanAttr);
    BuildPairActcData(1, local1, 2, &process.scanAttr);
    BuildPairActcData(2, remote2, 3, &process.scanAttr);
    BuildPairActcData(3, remote3, 2, &process.scanAttr);
    initializeMigList(mlist);

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigrateBackPages, 0, 2) = 2; /* promote: remote 2 -> local 0 */
    NM_AT(process.strategyAttr.nrMigratePages, 0, 3) = 2;
    NM_AT(process.strategyAttr.nrMigrateBackPages, 1, 2) = 1;
    NM_AT(process.strategyAttr.nrMigratePages, 1, 3) = 1;

    /* PairMigrationStrategy 按 for_each_remote_nid 遍历远端，需注册远端节点 2/3 */
    g_remoteNids[0] = 2;
    g_remoteNids[1] = 3;
    g_nrRemote = 2;
    ASSERT_EQ(0, PairMigrationStrategy(&process, mlist));
    g_nrRemote = 0;
    ASSERT_EQ(2U, NM_AT(mlist, 2, 0).nr);
    ASSERT_EQ(1U, NM_AT(mlist, 2, 1).nr);
    EXPECT_EQ(1U, NM_AT(mlist, 2, 0).addr[0]);
    EXPECT_EQ(2U, NM_AT(mlist, 2, 0).addr[1]);
    EXPECT_EQ(0U, NM_AT(mlist, 2, 1).addr[0]);
    ASSERT_EQ(2U, NM_AT(mlist, 0, 3).nr);
    ASSERT_EQ(1U, NM_AT(mlist, 1, 3).nr);
    EXPECT_EQ(0U, NM_AT(mlist, 0, 3).addr[0]);
    EXPECT_EQ(2U, NM_AT(mlist, 0, 3).addr[1]);
    EXPECT_EQ(0U, NM_AT(mlist, 1, 3).addr[0]);
    EXPECT_NE(NM_AT(mlist, 2, 0).addr[0], NM_AT(mlist, 2, 1).addr[0]);
    EXPECT_NE(NM_AT(mlist, 2, 0).addr[1], NM_AT(mlist, 2, 1).addr[0]);

    FreeMlist(mlist);
    for (int nid = 0; nid < 4; nid++) {
        free(process.scanAttr.actcData[nid]);
    }
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestPairMigrationStrategyBuildsPairSwapAfterNetMigration)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    const uint16_t local0[] = {0, 1, 6};
    const uint16_t remote2[] = {9, 8, 2};
    BuildPairActcData(0, local0, 3, &process.scanAttr);
    BuildPairActcData(2, remote2, 3, &process.scanAttr);
    initializeMigList(mlist);
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, 0, 2) = 3;
    NM_AT(process.strategyAttr.nrMigrateBackPages, 0, 2) = 2;

    /* PairMigrationStrategy 按 for_each_remote_nid 遍历远端，需注册远端节点 2 */
    g_remoteNids[0] = 2;
    g_nrRemote = 1;
    ASSERT_EQ(0, PairMigrationStrategy(&process, mlist));
    g_nrRemote = 0;
    ASSERT_EQ(3U, NM_AT(mlist, 0, 2).nr);
    EXPECT_EQ(0U, NM_AT(mlist, 0, 2).addr[0]);
    EXPECT_EQ(1U, NM_AT(mlist, 0, 2).addr[1]);
    EXPECT_EQ(2U, NM_AT(mlist, 0, 2).addr[2]);
    ASSERT_EQ(2U, NM_AT(mlist, 2, 0).nr);
    EXPECT_EQ(0U, NM_AT(mlist, 2, 0).addr[0]);
    EXPECT_EQ(1U, NM_AT(mlist, 2, 0).addr[1]);

    FreeMlist(mlist);
    free(process.scanAttr.actcData[0]);
    free(process.scanAttr.actcData[2]);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestPairMigrationStrategyUbBwSwapStopStripsSwap)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    const uint16_t local0[] = {0, 1, 6};
    const uint16_t remote2[] = {9, 8, 2};
    BuildPairActcData(0, local0, 3, &process.scanAttr);
    BuildPairActcData(2, remote2, 3, &process.scanAttr);
    initializeMigList(mlist);
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, 0, 2) = 3; /* local -> remote */
    NM_AT(process.strategyAttr.nrMigrateBackPages, 0, 2) = 2; /* remote -> local */
    process.strategyAttr.ubBwRestrict[2] = UB_BW_SWAP_STOP;

    /* PairMigrationStrategy 按 for_each_remote_nid 遍历远端，需注册远端节点 2 */
    g_remoteNids[0] = 2;
    g_nrRemote = 1;
    ASSERT_EQ(0, PairMigrationStrategy(&process, mlist));
    g_nrRemote = 0;

    /* swapPages = MIN(3, 2) = 2 -> 0; demotePages = 3 - 2 = 1, promotePages = 2 - 2 = 0.
     * UB_BW_SWAP_STOP only strips swap, net demote/promote stays. */
    ASSERT_EQ(1U, NM_AT(mlist, 0, 2).nr);
    EXPECT_EQ(0U, NM_AT(mlist, 0, 2).addr[0]);
    ASSERT_EQ(0U, NM_AT(mlist, 2, 0).nr);
    EXPECT_EQ(nullptr, NM_AT(mlist, 2, 0).addr);

    FreeMlist(mlist);
    free(process.scanAttr.actcData[0]);
    free(process.scanAttr.actcData[2]);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestPairMigrationStrategyDisabledRemoteSkipsBothDirections)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    const uint16_t remote2[] = {5};
    BuildPairActcData(2, remote2, 1, &process.scanAttr);
    initializeMigList(mlist);
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, 0, 2) = 1;
    NM_AT(process.strategyAttr.nrMigrateBackPages, 0, 2) = 1;
    EnvAtomicSet(&g_forbiddenNodes[2], 1);

    int ret = PairMigrationStrategy(&process, mlist);
    EnvAtomicSet(&g_forbiddenNodes[2], 0);
    ASSERT_EQ(0, ret);
    EXPECT_EQ(0U, NM_AT(mlist, 0, 2).nr);
    EXPECT_EQ(0U, NM_AT(mlist, 2, 0).nr);

    FreeMlist(mlist);
    free(process.scanAttr.actcData[2]);
    FreeProcessAttrArrays(&process);
}

extern "C" uint64_t GetNrFreePagesByNode(int nid);
TEST_F(SeparateStrategyTest, TestSeparateStrategy4KSwapBasic)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.numaAttr.numaNodes = 0b00100001;
    process.walkPage.nrPage = 10;
    process.separateParam.freqWt = 0;
    int nodeL0 = 0;
    int nodeR5 = 5;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, nodeL0, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL0, nodeR5) = 0;

    uint16_t nodeL0Data[] = {0, 2, 3, 0, 8, 1, 4, 0, 0, 0};
    uint16_t nodeR5Data[] = {0, 0, 1, 2, 3, 7, 6, 5, 4, 8};

    BuildActcData(nodeL0, nodeL0Data, sizeof(nodeL0Data) / sizeof(nodeL0Data[0]), &process.scanAttr);
    BuildActcData(nodeR5, nodeR5Data, sizeof(nodeR5Data) / sizeof(nodeR5Data[0]), &process.scanAttr);

    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue(10));

    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KSwapBasicDemote)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.numaAttr.numaNodes = 0b00100001;
    process.walkPage.nrPage = 10;
    process.separateParam.freqWt = 0;
    int nodeL0 = 0;
    int nodeR5 = 5;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, nodeL0, nodeR5) = 2;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL0, nodeR5) = 0;

    uint16_t nodeL0Data[] = {0, 2, 3, 0, 8, 1, 4, 0, 0, 0};
    uint16_t nodeR5Data[] = {0, 0, 1, 2, 3, 7, 6, 5, 4, 8};

    BuildActcData(nodeL0, nodeL0Data, sizeof(nodeL0Data) / sizeof(nodeL0Data[0]), &process.scanAttr);
    BuildActcData(nodeR5, nodeR5Data, sizeof(nodeR5Data) / sizeof(nodeR5Data[0]), &process.scanAttr);

    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue(10));

    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KSwapBasicPromote)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.numaAttr.numaNodes = 0b00100001;
    process.walkPage.nrPage = 10;
    process.separateParam.freqWt = 0;
    int nodeL0 = 0;
    int nodeR5 = 5;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, nodeL0, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL0, nodeR5) = 2;

    uint16_t nodeL0Data[] = {0, 2, 3, 0, 8, 1, 4, 0, 0, 0};
    uint16_t nodeR5Data[] = {0, 0, 1, 2, 3, 7, 6, 5, 4, 8};

    BuildActcData(nodeL0, nodeL0Data, sizeof(nodeL0Data) / sizeof(nodeL0Data[0]), &process.scanAttr);
    BuildActcData(nodeR5, nodeR5Data, sizeof(nodeR5Data) / sizeof(nodeR5Data[0]), &process.scanAttr);

    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue(10));

    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KSwapMultiNode)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.numaAttr.numaNodes = 0b00100011;
    process.walkPage.nrPage = 10;
    process.separateParam.freqWt = 0;
    int nodeL0 = 0;
    int nodeL1 = 1;
    int nodeR5 = 5;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, nodeL0, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL0, nodeR5) = 2;
    NM_AT(process.strategyAttr.nrMigratePages, nodeL1, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL1, nodeR5) = 8;

    uint16_t nodeL0Data[] = {0, 2, 3, 0, 8, 1, 4, 0, 0, 0};
    uint16_t nodeL1Data[] = {0, 0, 4, 6, 8, 0, 0, 0, 0, 0};
    uint16_t nodeR5Data[] = {1, 1, 1, 2, 3, 7, 6, 5, 4, 8};

    BuildActcData(nodeL0, nodeL0Data, sizeof(nodeL0Data) / sizeof(nodeL0Data[0]), &process.scanAttr);
    BuildActcData(nodeL1, nodeL1Data, sizeof(nodeL1Data) / sizeof(nodeL1Data[0]), &process.scanAttr);
    BuildActcData(nodeR5, nodeR5Data, sizeof(nodeR5Data) / sizeof(nodeR5Data[0]), &process.scanAttr);

    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue(20));

    /* SeparateStrategy 按 g_remoteNids 遍历远端节点，需注册远端节点 5 */
    g_remoteNids[0] = nodeR5;
    g_nrRemote = 1;

    int ret = SeparateStrategy(&process, mlist);
    g_nrRemote = 0;

    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, NM_AT(mlist, 0, 5).nr);
    EXPECT_EQ(2, NM_AT(mlist, 5, 0).nr);
    EXPECT_EQ(0, NM_AT(mlist, 1, 5).nr);
    EXPECT_EQ(8, NM_AT(mlist, 5, 1).nr);
    FreeProcessAttrArrays(&process);
}

TEST_F(SeparateStrategyTest, TestSeparateStrategy4KSwapLimitByFreePage)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.numaAttr.numaNodes = 0b00100011;
    process.walkPage.nrPage = 10;
    process.separateParam.freqWt = 0;
    int nodeL0 = 0;
    int nodeL1 = 1;
    int nodeR5 = 5;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ASSERT_EQ(0, InitProcessAttrArrays(&process));
    NM_AT(process.strategyAttr.nrMigratePages, nodeL0, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL0, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigratePages, nodeL1, nodeR5) = 0;
    NM_AT(process.strategyAttr.nrMigrateBackPages, nodeL1, nodeR5) = 0;

    uint16_t nodeL0Data[] = {0, 2, 3, 0, 8, 1, 4, 0, 0, 0};
    uint16_t nodeL1Data[] = {0, 0, 4, 6, 8, 0, 0, 0, 0, 0};
    uint16_t nodeR5Data[] = {0, 0, 1, 2, 3, 7, 6, 5, 4, 8};

    BuildActcData(nodeL0, nodeL0Data, sizeof(nodeL0Data) / sizeof(nodeL0Data[0]), &process.scanAttr);
    BuildActcData(nodeL1, nodeL1Data, sizeof(nodeL1Data) / sizeof(nodeL1Data[0]), &process.scanAttr);
    BuildActcData(nodeR5, nodeR5Data, sizeof(nodeR5Data) / sizeof(nodeR5Data[0]), &process.scanAttr);

    MOCKER(GetNrFreePagesByNode).stubs().will(returnValue(6));

    int ret = SeparateStrategy(&process, mlist);

    EXPECT_EQ(0, ret);
    FreeProcessAttrArrays(&process);
}

extern "C" void CalculateMigInfo(ProcessAttr *process, RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES],
                                 uint64_t nrPages[NR_LEVEL], uint64_t *demoteNum, uint64_t *promoteNum);

TEST_F(SeparateStrategyTest, TestCalculateMigInfo)
{
    int nrLocalNuma = 4;
    ProcessAttr process = {};
    process.remoteNumaCnt = 1;
    process.migrateParam[0].nid = 4;
    int nid = process.migrateParam[0].nid;
    process.migrateParam[0].memSize = 4096;
    process.scanAttr.actcLen[nid] = 1;
    uint64_t demoteNum = 0;
    uint64_t promoteNum = 0;
    RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES] = { 0 };
    uint64_t nrPages[NR_LEVEL] = { 0 };

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(nrLocalNuma)); // 本地4个numa
    CalculateMigInfo(&process, remoteMigInfo, nrPages, &demoteNum, &promoteNum);
    EXPECT_EQ(DEMOTE, remoteMigInfo[nid].dir);
    EXPECT_EQ(1, remoteMigInfo[nid].nrMig);
    EXPECT_EQ(remoteMigInfo[nid].nrMig, demoteNum);
    EXPECT_EQ(0, promoteNum);

    process.scanAttr.actcLen[nid] = 3;
    demoteNum = 0;
    promoteNum = 0;
    CalculateMigInfo(&process, remoteMigInfo, nrPages, &demoteNum, &promoteNum);
    EXPECT_EQ(PROMOTE, remoteMigInfo[nid].dir);
    EXPECT_EQ(1, remoteMigInfo[nid].nrMig);
    EXPECT_EQ(0, demoteNum);
    EXPECT_EQ(remoteMigInfo[nid].nrMig, promoteNum);

    process.scanAttr.actcLen[nid] = 2;
    demoteNum = 0;
    promoteNum = 0;
    CalculateMigInfo(&process, remoteMigInfo, nrPages, &demoteNum, &promoteNum);
    EXPECT_EQ(SWAP, remoteMigInfo[nid].dir);
    EXPECT_EQ(0, remoteMigInfo[nid].nrMig);
    EXPECT_EQ(0, demoteNum);
    EXPECT_EQ(0, promoteNum);
}

extern "C" int SwapMultiNumaVmStrategy(ProcessAttr *process, struct MigList *mlist,
                                       uint64_t nrPages[NR_LEVEL], int nrLocalNuma);
extern "C" int DemoteMultiNumaVmStrategy(ProcessAttr *process, struct MigList *mlist,
                                         RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES], uint64_t nrPages[NR_LEVEL],
                                         uint64_t demoteNum);
extern "C" int PromoteMultiNumaVmStrategy(ProcessAttr *process, struct MigList *mlist,
                                          RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES], int nrLocalNuma);
extern "C" int SeparateStrategyMultiNumaVm(ProcessAttr *process, struct MigList *mlist);
TEST_F(SeparateStrategyTest, TestSeparateStrategyMultiNumaVm)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    initializeMigList(mlist);
    process.separateParam.freqWt = 0;
    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        process.scanAttr.actcLen[i] = 0;
    }

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalculateMigInfo).stubs().will(ignoreReturnValue());
    MOCKER(SwapMultiNumaVmStrategy).stubs().will(returnValue(0));
    int ret = SeparateStrategyMultiNumaVm(&process, mlist);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    uint64_t demoteNum = 1;
    uint64_t promoteNum = 0;
    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        process.scanAttr.actcLen[i] = 1;
    }
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalculateMigInfo)
        .stubs()
        .with(any(), any(), any(), outBoundP(&demoteNum, sizeof(demoteNum)), outBoundP(&promoteNum, sizeof(promoteNum)))
        .will(ignoreReturnValue());
    MOCKER(DemoteMultiNumaVmStrategy).stubs().will(returnValue(1));
    ret = SeparateStrategyMultiNumaVm(&process, mlist);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    demoteNum = 0;
    promoteNum = 1;
    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        process.scanAttr.actcLen[i] = 1;
    }
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(InitSeparateParam).stubs().will(ignoreReturnValue());
    MOCKER(CalculateMigInfo)
        .stubs()
        .with(any(), any(), any(), outBoundP(&demoteNum, sizeof(demoteNum)), outBoundP(&promoteNum, sizeof(promoteNum)))
        .will(ignoreReturnValue());
    MOCKER(PromoteMultiNumaVmStrategy).stubs().will(returnValue(2));
    ret = SeparateStrategyMultiNumaVm(&process, mlist);
    EXPECT_EQ(2, ret);
}

extern "C" int BuildLevelActcData(ProcessAttr *process, LevelActcData *levelActcData, uint64_t nrPages,
                                  uint64_t *levelActcLen, int level);
extern "C" uint64_t CalcMultiNumaVmSwapNumByFreq(ProcessAttr *process, LevelActcData *levelActcData[NR_LEVEL],
                                                 uint64_t levelActcLen[NR_LEVEL]);
extern "C" int GroupMigPagesByNode(LevelActcData *levelActcData[NR_LEVEL], uint64_t swapNum, uint64_t nrMig[SMAP_MAX_NUMNODES],
                                   uint64_t *migAddrArray[SMAP_MAX_NUMNODES]);
extern "C" int BuildSwapMigLists(ProcessAttr *process, struct MigList *mlist,
                                 uint64_t nrMig[SMAP_MAX_NUMNODES], uint64_t *migAddrArray[SMAP_MAX_NUMNODES], int nrLocalNuma);
TEST_F(SeparateStrategyTest, TestSwapMultiNumaVmStrategy)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    uint64_t nrPages[NR_LEVEL] = { 0 };
    int nrLocalNuma = 0;

    nrPages[0] = 1;
    MOCKER(calloc).stubs().will(returnValue(static_cast<void *>(nullptr)));
    int ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(-ENOMEM, ret);

    /* SwapMultiNumaVmStrategy 的 calloc 顺序：nrMig、migAddrArray、levelActcData[nrPages>0 的层]，
     * 全部由实现内释放，mock 需按序返回独立真实缓冲区 */
    GlobalMockObject::verify();
    nrPages[0] = 1;
    uint64_t *nrMigBuf_1 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_1 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_1 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_1)))
        .then(returnValue(static_cast<void *>(migAddrBuf_1)))
        .then(returnValue(static_cast<void *>(levelActcData_1)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(1));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    uint64_t *nrMigBuf_2 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_2 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_2 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_2)))
        .then(returnValue(static_cast<void *>(migAddrBuf_2)))
        .then(returnValue(static_cast<void *>(levelActcData_2)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(0)).then(returnValue(2));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(2, ret);

    GlobalMockObject::verify();
    uint64_t *nrMigBuf_3 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_3 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_3 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_3)))
        .then(returnValue(static_cast<void *>(migAddrBuf_3)))
        .then(returnValue(static_cast<void *>(levelActcData_3)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(0)).then(returnValue(0));
    MOCKER(CalcMultiNumaVmSwapNumByFreq).stubs().will(returnValue((uint64_t)0));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    nrPages[1] = 1;
    uint64_t *nrMigBuf_4 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_4 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_4 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    LevelActcData *levelActcData_5 = (LevelActcData *)calloc(nrPages[1], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_4)))
        .then(returnValue(static_cast<void *>(migAddrBuf_4)))
        .then(returnValue(static_cast<void *>(levelActcData_4)))
        .then(returnValue(static_cast<void *>(levelActcData_5)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(0)).then(returnValue(0));
    MOCKER(CalcMultiNumaVmSwapNumByFreq).stubs().will(returnValue((uint64_t)1));
    MOCKER(GroupMigPagesByNode).stubs().will(returnValue(-ENOMEM));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(-ENOMEM, ret);

    GlobalMockObject::verify();
    uint64_t *nrMigBuf_5 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_5 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_6 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    LevelActcData *levelActcData_7 = (LevelActcData *)calloc(nrPages[1], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_5)))
        .then(returnValue(static_cast<void *>(migAddrBuf_5)))
        .then(returnValue(static_cast<void *>(levelActcData_6)))
        .then(returnValue(static_cast<void *>(levelActcData_7)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(0)).then(returnValue(0));
    MOCKER(CalcMultiNumaVmSwapNumByFreq).stubs().will(returnValue((uint64_t)1));
    MOCKER(GroupMigPagesByNode).stubs().will(returnValue(0));
    MOCKER(BuildSwapMigLists).stubs().will(returnValue(-EINVAL));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    uint64_t *nrMigBuf_6 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_6 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *levelActcData_8 = (LevelActcData *)calloc(nrPages[0], sizeof(LevelActcData));
    LevelActcData *levelActcData_9 = (LevelActcData *)calloc(nrPages[1], sizeof(LevelActcData));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_6)))
        .then(returnValue(static_cast<void *>(migAddrBuf_6)))
        .then(returnValue(static_cast<void *>(levelActcData_8)))
        .then(returnValue(static_cast<void *>(levelActcData_9)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(0)).then(returnValue(0));
    MOCKER(CalcMultiNumaVmSwapNumByFreq).stubs().will(returnValue((uint64_t)1));
    MOCKER(GroupMigPagesByNode).stubs().will(returnValue(0));
    MOCKER(BuildSwapMigLists).stubs().will(returnValue(0));
    ret = SwapMultiNumaVmStrategy(&process, mlist, nrPages, nrLocalNuma);
    EXPECT_EQ(0, ret);
}

extern "C" int BuildDemoteMultiNumaMigLists(uint64_t *migAddrArray[SMAP_MAX_NUMNODES], uint64_t nrMig[SMAP_MAX_NUMNODES],
                                            struct MigList *mlist,
                                            RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES]);
TEST_F(SeparateStrategyTest, TestDemoteMultiNumaVmStrategy)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES];
    uint64_t nrPages[NR_LEVEL] = { 0 };
    uint64_t demoteNum = 0;
    uint64_t l1ActcLen = 0;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(calloc).stubs().will(returnValue(static_cast<void *>(nullptr)));
    int ret = DemoteMultiNumaVmStrategy(&process, mlist, remoteMigInfo, nrPages, demoteNum);
    EXPECT_EQ(-ENOMEM, ret);

    /* DemoteMultiNumaVmStrategy 的 calloc 顺序：nrMig、counters、migAddrArray、l1ActcData，
     * 命中 demote 节点时还会为 migAddrArray[node] 再 calloc 一次；全部由实现内释放 */
    GlobalMockObject::verify();
    nrPages[L1] = 1;
    uint64_t *nrMigBuf = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t *countersBuf = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *l1ActcData_1 = (LevelActcData *)calloc(nrPages[L1], sizeof(LevelActcData));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf)))
        .then(returnValue(static_cast<void *>(countersBuf)))
        .then(returnValue(static_cast<void *>(migAddrBuf)))
        .then(returnValue(static_cast<void *>(l1ActcData_1)));
    MOCKER(BuildLevelActcData).stubs().will(returnValue(1));
    ret = DemoteMultiNumaVmStrategy(&process, mlist, remoteMigInfo, nrPages, demoteNum);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    demoteNum = 1;
    l1ActcLen = 1;
    uint64_t *nrMigBuf_2 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t *countersBuf_2 = (uint64_t *)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t));
    uint64_t **migAddrBuf_2 = (uint64_t **)calloc(SMAP_MAX_NUMNODES, sizeof(uint64_t *));
    LevelActcData *l1ActcData_2 = (LevelActcData *)calloc(nrPages[L1], sizeof(LevelActcData));
    ASSERT_NE(nullptr, l1ActcData_2);
    uint64_t *migAddrNode0 = (uint64_t *)calloc(1, sizeof(uint64_t));
    l1ActcData_2[0].node = 0;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(calloc).stubs()
        .will(returnValue(static_cast<void *>(nrMigBuf_2)))
        .then(returnValue(static_cast<void *>(countersBuf_2)))
        .then(returnValue(static_cast<void *>(migAddrBuf_2)))
        .then(returnValue(static_cast<void *>(l1ActcData_2)))
        .then(returnValue(static_cast<void *>(migAddrNode0)));
    MOCKER(BuildLevelActcData).stubs()
        .with(any(), any(), any(), outBoundP(&l1ActcLen, sizeof(uint64_t)))
        .will(returnValue(0));
    MOCKER(BuildDemoteMultiNumaMigLists).stubs().will(returnValue(0));
    ret = DemoteMultiNumaVmStrategy(&process, mlist, remoteMigInfo, nrPages, demoteNum);
    EXPECT_EQ(0, ret);
}

const int MULTI_NUMA_VM_OOM = 66;
TEST_F(SeparateStrategyTest, TestPromoteMultiNumaVmStrategy)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES] = {};
    int nrLocalNuma = 0;

    int ret = PromoteMultiNumaVmStrategy(&process, mlist, remoteMigInfo, nrLocalNuma);
    EXPECT_EQ(0, ret);

    remoteMigInfo[0].nrMig = 1;
    remoteMigInfo[0].dir = PROMOTE;
    ret = PromoteMultiNumaVmStrategy(&process, mlist, remoteMigInfo, nrLocalNuma);
    EXPECT_EQ(-MULTI_NUMA_VM_OOM, ret);
}

TEST_F(SeparateStrategyTest, TestBuildLevelActcData)
{
    ProcessAttr process = {};
    LevelActcData *levelActcData = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    uint64_t nrPages = 1;
    uint64_t levelActcLen = 0;
    int level = 0;

    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        process.scanAttr.actcLen[i] = 0;
    }
    int ret = BuildLevelActcData(&process, levelActcData, nrPages, &levelActcLen, level);
    EXPECT_EQ(0, ret);

    process.scanAttr.actcLen[5] = 1;
    level = L2;
    process.scanAttr.actcData[5] = (ActcData *)calloc(1, sizeof(ActcData));
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ret = BuildLevelActcData(&process, levelActcData, nrPages, &levelActcLen, level);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(levelActcData[0].node, 5);
    EXPECT_EQ(levelActcLen, 1);

    free(levelActcData);
    free(process.scanAttr.actcData[5]);
}

extern "C" int BuildMigListForDirection(MigrateDirection dir, uint64_t *migAddrArray[SMAP_MAX_NUMNODES],
                                        uint64_t nrMig[SMAP_MAX_NUMNODES], uint64_t destFreeList[SMAP_MAX_NUMNODES],
                                        struct MigList *mlist);
TEST_F(SeparateStrategyTest, TestBuildSwapMigLists)
{
    ProcessAttr process = {};
    struct MigList *mlist = TestMlist();
    uint64_t nrMig[SMAP_MAX_NUMNODES] = { 0 };
    uint64_t *migAddrArray[SMAP_MAX_NUMNODES] = { NULL };
    int nrLocalNuma = 4;

    process.numaAttr.numaNodes = 0b0000001;
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)1));
    MOCKER(BuildMigListForDirection).stubs().will(returnValue(-ENOMEM));
    int ret = BuildSwapMigLists(&process, mlist, nrMig, migAddrArray, nrLocalNuma);
    EXPECT_EQ(-ENOMEM, ret);

    GlobalMockObject::verify();
    process.numaAttr.numaNodes = 0b0100001;
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)1));
    MOCKER(BuildMigListForDirection).stubs().will(returnValue(0)).then(returnValue(-MULTI_NUMA_VM_OOM));
    ret = BuildSwapMigLists(&process, mlist, nrMig, migAddrArray, nrLocalNuma);
    EXPECT_EQ(-MULTI_NUMA_VM_OOM, ret);
}

TEST_F(SeparateStrategyTest, TestBuildMigListForDirection)
{
    MigrateDirection dir;
    uint64_t *migAddrArray[SMAP_MAX_NUMNODES] = { NULL };
    uint64_t nrMig[SMAP_MAX_NUMNODES] = { 0 };
    uint64_t destFreeList[SMAP_MAX_NUMNODES] = { 0 };
    struct MigList *mlist = TestMlist();

    dir = SWAP;
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    int ret = BuildMigListForDirection(dir, migAddrArray, nrMig, destFreeList, mlist);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    dir = PROMOTE;
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    ret = BuildMigListForDirection(dir, migAddrArray, nrMig, destFreeList, mlist);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, NM_AT(mlist, 4, 0).nr);
}

TEST_F(SeparateStrategyTest, TestBuildMigListForDirectionPromote)
{
    MigrateDirection dir;
    uint64_t *migAddrArray[SMAP_MAX_NUMNODES] = { NULL };
    uint64_t nrMig[SMAP_MAX_NUMNODES] = { 0 };
    uint64_t destFreeList[SMAP_MAX_NUMNODES] = { 0 };
    struct MigList *mlist = TestMlist();
    dir = PROMOTE;
    nrMig[4] = 1;
    migAddrArray[4] = (uint64_t *)calloc(nrMig[4], sizeof(uint64_t));
    destFreeList[0] = 1;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    int ret = BuildMigListForDirection(dir, migAddrArray, nrMig, destFreeList, mlist);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, NM_AT(mlist, 4, 0).nr);
    EXPECT_EQ(0, destFreeList[0]);
    EXPECT_NE(nullptr, NM_AT(mlist, 4, 0).addr);
    free(migAddrArray[4]);
    free(NM_AT(mlist, 4, 0).addr);
}

TEST_F(SeparateStrategyTest, TestGroupMigPagesByNode)
{
    LevelActcData *levelActcData[NR_LEVEL] = {NULL};
    uint64_t swapNum = 0;
    uint64_t nrMig[SMAP_MAX_NUMNODES] = { 0 };
    uint64_t *migAddrArray[SMAP_MAX_NUMNODES] = { NULL };

    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        nrMig[i] = 1;
    }
    int ret = GroupMigPagesByNode(levelActcData, swapNum, nrMig, migAddrArray);
    EXPECT_EQ(0, ret);

    swapNum = 1;
    levelActcData[0] = (LevelActcData *)calloc(nrMig[0], sizeof(LevelActcData));
    levelActcData[1] = (LevelActcData *)calloc(nrMig[1], sizeof(LevelActcData));
    levelActcData[0][0].node = 0;
    levelActcData[1][0].node = 5;
    ret = GroupMigPagesByNode(levelActcData, swapNum, nrMig, migAddrArray);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(levelActcData[0][0].addr, migAddrArray[0][0]);
    EXPECT_EQ(levelActcData[1][0].addr, migAddrArray[5][0]);

    for (int i = 0; i < SMAP_MAX_NUMNODES; i++) {
        if (!migAddrArray[i]) {
            free(migAddrArray[i]);
        }
    }
    free(levelActcData[0]);
    free(levelActcData[1]);
}

extern "C" uint64_t CalMultiNumaVmLowMigrateNum(uint64_t migrateNum, uint64_t freqWt, uint32_t slowThred,
                                                LevelActcData *levelActcData[NR_LEVEL]);
TEST_F(SeparateStrategyTest, TestCalcMultiNumaVmSwapNumByFreq)
{
    ProcessAttr process = {};
    LevelActcData *levelActcData[NR_LEVEL] = {NULL};
    uint64_t levelActcLen[NR_LEVEL] = {1};

    for (int i = 0; i < NR_LEVEL; i++) {
        levelActcData[i] = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    }

    process.numaAttr.numaNodes = 0b1000001;
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue((uint64_t)1));
    MOCKER(CalMultiNumaVmLowMigrateNum).stubs().will(returnValue((uint64_t)1));
    uint64_t ret = CalcMultiNumaVmSwapNumByFreq(&process, levelActcData, levelActcLen);
    EXPECT_EQ(1, ret);
}

TEST_F(SeparateStrategyTest, TestCalMultiNumaVmLowMigrateNum)
{
    uint64_t migrateNum = 1;
    uint64_t freqWt = 0;
    uint32_t slowThred = 0;
    LevelActcData *levelActcData[NR_LEVEL] = {NULL};

    for (int i = 0; i < NR_LEVEL; i++) {
        levelActcData[i] = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    }

    uint64_t ret = CalMultiNumaVmLowMigrateNum(migrateNum, freqWt, slowThred, levelActcData);
    EXPECT_EQ(0, ret);
}

extern "C" int FreqDescFunc(const void *actc1, const void *actc2);
TEST_F(SeparateStrategyTest, TestFreqDescFunc)
{
    LevelActcData *levelActcData_1 = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    ASSERT_NE(nullptr, levelActcData_1);
    LevelActcData *levelActcData_2 = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    ASSERT_NE(nullptr, levelActcData_2);

    // a1->freq < a2->freq
    levelActcData_1->freq = 1;
    levelActcData_2->freq = 2;
    int ret = FreqDescFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(1, ret);

    // a1->freq > a2->freq
    levelActcData_1->freq = 2;
    levelActcData_2->freq = 1;
    ret = FreqDescFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(-1, ret);

    // a1->freq == a2->freq
    levelActcData_1->freq = 1;
    levelActcData_2->freq = 1;
    levelActcData_1->prior = 3;
    levelActcData_2->prior = 1;
    ret = FreqDescFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(2, ret);

    free(levelActcData_1);
    free(levelActcData_2);
}

extern "C" int FreqAscFunc(const void *actc1, const void *actc2);
TEST_F(SeparateStrategyTest, TestFreqAscFunc)
{
    LevelActcData *levelActcData_1 = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    ASSERT_NE(nullptr, levelActcData_1);
    LevelActcData *levelActcData_2 = (LevelActcData *)calloc(1, sizeof(LevelActcData));
    ASSERT_NE(nullptr, levelActcData_2);

    levelActcData_1->freq = 1;
    levelActcData_2->freq = 2;
    int ret = FreqAscFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(-1, ret);

    levelActcData_1->freq = 2;
    levelActcData_2->freq = 1;
    ret = FreqAscFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(1, ret);

    levelActcData_1->freq = 1;
    levelActcData_2->freq = 1;
    levelActcData_1->prior = 3;
    levelActcData_2->prior = 1;
    ret = FreqAscFunc(levelActcData_1, levelActcData_2);
    EXPECT_EQ(-2, ret);
}

TEST_F(SeparateStrategyTest, TestBuildDemoteMultiNumaMigLists)
{
    uint64_t *migAddrArray[SMAP_MAX_NUMNODES] = { NULL };
    uint64_t nrMig[SMAP_MAX_NUMNODES] = { 0 };
    struct MigList *mlist = TestMlist();
    RemoteMigInfo remoteMigInfo[SMAP_MAX_NUMNODES] = {};
    remoteMigInfo[4].dir = DEMOTE;
    remoteMigInfo[4].nrMig = 1;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    MOCKER(GetNrFreeHugePagesByNode).stubs().will(returnValue(2));
    MOCKER(BuildMigListForDirection).stubs().will(returnValue(-ENOMEM));
    int ret = BuildDemoteMultiNumaMigLists(migAddrArray, nrMig, mlist, remoteMigInfo);
    EXPECT_EQ(-ENOMEM, ret);
}
