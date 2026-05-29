/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "turbo_strategy_executor.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace std;
using namespace turbo::ucache;

class TurboStrategyExecutorTest : public ::testing::Test {
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

TEST_F(TurboStrategyExecutorTest, ExecuteMigrationStrategyTest)
{
    TurboStrategyExecutor executor;
    MigrationStrategy strategy = {
        .dstNid = 1,
        .highWatermarkPages = 100,
        .lowWatermarkPages = 50,
        .dockerIds =
            {
                "abc",
                "def",
            },
        .srcNids =
            {
                1,
                2,
            },
    };

    MOCKER_CPP(&MigrationExecutor::ExecuteNewMigrationStrategy, uint32_t(*)(MigrationStrategy *))
        .stubs()
        .will(returnValue(UCACHE_ERR));
    uint32_t ret = executor.ExecuteMigrationStrategy(strategy);
    EXPECT_EQ(ret, UCACHE_ERR);
    GlobalMockObject::verify();

    MOCKER_CPP(&MigrationExecutor::ExecuteNewMigrationStrategy, uint32_t(*)(MigrationStrategy *))
        .stubs()
        .will(returnValue(UCACHE_OK));
    ret = executor.ExecuteMigrationStrategy(strategy);
    EXPECT_EQ(ret, UCACHE_OK);
}