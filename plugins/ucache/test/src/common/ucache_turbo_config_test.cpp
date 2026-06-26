/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#define private public
#include <string>
#include "ucache_turbo_config.h"

using namespace std;
using namespace turbo::config;
using namespace turbo::ucache;
using namespace turbo::log;

class UcacheTurboConfigTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[phase SetUp Begin]" << endl;
        cout << "[phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[phase TearDown End]" << endl;
    }
};

TEST_F(UcacheTurboConfigTest, LoadConfig)
{
    UcacheTurboConfig obj;

    uint32_t ret = obj.LoadConfig();
    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(UcacheTurboConfigTest, Initialize)
{
    UcacheTurboConfig obj;
    uint16_t moduleCode = 0x1234;
    uint32_t ret = obj.Initialize(moduleCode);
    EXPECT_EQ(ret, UCACHE_OK);
    EXPECT_EQ(obj.moduleCode, moduleCode);
}

TEST_F(UcacheTurboConfigTest, LoadMigrateConfig)
{
    UcacheTurboConfig obj;
    uint32_t ret = obj.LoadMigrateConfig();
    EXPECT_EQ(ret, UCACHE_OK);
    EXPECT_GT(obj.GetMigrateInterval(), 0);
}

constexpr int INTERVAL = 100;
TEST_F(UcacheTurboConfigTest, GetMigrateInterval)
{
    UcacheTurboConfig obj;
    obj.migrateInterval = INTERVAL;
    uint32_t interval = obj.GetMigrateInterval();
    EXPECT_EQ(interval, INTERVAL);
}

TEST_F(UcacheTurboConfigTest, LoadMigrateConfig_Invalid)
{
    UcacheTurboConfig obj;

    MOCKER(UBTurboGetUInt32).stubs().will(returnValue(UCACHE_ERR));
    uint32_t ret = obj.LoadMigrateConfig();
    EXPECT_EQ(ret, UCACHE_ERR);

    GlobalMockObject::verify();
}

TEST_F(UcacheTurboConfigTest, LoadConfig_Invalid)
{
    UcacheTurboConfig obj;

    MOCKER(UBTurboGetUInt32).stubs().will(returnValue(UCACHE_ERR));
    uint32_t ret = obj.LoadConfig();
    EXPECT_EQ(ret, UCACHE_ERR);

    GlobalMockObject::verify();

    MOCKER(UBTurboGetBool).stubs().will(returnValue(UCACHE_OK));
    MOCKER(UBTurboGetStr).stubs().will(returnValue(UCACHE_OK));
    ret = obj.LoadConfig();
    EXPECT_EQ(ret, UCACHE_OK);

    GlobalMockObject::verify();
}

TEST_F(UcacheTurboConfigTest, LoadConfig_Error)
{
    UcacheTurboConfig obj;
    obj.migrateInterval = 0;
    uint32_t ret = obj.LoadConfig();
    EXPECT_NE(ret, UCACHE_OK);
}

TEST_F(UcacheTurboConfigTest, LoadMigrateConfig_Error)
{
    UcacheTurboConfig obj;
    obj.migrateInterval = 0;
    uint32_t ret = obj.LoadMigrateConfig();
    EXPECT_NE(ret, UCACHE_OK);
}
