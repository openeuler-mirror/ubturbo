/*
 * Description: smap5.0 user migration ut code
 */

#include <cstddef>
#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "securec.h"
#include "smap_user_log.h"
#include "strategy/strategy_config.h"

using namespace std;

class PeriodConfigTest : public ::testing::Test {
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

constexpr int RETURN_ERROR = -1;

using StrategyConfigReadFunc = int32_t (*)(char *, char *);

/* StrategyConfig 定义来自 strategy/strategy_config.h（单一来源，禁止手工镜像） */

typedef struct {
    char *substr;
    bool seen;
    StrategyConfigReadFunc func;
} StrategyConfigReadElem;

extern "C" StrategyConfig g_tmpStrategyConfig;
extern "C" StrategyConfig g_strategyConfig;
extern "C" StrategyConfigReadElem g_strategyConfigRead[];

extern "C" uint32_t GetScanPeriodConfig(void);
TEST_F(PeriodConfigTest, GetScanPeriodConfigTest)
{
    g_strategyConfig.scanPeriod = 1;
    uint32_t ret = GetScanPeriodConfig();
    EXPECT_EQ(1, ret);
}

extern "C" uint32_t GetMigratePeriodConfig(void);
TEST_F(PeriodConfigTest, GetMigratePeriodConfigTest)
{
    g_strategyConfig.migratePeriod = 2;
    uint32_t ret = GetMigratePeriodConfig();
    EXPECT_EQ(2, ret);
}

extern "C" bool GetZeroFreqMigrateEnableConfig(void);
TEST_F(PeriodConfigTest, GetZeroFreqMigrateEnableConfigTest)
{
    g_strategyConfig.zeroFreqMigrateEnable = true;
    bool ret = GetZeroFreqMigrateEnableConfig();
    EXPECT_EQ(true, ret);
    g_strategyConfig.zeroFreqMigrateEnable = false;
    ret = GetZeroFreqMigrateEnableConfig();
    EXPECT_EQ(false, ret);
}

extern "C" bool GetAdaptiveRatioEnableConfig(void);
TEST_F(PeriodConfigTest, GetAdaptiveRatioEnableConfigTest)
{
    g_strategyConfig.adaptiveRatioEnable = true;
    bool ret = GetAdaptiveRatioEnableConfig();
    EXPECT_EQ(true, ret);
    g_strategyConfig.adaptiveRatioEnable = false;
    ret = GetAdaptiveRatioEnableConfig();
    EXPECT_EQ(false, ret);
}

extern "C" bool GetFileConfSwitchConfig(void);
TEST_F(PeriodConfigTest, GetFileConfSwitchConfigTest)
{
    g_strategyConfig.fileConfSwitch = true;
    bool ret = GetFileConfSwitchConfig();
    EXPECT_EQ(true, ret);
}

extern "C" uint32_t GetGroupSwapRatioConfig(void);
TEST_F(PeriodConfigTest, GetGroupSwapRatioConfigTest)
{
    g_strategyConfig.groupSwapRatio = 1;
    uint32_t ret = GetGroupSwapRatioConfig();
    EXPECT_EQ(1, ret);
}

extern "C" uint32_t GetGroupSwapMinRemoteFreqConfig(void);
TEST_F(PeriodConfigTest, GetGroupSwapMinRemoteFreqConfigTest)
{
    g_strategyConfig.groupSwapMinRemoteFreq = 5;
    uint32_t ret = GetGroupSwapMinRemoteFreqConfig();
    EXPECT_EQ(5, ret);
}

extern "C" uint32_t GetGroupSwapMinFreqGainConfig(void);
TEST_F(PeriodConfigTest, GetGroupSwapMinFreqGainConfigTest)
{
    g_strategyConfig.groupSwapMinFreqGain = 3;
    uint32_t ret = GetGroupSwapMinFreqGainConfig();
    EXPECT_EQ(3, ret);
}

extern "C" uint32_t GetGroupSwapLocalWatermarkRatioConfig(void);
TEST_F(PeriodConfigTest, GetGroupSwapLocalWatermarkRatioConfigTest)
{
    g_strategyConfig.groupSwapLocalWatermarkRatio = 95;
    uint32_t ret = GetGroupSwapLocalWatermarkRatioConfig();
    EXPECT_EQ(95, ret);
}

extern "C" uint32_t GetRemoteScanModeConfig(void);
TEST_F(PeriodConfigTest, GetRemoteScanModeConfigTest)
{
    g_strategyConfig.remoteScanMode = 2;
    EXPECT_EQ(2, GetRemoteScanModeConfig());
}

extern "C" bool GetRemoteScanModeChanged(void);
TEST_F(PeriodConfigTest, GetRemoteScanModeChangedTest)
{
    g_strategyConfig.remoteScanModeChanged = true;
    EXPECT_EQ(true, GetRemoteScanModeChanged());
    g_strategyConfig.remoteScanModeChanged = false;
}

extern "C" void SetRemoteScanModeChanged(bool val);
TEST_F(PeriodConfigTest, SetRemoteScanModeChangedTest)
{
    g_strategyConfig.remoteScanModeChanged = false;
    SetRemoteScanModeChanged(true);
    EXPECT_EQ(true, g_strategyConfig.remoteScanModeChanged);
    SetRemoteScanModeChanged(false);
}

extern "C" int32_t ConfigReadValueToInt(char *pvalue, uint32_t *resultvalue);
extern "C" int32_t ConfigRemoteScanMode(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigRemoteScanModeTest)
{
    char *substr = "smap.remote.scan.mode";

    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    EXPECT_EQ(RETURN_ERROR, ConfigRemoteScanMode(substr, (char *)"1"));

    GlobalMockObject::verify();
    /* 超出取值上限，仅鲲鹏950支持纯硬模式，取值范围 0-2 */
    EXPECT_EQ(RETURN_ERROR, ConfigRemoteScanMode(substr, (char *)"3"));

    GlobalMockObject::verify();
    EXPECT_EQ(0, ConfigRemoteScanMode(substr, (char *)"2"));
    EXPECT_EQ(2, g_tmpStrategyConfig.remoteScanMode);

    GlobalMockObject::verify();
    EXPECT_EQ(0, ConfigRemoteScanMode(substr, (char *)"0"));
    EXPECT_EQ(0, g_tmpStrategyConfig.remoteScanMode);

    g_tmpStrategyConfig.remoteScanMode = 1;
}

extern "C" uint32_t GetMigrateModeConfig(void);
TEST_F(PeriodConfigTest, GetMigrateModeConfigTest)
{
    g_strategyConfig.migrateMode = 1;
    uint32_t ret = GetMigrateModeConfig();
    EXPECT_EQ(1, ret);
    g_strategyConfig.migrateMode = 2;
    ret = GetMigrateModeConfig();
    EXPECT_EQ(2, ret);
}

extern "C" bool GetMigrateModeChanged(void);
TEST_F(PeriodConfigTest, GetMigrateModeChangedTest)
{
    g_strategyConfig.migrateModeChanged = true;
    bool ret = GetMigrateModeChanged();
    EXPECT_EQ(true, ret);
    g_strategyConfig.migrateModeChanged = false;
    ret = GetMigrateModeChanged();
    EXPECT_EQ(false, ret);
}

extern "C" void SetMigrateModeChanged(bool val);
TEST_F(PeriodConfigTest, SetMigrateModeChangedTest)
{
    g_strategyConfig.migrateModeChanged = true;
    SetMigrateModeChanged(false);
    EXPECT_EQ(false, g_strategyConfig.migrateModeChanged);
    SetMigrateModeChanged(true);
    EXPECT_EQ(true, g_strategyConfig.migrateModeChanged);
}

extern "C" uint32_t GetScanCpuMinConfig(void);
TEST_F(PeriodConfigTest, GetScanCpuMinConfigTest)
{
    g_strategyConfig.scanCpuMin = 1;
    uint32_t ret = GetScanCpuMinConfig();
    EXPECT_EQ(1, ret);
}

extern "C" uint32_t GetScanCpuMaxConfig(void);
TEST_F(PeriodConfigTest, GetScanCpuMaxConfigTest)
{
    g_strategyConfig.scanCpuMax = 3;
    uint32_t ret = GetScanCpuMaxConfig();
    EXPECT_EQ(3, ret);
}

extern "C" bool GetScanCpuChanged(void);
TEST_F(PeriodConfigTest, GetScanCpuChangedTest)
{
    g_strategyConfig.scanCpuChanged = true;
    bool ret = GetScanCpuChanged();
    EXPECT_EQ(true, ret);
    g_strategyConfig.scanCpuChanged = false;
    ret = GetScanCpuChanged();
    EXPECT_EQ(false, ret);
}

extern "C" void SetScanCpuChanged(bool val);
TEST_F(PeriodConfigTest, SetScanCpuChangedTest)
{
    g_strategyConfig.scanCpuChanged = true;
    SetScanCpuChanged(false);
    EXPECT_EQ(false, g_strategyConfig.scanCpuChanged);
    SetScanCpuChanged(true);
    EXPECT_EQ(true, g_strategyConfig.scanCpuChanged);
}

extern "C" int32_t ConfigScanCpu(char *substr, char *value);
extern "C" int32_t ConfigReadValueToInt(char *pvalue, uint32_t *resultvalue);
TEST_F(PeriodConfigTest, ConfigScanCpuTestValid)
{
    char *substr = "smap.scan.cpu";
    /* CPU 0 is available even on small CI hosts. */
    char value[] = "0-0";
    g_tmpStrategyConfig.scanCpuMin = 0;
    g_tmpStrategyConfig.scanCpuMax = 0;
    int32_t ret = ConfigScanCpu(substr, value);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.scanCpuMin);
    EXPECT_EQ(0, g_tmpStrategyConfig.scanCpuMax);
}

