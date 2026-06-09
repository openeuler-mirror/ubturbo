/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user migration ut code
 */

#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include "smap_user_log.h"
#include "securec.h"
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

using StrategyConfigReadFunc = int32_t (*)(char*, char*);

typedef struct {
    uint32_t scanPeriod;
    uint32_t migratePeriod;
    uint32_t remoteFreqPercentile;
    uint32_t slowThreshold;
    uint64_t freqWt;
    uint32_t remoteHotThreshold;
    uint32_t groupSwapRatio;
    uint32_t groupSwapMinRemoteFreq;
    uint32_t groupSwapMinFreqGain;
    bool zeroFreqMigrateEnable;
    bool adaptiveRatioEnable;
    bool fileConfSwitch;
    bool scanPeriodChanged;
    bool migratePeriodChanged;
} StrategyConfig;

typedef struct {
    char *substr;
    StrategyConfigReadFunc func;
    uint32_t needCfg;
    uint32_t realCfg;
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
    char* pvalue = "123";
    uint32_t resultvalue = 0;
    int32_t ret = ConfigReadValueToInt(pvalue, &resultvalue);
    EXPECT_EQ(123, resultvalue);
    EXPECT_EQ(0, ret);

    char* pvalue2 = "abc";
    uint32_t resultvalue2 = 0;
    ret = ConfigReadValueToInt(pvalue2, &resultvalue2);
    EXPECT_EQ(RETURN_ERROR, ret);
}

extern "C" int32_t ConfigScanPeriod(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigScanPeriodTest)
{
    char* substr = "scanPeriod";
    char* value = "123";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigScanPeriod(substr, value);
    EXPECT_EQ(-1, ret);

    char* substr2 = "scanPeriod";
    char* value2 = "123";
    g_tmpStrategyConfig.scanPeriod = 0;
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    ret = ConfigScanPeriod(substr2, value2);
    EXPECT_EQ(RETURN_ERROR, ret);

    g_tmpStrategyConfig.scanPeriod = 0;
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    ret = ConfigScanPeriod(substr2, value2);
    EXPECT_EQ(RETURN_ERROR, ret);
}

TEST_F(PeriodConfigTest, ConfigScanPeriodTestSuccess)
{
    int ret;
    char* substr2 = "scanPeriod";
    char* value2 = "123";
    g_tmpStrategyConfig.scanPeriod = 100UL;
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    ret = ConfigScanPeriod(substr2, value2);
    EXPECT_EQ(0, ret);
}

extern "C" int32_t ConfigMigratePeriod(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigMigratePeriodTest)
{
    char* substr = "migratePeriod";
    char* value = "123";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigMigratePeriod(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.migratePeriod = 0;
    GlobalMockObject::verify();
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    ret = ConfigMigratePeriod(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);

    g_tmpStrategyConfig.migratePeriod = 1000;
    GlobalMockObject::verify();
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(0));
    ret = ConfigMigratePeriod(substr, value);
    EXPECT_EQ(0, ret);
}

