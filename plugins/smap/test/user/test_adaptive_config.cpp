#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#include "strategy/strategy_config.h"

extern "C" StrategyConfig g_strategyConfig;
extern "C" StrategyConfig g_tmpStrategyConfig;
extern "C" int ConfigReadReadFile(const char *path);
extern "C" int ConfigAdaptivePeriodEnable(char *key, char *value);
extern "C" int ConfigAdaptiveFastScanPeriod(char *key, char *value);
extern "C" int ConfigAdaptiveFastMigratePeriod(char *key, char *value);
extern "C" int ConfigAdaptiveSlowScanPeriod(char *key, char *value);
extern "C" int ConfigAdaptiveSlowMigratePeriod(char *key, char *value);

namespace
{
bool fileEnabled;
bool invalidProfile;
bool omitProfiles;
int ReadAdaptiveConfig(const char *)
{
    char enable[] = "smap.adaptive.period.enable", yes[] = "true";
    ConfigAdaptivePeriodEnable(enable, yes);
    g_tmpStrategyConfig.fileConfSwitch = fileEnabled;
    if (!omitProfiles) {
        char key[] = "test", fastScan[] = "100", fastMigrate[] = "1000";
        char slowScan[] = "1000", slowMigrate[] = "4000", invalid[] = "4100";
        ConfigAdaptiveFastScanPeriod(key, fastScan);
        ConfigAdaptiveFastMigratePeriod(key, fastMigrate);
        ConfigAdaptiveSlowScanPeriod(key, slowScan);
        ConfigAdaptiveSlowMigratePeriod(key, invalidProfile ? invalid : slowMigrate);
    }
    return 0;
}
}

class AdaptiveConfigTest : public ::testing::Test {
protected:
    StrategyConfig saved, tmp;
    void SetUp() override
    {
        saved = g_strategyConfig;
        tmp = g_tmpStrategyConfig;
        fileEnabled = false;
        invalidProfile = false;
        omitProfiles = false;
        // Other suites may deliberately leave invalid candidate values behind.
        g_strategyConfig.scanPeriod = 200;
        g_strategyConfig.migratePeriod = 2000;
        g_strategyConfig.adaptiveFastScanPeriod = 200;
        g_strategyConfig.adaptiveFastMigratePeriod = 2000;
        g_strategyConfig.adaptiveSlowScanPeriod = 3000;
        g_strategyConfig.adaptiveSlowMigratePeriod = 3000;
        g_strategyConfig.adaptivePeriodEnable = false;
        MOCKER(ConfigReadReadFile).stubs().will(invoke(ReadAdaptiveConfig));
    }
    void TearDown() override
    {
        g_strategyConfig = saved;
        g_tmpStrategyConfig = tmp;
        GlobalMockObject::verify();
    }
};

TEST_F(AdaptiveConfigTest, SwitchWorksWithoutFilePeriodsAndProfilesUseDefaults)
{
    StrategyConfigRead();
    EXPECT_TRUE(GetAdaptivePeriodEnableConfig());
    EXPECT_FALSE(GetFileConfSwitchConfig());
    uint32_t scan, migrate;
    GetAdaptivePeriods(false, &scan, &migrate);
    EXPECT_EQ(200u, scan);
    EXPECT_EQ(2000u, migrate);
    GetAdaptivePeriods(true, &scan, &migrate);
    EXPECT_EQ(3000u, scan);
    EXPECT_EQ(3000u, migrate);
}

TEST_F(AdaptiveConfigTest, FileProfilesAreUsedAndOmittedKeysFallBackToDefaults)
{
    fileEnabled = true;
    StrategyConfigRead();
    uint32_t scan, migrate;
    GetAdaptivePeriods(false, &scan, &migrate);
    EXPECT_EQ(100u, scan);
    EXPECT_EQ(1000u, migrate);
    GetAdaptivePeriods(true, &scan, &migrate);
    EXPECT_EQ(1000u, scan);
    EXPECT_EQ(4000u, migrate);
    omitProfiles = true;
    StrategyConfigRead();
    GetAdaptivePeriods(true, &scan, &migrate);
    EXPECT_EQ(3000u, scan);
    EXPECT_EQ(3000u, migrate);
}

TEST_F(AdaptiveConfigTest, InvalidProfileRejectsReloadAndKeepsPreviousValues)
{
    fileEnabled = true;
    StrategyConfigRead();
    invalidProfile = true;
    StrategyConfigRead();
    uint32_t scan, migrate;
    GetAdaptivePeriods(true, &scan, &migrate);
    EXPECT_EQ(1000u, scan);
    EXPECT_EQ(4000u, migrate);
}

TEST_F(AdaptiveConfigTest, InvalidSwitchAndOutOfRangePeriodsAreRejected)
{
    char key[] = "test", invalid[] = "yes", shortScan[] = "49", shortMigrate[] = "499";
    char longScan[] = "60001", longMigrate[] = "300001";
    EXPECT_EQ(-1, ConfigAdaptivePeriodEnable(key, invalid));
    EXPECT_EQ(-1, ConfigAdaptiveFastScanPeriod(key, shortScan));
    EXPECT_EQ(-1, ConfigAdaptiveFastMigratePeriod(key, shortMigrate));
    EXPECT_EQ(-1, ConfigAdaptiveSlowScanPeriod(key, longScan));
    EXPECT_EQ(-1, ConfigAdaptiveSlowMigratePeriod(key, longMigrate));
}