TEST_F(PeriodConfigTest, ConfigScanCpuTestNoDash)
{
    char *substr = "smap.scan.cpu";
    char *value = "123";
    int32_t ret = ConfigScanCpu(substr, value);
    EXPECT_EQ(-1, ret);
}

TEST_F(PeriodConfigTest, ConfigScanCpuTestMinParseFailed)
{
    char *substr = "smap.scan.cpu";
    char value[] = "abc-3";
    int32_t ret = ConfigScanCpu(substr, value);
    EXPECT_EQ(-1, ret);
}

TEST_F(PeriodConfigTest, ConfigScanCpuTestMaxParseFailed)
{
    char *substr = "smap.scan.cpu";
    char value[] = "1-abc";
    int32_t ret = ConfigScanCpu(substr, value);
    EXPECT_EQ(-1, ret);
}

TEST_F(PeriodConfigTest, ConfigScanCpuTestMinGreaterThanMax)
{
    char *substr = "smap.scan.cpu";
    char value[] = "5-3";
    int32_t ret = ConfigScanCpu(substr, value);
    EXPECT_EQ(-1, ret);
}

// UpdateMigrateModeAndScanCpu 只负责"置位"变更标志（消费标志并下发 ioctl 的是
// migration.c 的 MigrationUpdateMigrateModeAndScanCpu，其用例见 test_migration.cpp）
extern "C" void UpdateMigrateModeAndScanCpu(void);
TEST_F(PeriodConfigTest, TestUpdateMigrateModeAndScanCpuMigrateModeChanged)
{
    g_strategyConfig.migrateMode = 0;
    g_strategyConfig.scanCpuMin = 1;
    g_strategyConfig.scanCpuMax = 3;
    g_tmpStrategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.scanCpuMin = 1;
    g_tmpStrategyConfig.scanCpuMax = 3;

    UpdateMigrateModeAndScanCpu();
    EXPECT_EQ(1, g_strategyConfig.migrateMode);
    EXPECT_EQ(true, g_strategyConfig.migrateModeChanged);
    EXPECT_EQ(true, g_tmpStrategyConfig.migrateModeChanged);
}

