/* Per-PID feedback aggregation and adaptive period transitions. */
#include <cstdlib>
#include <cstring>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#include "manage/manage_internal.h"
#include "strategy/strategy_config.h"

extern "C" StrategyConfig g_strategyConfig;

namespace
{
bool fixedPeriods;
bool adaptiveEnabled;
bool AdaptiveEnabled()
{
    return adaptiveEnabled;
}
bool FixedPeriods()
{
    return fixedPeriods;
}
}

class AdaptivePeriodTest : public ::testing::Test {
protected:
    ProcessAttr *attr;
    StrategyConfig savedConfig;
    void SetUp() override
    {
        attr = static_cast<ProcessAttr *>(calloc(1, sizeof(ProcessAttr)));
        attr->pid = 123;
        attr->scanType = NORMAL_SCAN;
        savedConfig = g_strategyConfig;
        g_strategyConfig.adaptiveFastScanPeriod = 200;
        g_strategyConfig.adaptiveFastMigratePeriod = 2000;
        g_strategyConfig.adaptiveSlowScanPeriod = 3000;
        g_strategyConfig.adaptiveSlowMigratePeriod = 3000;
        adaptiveEnabled = true;
        MOCKER(GetAdaptivePeriodEnableConfig).stubs().will(invoke(AdaptiveEnabled));
        fixedPeriods = false;
        MOCKER(GetFileConfSwitchConfig).stubs().will(invoke(FixedPeriods));
        InitPidAdaptivePeriod(attr);
    }
    void TearDown() override
    {
        free(attr);
        attr = nullptr;
        g_strategyConfig = savedConfig;
        GlobalMockObject::verify();
    }
    void Sample(double ratio, bool valid = true)
    {
        attr->periodFeedbackReady = true;
        attr->periodFeedback = {};
        attr->periodFeedback.localHotPages = 1;
        attr->periodFeedback.localFreq = valid ? 1 : 0;
        attr->periodFeedback.remoteHotRatio = ratio;
        ObservePidAdaptivePeriod(attr);
    }
    void Idle()
    {
        attr->periodFeedback = {};
        attr->periodFeedbackReady = true;
        ObservePidAdaptivePeriod(attr);
    }
    void Apply()
    {
        RecordPidAppliedPeriods(attr, attr->scanTime, attr->migPeriod, true);
    }
    void Slow()
    {
        Sample(0.10);
        Sample(0.15);
        ASSERT_TRUE(attr->adaptivePeriod.slow);
        EXPECT_EQ(3000u, attr->scanTime);
        EXPECT_EQ(3000u, attr->migPeriod);
        Apply();
    }
};

TEST_F(AdaptivePeriodTest, TwoWindowsSwitchAtInclusiveBoundariesAndClearHistory)
{
    EXPECT_EQ(200u, attr->scanTime);
    EXPECT_EQ(2000u, attr->migPeriod);
    Slow();
    // A new slow window establishes its own baseline; the old fast ratio is ignored.
    Sample(0.80);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    Sample(1.00);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    EXPECT_EQ(200u, attr->scanTime);
    EXPECT_EQ(2000u, attr->migPeriod);
    Apply();
    Sample(0.40);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.40);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, LargeDecreaseAndHysteresisKeepCurrentGear)
{
    Sample(0.70);
    Sample(0.20);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.20);
    ASSERT_TRUE(attr->adaptivePeriod.slow);
    Apply();
    Sample(0.40);
    Sample(0.54);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, InvalidOrMissingSampleBreaksContinuityAndCannotBeConsumedTwice)
{
    Sample(0.10);
    Sample(0.10, false);
    Sample(0.10);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    ObservePidAdaptivePeriod(attr); // no new completed window
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, FrequencyWithoutHotPagesBreaksContinuityWithoutSlowing)
{
    Sample(0.10);
    ASSERT_TRUE(attr->adaptivePeriod.previousValid);
    attr->periodFeedback = {};
    attr->periodFeedback.localFreq = 1;
    attr->periodFeedbackReady = true;
    ObservePidAdaptivePeriod(attr);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, UnappliedGearDoesNotContributeSamples)
{
    Slow();
    Sample(0.10);
    Sample(0.30);
    ASSERT_FALSE(attr->adaptivePeriod.slow);
    // Simulate failed period update: the completed window still used slow periods.
    Sample(0.30);
    Sample(0.30);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Apply();
    Sample(0.30);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.30);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, IndependentSwitchDisablesAdaptationAndReenableRestartsFast)
{
    Slow();
    adaptiveEnabled = false;
    PreparePidAdaptivePeriod(attr);
    RecordPidAppliedPeriods(attr, 200, 2000, false);
    Sample(0.10);
    Sample(0.10);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    adaptiveEnabled = true;
    Sample(0.10); // completed while automation was disabled
    PreparePidAdaptivePeriod(attr);
    EXPECT_EQ(200u, attr->scanTime);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    Apply();
    Sample(0.10);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    Sample(0.10);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, ConfiguredProfilesApplyAndHotReloadClearsHistory)
{
    fixedPeriods = true;
    g_strategyConfig.adaptiveFastScanPeriod = 100;
    g_strategyConfig.adaptiveFastMigratePeriod = 1000;
    g_strategyConfig.adaptiveSlowScanPeriod = 1000;
    g_strategyConfig.adaptiveSlowMigratePeriod = 4000;
    PreparePidAdaptivePeriod(attr);
    EXPECT_EQ(100u, attr->scanTime);
    EXPECT_EQ(1000u, attr->migPeriod);
    Apply();
    Sample(0.10);
    Sample(0.10);
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_EQ(1000u, attr->scanTime);
    EXPECT_EQ(4000u, attr->migPeriod);
    Apply();
    Sample(0.10);
    g_strategyConfig.adaptiveSlowScanPeriod = 2000;
    PreparePidAdaptivePeriod(attr);
    Apply();
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    EXPECT_EQ(2000u, attr->scanTime);
    fixedPeriods = false; // switch stays enabled; profiles now use defaults
    PreparePidAdaptivePeriod(attr);
    Apply();
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_EQ(3000u, attr->scanTime);
    EXPECT_EQ(3000u, attr->migPeriod);
}

