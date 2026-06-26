/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#define private public
#include <sys/ioctl.h>
#include <fstream>
#include <vector>
#include "driver_interaction.h"
#include "ucache_turbo_config.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace std;
using namespace turbo::ucache;

class DriverInteractionTest : public ::testing::Test {
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

TEST_F(DriverInteractionTest, MigrateIoFailedTest)
{
    turbo::ucache::DriverInteraction &driver = turbo::ucache::DriverInteraction::GetInstance();

    uint32_t ret = driver.MigrateFoliosInfo(1, 1, 1);
    EXPECT_EQ(ret, UCACHE_ERR);
}

TEST_F(DriverInteractionTest, EnsureDevice)
{
    turbo::ucache::DriverInteraction &driver = turbo::ucache::DriverInteraction::GetInstance();
    MOCKER_CPP(stat, int (*)(const char *, struct stat *)).stubs().will(returnValue(0));
    uint32_t ret = driver.EnsureDevice();
    EXPECT_EQ(ret, false);

    MOCKER_CPP(open, int (*)(void *)).stubs().will(returnValue(0));
    ret = driver.EnsureDevice();
    EXPECT_EQ(ret, true);
}

TEST_F(DriverInteractionTest, MigrateFoliosInfoTest)
{
    MOCKER_CPP(&turbo::ucache::DriverInteraction::EnsureDevice, bool (*)(void *)).stubs().will(returnValue(true));
    MOCKER_CPP(ioctl, int (*)(int, unsigned long, void *)).stubs().will(returnValue(UCACHE_OK));
    turbo::ucache::DriverInteraction &driver = turbo::ucache::DriverInteraction::GetInstance();

    uint32_t ret = driver.MigrateFoliosInfo(1, 1, 1);
    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(DriverInteractionTest, MigrateFoliosInfoOpenFailTest)
{
    MOCKER_CPP(&turbo::ucache::DriverInteraction::EnsureDevice, bool (*)(void *)).stubs().will(returnValue(false));
    turbo::ucache::DriverInteraction &driver = turbo::ucache::DriverInteraction::GetInstance();

    uint32_t ret = driver.MigrateFoliosInfo(1, 1, 1);
    EXPECT_EQ(ret, UCACHE_ERR);
}