TEST_F(PeriodConfigTest, TestUpdateMigrateModeAndScanCpuMigrateModeNoChange)
{
    g_strategyConfig.migrateMode = 1;
    g_strategyConfig.migrateModeChanged = false;
    g_strategyConfig.scanCpuMin = 1;
    g_strategyConfig.scanCpuMax = 3;
    g_tmpStrategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.migrateModeChanged = false;
    g_tmpStrategyConfig.scanCpuMin = 1;
    g_tmpStrategyConfig.scanCpuMax = 3;

    UpdateMigrateModeAndScanCpu();
    EXPECT_EQ(1, g_strategyConfig.migrateMode);
    EXPECT_EQ(false, g_strategyConfig.migrateModeChanged);
    EXPECT_EQ(false, g_tmpStrategyConfig.migrateModeChanged);
}

TEST_F(PeriodConfigTest, TestUpdateMigrateModeAndScanCpuScanCpuChanged)
{
    g_strategyConfig.migrateModeChanged = false;
    g_strategyConfig.scanCpuChanged = false;
    g_strategyConfig.scanCpuMin = 1;
    g_strategyConfig.scanCpuMax = 3;
    g_tmpStrategyConfig.scanCpuMin = 5;
    g_tmpStrategyConfig.scanCpuMax = 7;

    UpdateMigrateModeAndScanCpu();
    EXPECT_EQ(5, g_strategyConfig.scanCpuMin);
    EXPECT_EQ(7, g_strategyConfig.scanCpuMax);
    EXPECT_EQ(true, g_strategyConfig.scanCpuChanged);
    EXPECT_EQ(true, g_tmpStrategyConfig.scanCpuChanged);
}