extern "C" int32_t ConfigRemoteFreqPercentile(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigRemoteFreqPercentileTest)
{
    char *substr = "smap.remote.freq.percentile";
    char *value = "101";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigRemoteFreqPercentile(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.remoteFreqPercentile = 0;
    GlobalMockObject::verify();
    ret = ConfigRemoteFreqPercentile(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(101, g_tmpStrategyConfig.remoteFreqPercentile);

    GlobalMockObject::verify();
    char *value1 = "100";
    ret = ConfigRemoteFreqPercentile(substr, value1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, g_tmpStrategyConfig.remoteFreqPercentile);
}

extern "C" int32_t ConfigSlowThreshold(char *substr, char *value);
TEST_F(PeriodConfigTest, ConfigSlowThresholdTest)
{
    char *substr = "smap.slow.threshold";
    char *value = "1201";
    MOCKER(ConfigReadValueToInt).stubs().will(returnValue(-1));
    int32_t ret = ConfigSlowThreshold(substr, value);
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.slowThreshold = 0;
    GlobalMockObject::verify();
    ret = ConfigSlowThreshold(substr, value);
    EXPECT_EQ(RETURN_ERROR, ret);
    EXPECT_EQ(1201, g_tmpStrategyConfig.slowThreshold);

    GlobalMockObject::verify();
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

TEST_F(PeriodConfigTest, ConfigFileConfSwitchTest)
{
    char* substr = "test";
    char* value1 = "true";
    g_tmpStrategyConfig.fileConfSwitch = false;
    int32_t ret = ConfigFileConfSwitch(substr, value1);
    EXPECT_EQ(true, g_tmpStrategyConfig.fileConfSwitch);

    char* value2 = "false";
    g_tmpStrategyConfig.fileConfSwitch = true;
    ret = ConfigFileConfSwitch(substr, value2);
    EXPECT_EQ(false, g_tmpStrategyConfig.fileConfSwitch);

    char* value3 = "test";
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
    const char* substr = "smap";
    char* res = nullptr;
    res = ConfigReadSearchSubString(buf, nullptr);
    EXPECT_EQ(nullptr, res);

    res = ConfigReadSearchSubString(buf, substr);
    EXPECT_EQ('2', res[0]);
}

extern "C" int StrategyConfigAnalyze(const char *str);
TEST_F(PeriodConfigTest, PeriodConfigAnalyzeTest)
{
    char *str = "smap";
    MOCKER(ConfigReadSearchSubString).stubs().will(returnValue(static_cast<char *>(nullptr)));
    uint32_t ret = StrategyConfigAnalyze(str);
    EXPECT_EQ(0, ret);
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

extern "C" int ConfigReadReadFile(const char *configFile);
extern "C" int32_t StrategyConfigReview(void);
TEST_F(PeriodConfigTest, PeriodConfigReviewTest)
{
    g_strategyConfigRead[0].needCfg = 1UL;
    g_strategyConfigRead[0].realCfg = 0UL;
    int32_t ret = StrategyConfigReview();
    EXPECT_EQ(-1, ret);

    uint32_t num = 12;
    for (int i = 0; i < num; i++) {
        g_strategyConfigRead[i].needCfg = 2UL;
        g_strategyConfigRead[i].realCfg = 2UL;
    }
    g_tmpStrategyConfig.scanPeriod = 1000;
    g_tmpStrategyConfig.migratePeriod = 500;
    ret = StrategyConfigReview();
    EXPECT_EQ(-1, ret);

    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    ret = StrategyConfigReview();
    EXPECT_EQ(0, ret);
}

extern "C" void PeriodConifgReset(void);
TEST_F(PeriodConfigTest, PeriodConifgResetTest)
{
    for (int i = 0; i < 3; i++) {
        g_strategyConfigRead[i].realCfg = 1;
    }
    PeriodConifgReset();
    EXPECT_EQ(0, g_strategyConfigRead[0].realCfg);
}

extern "C" void InitStrategyConfig(void);
TEST_F(PeriodConfigTest, InitPeriodConfigTest)
{
    g_strategyConfig.scanPeriod = 10;
    g_strategyConfig.migratePeriod = 10;
    g_strategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriodChanged = true;
    g_strategyConfig.migratePeriodChanged = true;

    InitStrategyConfig();
    EXPECT_EQ(200, g_strategyConfig.scanPeriod);
    EXPECT_EQ(2000, g_strategyConfig.migratePeriod);
    EXPECT_EQ(1, g_strategyConfig.groupSwapRatio);
    EXPECT_EQ(0, g_strategyConfig.groupSwapMinRemoteFreq);
    EXPECT_EQ(0, g_strategyConfig.groupSwapMinFreqGain);
    EXPECT_EQ(true, g_strategyConfig.zeroFreqMigrateEnable);
    EXPECT_EQ(true, g_strategyConfig.adaptiveRatioEnable);
    EXPECT_EQ(false, g_strategyConfig.fileConfSwitch);
    EXPECT_EQ(false, g_strategyConfig.scanPeriodChanged);
    EXPECT_EQ(false, g_strategyConfig.migratePeriodChanged);
}

extern "C" int32_t EnsureDirectoryExists(const char *dirPath);
TEST_F(PeriodConfigTest, TestEnsureDirectoryExists)
{
    const char* dir = "/home/test";
    MOCKER(stat).stubs().will(returnValue(1));
    MOCKER(mkdir).stubs().will(returnValue(1));
    int32_t ret = EnsureDirectoryExists(dir);

    EXPECT_EQ(-1, ret);
}

extern "C" bool UpdateStrategyConfigChanged(void);
TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChanged)
{
    g_tmpStrategyConfig.fileConfSwitch = true;
    g_strategyConfig.scanPeriod = 500;
    g_strategyConfig.migratePeriod = 1000;
    g_tmpStrategyConfig.scanPeriod = 1500;
    g_tmpStrategyConfig.migratePeriod = 2000;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(true, ret);
}

TEST_F(PeriodConfigTest, TestUpdatePeriodConfigChangedNoChange)
{
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
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
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
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_tmpStrategyConfig.scanPeriod = 500;
    g_tmpStrategyConfig.migratePeriod = 1000;
    g_tmpStrategyConfig.zeroFreqMigrateEnable = true;
    g_tmpStrategyConfig.adaptiveRatioEnable = false;

    bool ret = UpdateStrategyConfigChanged();
    EXPECT_EQ(true, ret);
}

extern "C" void StrategyConfigRead(const char *configFile);
TEST_F(PeriodConfigTest, PeriodConfigReadTestReadFileFailed)
{
    const char *configFile = "/test";
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(-1));
    MOCKER(PeriodConifgReset).stubs().will(ignoreReturnValue());

    StrategyConfigRead(configFile);
}

TEST_F(PeriodConfigTest, PeriodConfigReadTestConfigReviewFailed)
{
    const char *configFile = "/test";
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(0));
    MOCKER(StrategyConfigReview).stubs().will(returnValue(-1));
    MOCKER(PeriodConifgReset).stubs().will(ignoreReturnValue());

    StrategyConfigRead(configFile);
}

TEST_F(PeriodConfigTest, PeriodConfigReadTestSuccess)
{
    const char *configFile = "/test";
    MOCKER(ConfigReadReadFile).stubs().will(returnValue(0));
    MOCKER(StrategyConfigReview).stubs().will(returnValue(0));
    MOCKER(UpdateStrategyConfigChanged).stubs().will(returnValue(false));
    MOCKER(PeriodConifgReset).stubs().will(ignoreReturnValue());

    StrategyConfigRead(configFile);
}