TEST_F(AdaptivePeriodTest, TargetResetReturnsToFastAndDiscardsFeedback)
{
    Slow();
    ResetPidAdaptivePeriod(attr);
    EXPECT_EQ(200u, attr->scanTime);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    EXPECT_FALSE(attr->periodFeedbackReady);
}

TEST_F(AdaptivePeriodTest, FeedbackUsesExistingFreqAndAllRemoteNodes)
{
    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    uint32_t oldCap = g_numaCap;
    g_numaCap = 3;
    MOCKER(GetRemoteHotThreshold).stubs().will(returnValue(uint32_t(65535)));
    ActcData local[3] = {}, remote1[2] = {}, remote2[1] = {};
    local[0].freq = 10;
    local[0].prior = 9; // Window frequency is independent of prior.
    local[1].freq = 20;
    local[1].isWhiteListPage = true;
    remote1[0].freq = 30;
    remote2[0].freq = 40;
    attr->scanAttr.actcData[0] = local;
    attr->scanAttr.actcData[1] = remote1;
    attr->scanAttr.actcData[2] = remote2;
    attr->scanAttr.actcLen[0] = 3;
    attr->scanAttr.actcLen[1] = 2;
    attr->scanAttr.actcLen[2] = 1;
    // CalcActcStats also resets selectedBuckets.
    attr->scanAttr.selectedBuckets =
        static_cast<uint32_t(*)[FREQ_BUCKETS_SIZE]>(calloc(3, sizeof(*attr->scanAttr.selectedBuckets)));
    /* CalcActcStats 在 actCount 为 NULL 时直接返回，须先分配再统计 */
    attr->scanAttr.actCount = static_cast<ActCount *>(calloc(3, sizeof(ActCount)));
    ASSERT_NE(nullptr, attr->scanAttr.actCount);
    CalcActcStats(attr);
    EXPECT_EQ(2u, attr->periodFeedback.localHotPages);
    EXPECT_EQ(2u, attr->periodFeedback.remoteHotPages);
    EXPECT_DOUBLE_EQ(0.5, attr->periodFeedback.remoteHotRatio);
    EXPECT_DOUBLE_EQ(0.7, attr->periodFeedback.remoteFreqRatio);
    memset(local, 0, sizeof(local));
    memset(remote1, 0, sizeof(remote1));
    memset(remote2, 0, sizeof(remote2));
    CalcActcStats(attr);
    EXPECT_EQ(0u, attr->periodFeedback.localHotPages);
    EXPECT_EQ(0u, attr->periodFeedback.remoteHotPages);
    EXPECT_EQ(0u, attr->periodFeedback.localFreq);
    EXPECT_EQ(0u, attr->periodFeedback.remoteFreq);
    EXPECT_DOUBLE_EQ(0, attr->periodFeedback.remoteHotRatio);
    EXPECT_DOUBLE_EQ(0, attr->periodFeedback.remoteFreqRatio);
    free(attr->scanAttr.selectedBuckets);
    free(attr->scanAttr.actCount);
    attr->scanAttr.actCount = nullptr;
    g_numaCap = oldCap;
}

TEST_F(AdaptivePeriodTest, IdleWindowImmediatelySlowsFastPidAndClearsBaseline)
{
    Sample(0.40);
    Idle();
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_EQ(3000u, attr->scanTime);
    EXPECT_EQ(3000u, attr->migPeriod);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    Apply();
    Idle(); // Continued inactivity stays slow without creating an R=0 baseline.
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    Sample(0.80); // Activity resumes: first valid ratio only establishes a baseline.
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    Sample(1.00);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
}

TEST_F(AdaptivePeriodTest, FirstWindowCanBeIdleWithoutAnyHistory)
{
    Idle();
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_EQ(3000u, attr->scanTime);
    EXPECT_EQ(3000u, attr->migPeriod);
}

TEST_F(AdaptivePeriodTest, MissingWindowAndDisabledAutomationDoNotTriggerIdleSwitch)
{
    // Counters are initially zero, but no new complete window has been read.
    ObservePidAdaptivePeriod(attr);
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    EXPECT_EQ(200u, attr->scanTime);
    adaptiveEnabled = false;
    Idle();
    EXPECT_FALSE(attr->adaptivePeriod.slow);
    EXPECT_EQ(200u, attr->scanTime);
}