extern "C" void UpdateRemoteScanMode(void);
TEST_F(PeriodConfigTest, TestUpdateRemoteScanModeChanged)
{
    g_strategyConfig.remoteScanMode = 1;
    g_strategyConfig.remoteScanModeChanged = false;
    g_tmpStrategyConfig.remoteScanMode = 2;
    g_tmpStrategyConfig.remoteScanModeChanged = false;

    UpdateRemoteScanMode();
    EXPECT_EQ(2, g_strategyConfig.remoteScanMode);
    EXPECT_EQ(true, g_strategyConfig.remoteScanModeChanged);
    EXPECT_EQ(true, g_tmpStrategyConfig.remoteScanModeChanged);
}

TEST_F(PeriodConfigTest, TestUpdateRemoteScanModeNoChange)
{
    g_strategyConfig.remoteScanMode = 1;
    g_strategyConfig.remoteScanModeChanged = false;
    g_tmpStrategyConfig.remoteScanMode = 1;

    UpdateRemoteScanMode();
    EXPECT_EQ(false, g_strategyConfig.remoteScanModeChanged);
}

TEST_F(PeriodConfigTest, TestUpdateMigrateModeAndScanCpuNoChange)
{
    g_strategyConfig.migrateModeChanged = false;
    g_strategyConfig.scanCpuChanged = false;
    g_strategyConfig.scanCpuMin = 1;
    g_strategyConfig.scanCpuMax = 3;
    g_tmpStrategyConfig.scanCpuMin = 1;
    g_tmpStrategyConfig.scanCpuMax = 3;

    UpdateMigrateModeAndScanCpu();
    EXPECT_EQ(false, g_strategyConfig.migrateModeChanged);
    EXPECT_EQ(false, g_strategyConfig.scanCpuChanged);
}

extern "C" int32_t ConfigReadValueToInt(char *pvalue, uint32_t *resultvalue);
extern "C" int32_t ConfigMigrateMode(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigMigrateModeTest)
{
    char *substr = "smap.migrate.mode";
    char *value = "3";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigMigrateMode(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.migrateMode = 0;
    GlobalMockObject::verify();
    ret = ConfigMigrateMode(substr, value);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    char *value1 = "1";
    ret = ConfigMigrateMode(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_tmpStrategyConfig.migrateMode);
}

extern "C" bool GetScanPeriodChanged(void);
TEST_F(PeriodConfigTest, GetScanPeriodChangedTest)
{
    g_strategyConfig.scanPeriodChanged = true;
    bool ret = GetScanPeriodChanged();
    EXPECT_EQ(true, ret);
}

extern "C" bool GetMigratePeriodChanged(void);
TEST_F(PeriodConfigTest, GetMigratePeriodChangedTest)
{
    g_strategyConfig.migratePeriodChanged = true;
    bool ret = GetMigratePeriodChanged();
    EXPECT_EQ(true, ret);
}

extern "C" void SetScanPeriodChanged(bool val);
TEST_F(PeriodConfigTest, SetScanPeriodChangedTest)
{
    g_strategyConfig.scanPeriodChanged = true;
    SetScanPeriodChanged(false);
    EXPECT_EQ(false, g_strategyConfig.scanPeriodChanged);
}

extern "C" void SetMigratePeriodChanged(bool val);
TEST_F(PeriodConfigTest, SetMigratePeriodChangedTest)
{
    g_strategyConfig.migratePeriodChanged = false;
    SetMigratePeriodChanged(true);
    EXPECT_EQ(true, g_strategyConfig.migratePeriodChanged);
}

extern "C" int32_t ConfigReadValueToInt(char *pvalue, uint32_t *resultvalue);
TEST_F(PeriodConfigTest, ConfigReadValueToIntTest)
{
    char *pvalue = "123";
    uint32_t resultvalue = 0;
    int32_t ret = ConfigReadValueToInt(pvalue, &resultvalue);
    EXPECT_EQ(123, resultvalue);
    EXPECT_EQ(0, ret);

    char *pvalue2 = "abc";
    uint32_t resultvalue2 = 0;
    ret = ConfigReadValueToInt(pvalue2, &resultvalue2);
    EXPECT_EQ(RETURN_ERROR, ret);
}

extern "C" int32_t ConfigScanPeriod(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigScanPeriodTest)
{
    char *substr = "scanPeriod";
    /* non-digit -> parse fail */
    char *value = "abc";
    int32_t ret = ConfigScanPeriod(substr, value);
    EXPECT_EQ(-1, ret);

    /* below MIN_SCAN_PERIOD -> range fail, tmp untouched */
    char *value2 = "10";
    g_tmpStrategyConfig.scanPeriod = 0;
    ret = ConfigScanPeriod(substr, value2);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.scanPeriod);
}

TEST_F(PeriodConfigTest, ConfigScanPeriodTestSuccess)
{
    int ret;
    char *substr2 = "scanPeriod";
    char *value2 = "123";
    g_tmpStrategyConfig.scanPeriod = 100UL;
    ret = ConfigScanPeriod(substr2, value2);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(123, g_tmpStrategyConfig.scanPeriod);
}

extern "C" int32_t ConfigMigratePeriod(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigMigratePeriodTest)
{
    char *substr = "migratePeriod";
    /* non-digit -> parse fail */
    char *value = "abc";
    int32_t ret = ConfigMigratePeriod(substr, value);
    EXPECT_EQ(-1, ret);

    /* below MIN_MIGRATE_PERIOD -> range fail, tmp untouched */
    g_tmpStrategyConfig.migratePeriod = 0;
    char *value2 = "123";
    ret = ConfigMigratePeriod(substr, value2);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.migratePeriod);

    /* in range -> success */
    g_tmpStrategyConfig.migratePeriod = 1000;
    char *value3 = "1234";
    ret = ConfigMigratePeriod(substr, value3);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1234, g_tmpStrategyConfig.migratePeriod);
}

extern "C" int32_t ConfigRemoteFreqPercentile(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigRemoteFreqPercentileTest)
{
    char *substr = "smap.remote.freq.percentile";
    char *value = "101";
    g_tmpStrategyConfig.remoteFreqPercentile = 0;
    int32_t ret = ConfigRemoteFreqPercentile(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.remoteFreqPercentile);

    char *value1 = "100";
    ret = ConfigRemoteFreqPercentile(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, g_tmpStrategyConfig.remoteFreqPercentile);
}

extern "C" int32_t ConfigSlowThreshold(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigSlowThresholdTest)
{
    char *substr = "smap.slow.threshold";
    constexpr uint32_t invalidSlowThreshold = MAX_MIGRATE_PERIOD / MIN_SCAN_PERIOD + 1;
    char value[16] = {};
    (void)snprintf(value, sizeof(value), "%u", invalidSlowThreshold);

    g_tmpStrategyConfig.slowThreshold = 0;
    int32_t ret = ConfigSlowThreshold(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.slowThreshold);

    char *value1 = "40";
    ret = ConfigSlowThreshold(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(40, g_tmpStrategyConfig.slowThreshold);
}

extern "C" int32_t ConfigFreqWt(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigFreqWtTest)
{
    char *substr = "smap.freq.wt";
    char *value = "65536";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigFreqWt(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.freqWt = 0;
    GlobalMockObject::verify();
    ret = ConfigFreqWt(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.freqWt);

    GlobalMockObject::verify();
    char *value1 = "40";
    ret = ConfigFreqWt(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(40, g_tmpStrategyConfig.freqWt);
}

extern "C" int32_t ConfigZeroFreqMigrateEnable(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigZeroFreqMigrateEnableTest)
{
    char *substr = "test";
    char *value1 = "true";
    g_tmpStrategyConfig.zeroFreqMigrateEnable = false;
    int32_t ret = ConfigZeroFreqMigrateEnable(substr, value1);
    EXPECT_EQ(true, g_tmpStrategyConfig.zeroFreqMigrateEnable);
    EXPECT_EQ(0, ret);

    char *value2 = "false";
    g_tmpStrategyConfig.zeroFreqMigrateEnable = true;
    ret = ConfigZeroFreqMigrateEnable(substr, value2);
    EXPECT_EQ(false, g_tmpStrategyConfig.zeroFreqMigrateEnable);
    EXPECT_EQ(0, ret);

    char *value3 = "test";
    ret = ConfigZeroFreqMigrateEnable(substr, value3);
    EXPECT_EQ(-1, ret);
}

extern "C" int32_t ConfigAdaptiveRatioEnable(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigAdaptiveRatioEnableTest)
{
    char *substr = "test";
    char *value1 = "true";
    g_tmpStrategyConfig.adaptiveRatioEnable = false;
    int32_t ret = ConfigAdaptiveRatioEnable(substr, value1);
    EXPECT_EQ(true, g_tmpStrategyConfig.adaptiveRatioEnable);
    EXPECT_EQ(0, ret);

    char *value2 = "false";
    g_tmpStrategyConfig.adaptiveRatioEnable = true;
    ret = ConfigAdaptiveRatioEnable(substr, value2);
    EXPECT_EQ(false, g_tmpStrategyConfig.adaptiveRatioEnable);
    EXPECT_EQ(0, ret);

    char *value3 = "test";
    ret = ConfigAdaptiveRatioEnable(substr, value3);
    EXPECT_EQ(-1, ret);
}

extern "C" int32_t ConfigFileConfSwitch(char *substr, char *value);
extern "C" int32_t ConfigGroupSwapRatio(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigGroupSwapRatioTest)
{
    char *substr = "smap.group.swap.ratio";
    char *value = "101";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigGroupSwapRatio(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.groupSwapRatio = 101;
    GlobalMockObject::verify();
    ret = ConfigGroupSwapRatio(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(101, g_tmpStrategyConfig.groupSwapRatio);

    GlobalMockObject::verify();
    char *value1 = "1";
    ret = ConfigGroupSwapRatio(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_tmpStrategyConfig.groupSwapRatio);
}

extern "C" int32_t ConfigGroupSwapMinRemoteFreq(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigGroupSwapMinRemoteFreqTest)
{
    char *substr = "smap.group.swap.min.remote.freq";
    char *value = "65536";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigGroupSwapMinRemoteFreq(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.groupSwapMinRemoteFreq = 65536;
    GlobalMockObject::verify();
    ret = ConfigGroupSwapMinRemoteFreq(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(65536, g_tmpStrategyConfig.groupSwapMinRemoteFreq);

    GlobalMockObject::verify();
    char *value1 = "5";
    ret = ConfigGroupSwapMinRemoteFreq(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(5, g_tmpStrategyConfig.groupSwapMinRemoteFreq);
}

extern "C" int32_t ConfigGroupSwapMinFreqGain(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigGroupSwapMinFreqGainTest)
{
    char *substr = "smap.group.swap.min.freq.gain";
    char *value = "65536";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigGroupSwapMinFreqGain(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.groupSwapMinFreqGain = 65536;
    GlobalMockObject::verify();
    ret = ConfigGroupSwapMinFreqGain(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(65536, g_tmpStrategyConfig.groupSwapMinFreqGain);

    GlobalMockObject::verify();
    char *value1 = "3";
    ret = ConfigGroupSwapMinFreqGain(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(3, g_tmpStrategyConfig.groupSwapMinFreqGain);
}

extern "C" int32_t ConfigGroupSwapLocalWatermarkRatio(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigGroupSwapLocalWatermarkRatioTest)
{
    char *substr = "smap.group.swap.local.watermark.ratio";
    char *value = "101";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigGroupSwapLocalWatermarkRatio(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.groupSwapLocalWatermarkRatio = 101;
    GlobalMockObject::verify();
    ret = ConfigGroupSwapLocalWatermarkRatio(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(101, g_tmpStrategyConfig.groupSwapLocalWatermarkRatio);

    GlobalMockObject::verify();
    char *value1 = "95";
    ret = ConfigGroupSwapLocalWatermarkRatio(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(95, g_tmpStrategyConfig.groupSwapLocalWatermarkRatio);
}

TEST_F(PeriodConfigTest, ConfigFileConfSwitchTest)
{
    char *substr = "test";
    char *value1 = "true";
    g_tmpStrategyConfig.fileConfSwitch = false;
    int32_t ret = ConfigFileConfSwitch(substr, value1);
    EXPECT_EQ(true, g_tmpStrategyConfig.fileConfSwitch);

    char *value2 = "false";
    g_tmpStrategyConfig.fileConfSwitch = true;
    ret = ConfigFileConfSwitch(substr, value2);
    EXPECT_EQ(false, g_tmpStrategyConfig.fileConfSwitch);

    char *value3 = "test";
    ret = ConfigFileConfSwitch(substr, value3);
    EXPECT_EQ(-1, ret);
}

extern "C" void ConfigReadTrim(char *str);
TEST_F(PeriodConfigTest, ConfigReadTrimTest)
{
    char str1[] = "123test\n";
    ConfigReadTrim(str1);
    EXPECT_EQ('1', str1[0]);
    EXPECT_EQ('3', str1[2]);
}

extern "C" char *ConfigReadSearchSubString(const char *buf, const char *substr);
TEST_F(PeriodConfigTest, ConfigReadSearchSubStringTest)
{
    char buf[20] = "smap = 2\n";
    const char *substr = "smap";
    char *res = nullptr;
    res = ConfigReadSearchSubString(buf, nullptr);
    EXPECT_EQ(nullptr, res);

    res = ConfigReadSearchSubString(buf, substr);
    EXPECT_EQ('2', res[0]);
}

extern "C" void StrategyConfigAnalyze(const char *str, int lineNo);
TEST_F(PeriodConfigTest, PeriodConfigAnalyzeTest)
{
    char *str = "smap";
    MOCKER(ConfigReadSearchSubString).stubs().will(returnValue(static_cast<char *>(nullptr)));
    StrategyConfigAnalyze(str, 1);
}

extern "C" int32_t ConfigReadByLine(FILE *fptemp, char *buf, int32_t readLine);
TEST_F(PeriodConfigTest, ConfigReadByLineTest)
{
    FILE *fptemp = (FILE *)malloc(sizeof(FILE));
    char *buf = (char *)malloc(1024);
    int32_t readLine = 1;

    MOCKER(fgets).stubs().will(returnValue(buf));
    int32_t ret = ConfigReadByLine(fptemp, buf, readLine);
    EXPECT_EQ(0, ret);

    free(buf);
    free(fptemp);
}

extern "C" int32_t ValidatePeriodConstraint(void);
TEST_F(PeriodConfigTest, PeriodConfigReviewTest)
{
    g_tmpStrategyConfig.scanPeriod = 1000;
    g_tmpStrategyConfig.migratePeriod = 500;
    int32_t ret = ValidatePeriodConstraint();
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    ret = ValidatePeriodConstraint();
    EXPECT_EQ(0, ret);
}

extern "C" void PeriodConifgReset(void);
TEST_F(PeriodConfigTest, PeriodConifgResetTest)
{
    for (int i = 0; i < 3; i++) {
        g_strategyConfigRead[i].seen = true;
    }
    PeriodConifgReset();
    EXPECT_EQ(false, g_strategyConfigRead[0].seen);
}

extern "C" void InitStrategyConfig(void);
TEST_F(PeriodConfigTest, InitPeriodConfigTest)
{
    g_strategyConfig.scanPeriod = 10;
    g_strategyConfig.migratePeriod = 10;
    g_strategyConfig.migrateMode = 5;
    g_strategyConfig.migrateModeChanged = true;
    g_strategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriodChanged = true;
    g_strategyConfig.migratePeriodChanged = true;

    InitStrategyConfig();
    EXPECT_EQ(200, g_strategyConfig.scanPeriod);
    EXPECT_EQ(2000, g_strategyConfig.migratePeriod);
    EXPECT_EQ(1, g_strategyConfig.groupSwapRatio);
    EXPECT_EQ(0, g_strategyConfig.groupSwapMinRemoteFreq);
    EXPECT_EQ(0, g_strategyConfig.groupSwapMinFreqGain);
    EXPECT_EQ(95, g_strategyConfig.groupSwapLocalWatermarkRatio);
    EXPECT_EQ(0, g_strategyConfig.migrateMode);
    EXPECT_EQ(false, g_strategyConfig.migrateModeChanged);
    EXPECT_EQ(true, g_strategyConfig.zeroFreqMigrateEnable);
    EXPECT_EQ(true, g_strategyConfig.adaptiveRatioEnable);
    EXPECT_EQ(false, g_strategyConfig.fileConfSwitch);
    EXPECT_EQ(false, g_strategyConfig.scanPeriodChanged);
    EXPECT_EQ(false, g_strategyConfig.migratePeriodChanged);
    EXPECT_EQ(false, g_strategyConfig.scanCpuChanged);
    EXPECT_EQ(false, g_strategyConfig.scanCpuConfigured);
}

extern "C" bool UpdateStrategyConfigChanged(void);
TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChanged)
{
    g_tmpStrategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriod = 500;
    g_strategyConfig.migratePeriod = 1000;
    g_strategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.scanPeriod = 1500;
    g_tmpStrategyConfig.migratePeriod = 2000;
    g_tmpStrategyConfig.migrateMode = 1;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(true, ret);
}

TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChangedNoChange)
{
    (void)memset_s(&g_tmpStrategyConfig, sizeof(g_tmpStrategyConfig), 0, sizeof(g_tmpStrategyConfig));
    (void)memset_s(&g_strategyConfig, sizeof(g_strategyConfig), 0, sizeof(g_strategyConfig));
    g_tmpStrategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriod = 500;
    g_strategyConfig.migratePeriod = 1000;
    g_strategyConfig.remoteHotThreshold = 10;
    g_strategyConfig.remoteFreqPercentile = 95;
    g_strategyConfig.slowThreshold = 40;
    g_strategyConfig.freqWt = 5;
    g_strategyConfig.groupSwapRatio = 1;
    g_strategyConfig.groupSwapMinRemoteFreq = 5;
    g_strategyConfig.groupSwapMinFreqGain = 3;
    g_strategyConfig.groupSwapLocalWatermarkRatio = 95;
    g_strategyConfig.migrateMode = 1;
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    g_tmpStrategyConfig.remoteHotThreshold = 10;
    g_tmpStrategyConfig.remoteFreqPercentile = 95;
    g_tmpStrategyConfig.slowThreshold = 40;
    g_tmpStrategyConfig.freqWt = 5;
    g_tmpStrategyConfig.groupSwapRatio = 1;
    g_tmpStrategyConfig.groupSwapMinRemoteFreq = 5;
    g_tmpStrategyConfig.groupSwapMinFreqGain = 3;
    g_tmpStrategyConfig.groupSwapLocalWatermarkRatio = 95;
    g_tmpStrategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.zeroFreqMigrateEnable = true;
    g_tmpStrategyConfig.adaptiveRatioEnable = true;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(false, ret);
}

TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChangedZeroFreqChanged)
{
    g_tmpStrategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriod = 500;
    g_strategyConfig.migratePeriod = 1000;
    g_strategyConfig.migrateMode = 1;
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    g_tmpStrategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.zeroFreqMigrateEnable = false;
    g_tmpStrategyConfig.adaptiveRatioEnable = true;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(true, ret);
}

TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChangedAdaptiveRatioChanged)
{
    g_tmpStrategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriod = 500;
    g_strategyConfig.migratePeriod = 1000;
    g_strategyConfig.migrateMode = 1;
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    g_tmpStrategyConfig.migrateMode = 1;
    g_tmpStrategyConfig.zeroFreqMigrateEnable = true;
    g_tmpStrategyConfig.adaptiveRatioEnable = false;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(true, ret);
}

extern "C" void StrategyConfigRead(void);
extern "C" int ConfigReadReadFile(const char *configFile);

TEST_F(PeriodConfigTest, PeriodConfigReadTestReadFileFailed)
{
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(-1));

    StrategyConfigRead();
}

TEST_F(PeriodConfigTest, PeriodConfigReadTestConfigReviewFailed)
{
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(0));
    MOCKER(ValidatePeriodConstraint).stubs().will(returnValue(-1));

    StrategyConfigRead();
}

TEST_F(PeriodConfigTest, PeriodConfigReadTestSuccess)
{
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(0));
    MOCKER(ValidatePeriodConstraint).stubs().will(returnValue(0));
    MOCKER(UpdateStrategyConfigChanged).stubs().will(returnValue(false));
    MOCKER(PeriodConifgReset).stubs().will(ignoreReturnValue());

    StrategyConfigRead();
}

extern "C" uint32_t GetUbBwThresholdConfig(void);
TEST_F(PeriodConfigTest, GetUbBwThresholdConfigTest)
{
    g_strategyConfig.ubBwThreshold = 0;
    uint32_t ret = GetUbBwThresholdConfig();
    EXPECT_EQ(0, ret);

    g_strategyConfig.ubBwThreshold = 1000;
    ret = GetUbBwThresholdConfig();
    EXPECT_EQ(1000, ret);

    g_strategyConfig.ubBwThreshold = 65535;
    ret = GetUbBwThresholdConfig();
    EXPECT_EQ(65535, ret);
}

extern "C" int32_t ConfigUbBwThreshold(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigUbBwThresholdReadFail)
{
    char *substr = "smap.ub.bw.threshold";
    char *value = "abc";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigUbBwThreshold(substr, value);
    EXPECT_EQ(-1, ret);
}

TEST_F(PeriodConfigTest, ConfigUbBwThresholdOutOfRange)
{
    char *substr = "smap.ub.bw.threshold";
    char *value = "99999";
    g_tmpStrategyConfig.ubBwThreshold = 99999;
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    int32_t ret = ConfigUbBwThreshold(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
}

TEST_F(PeriodConfigTest, ConfigUbBwThresholdSuccess)
{
    char *substr = "smap.ub.bw.threshold";
    char *value = "1000";
    g_tmpStrategyConfig.ubBwThreshold = 1000;
    int32_t ret = ConfigUbBwThreshold(substr, value);
    EXPECT_EQ(0, ret);
}

TEST_F(PeriodConfigTest, ConfigUbBwThresholdZeroAllowed)
{
    char *substr = "smap.ub.bw.threshold";
    char *value = "0";
    g_tmpStrategyConfig.ubBwThreshold = 0;
    int32_t ret = ConfigUbBwThreshold(substr, value);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, g_tmpStrategyConfig.ubBwThreshold);
}
