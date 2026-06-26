/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#define private public
#include "driver_interaction.h"
#include "migration_executor.h"
#include "ucache_turbo_error.h"

#include <filesystem>
#include <fstream>

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace std;
using namespace turbo::ucache;

class MigrationExecutorTest : public ::testing::Test {
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

TEST_F(MigrationExecutorTest, ExecuteNewMigrationStrategy_alreadyStarting_true_TEST)
{
    MigrationExecutor obj;

    obj.alreadyStarting_.store(true);
    MigrationStrategy newStrategy;
    uint32_t ret = obj.MigrationExecutor::ExecuteNewMigrationStrategy(newStrategy);

    EXPECT_EQ(ret, UCACHE_ERR);
}

TEST_F(MigrationExecutorTest, ExecuteNewMigrationStrategy_IsLowWatermarkExceeded_true_TEST)
{
    MigrationExecutor obj;
    obj.alreadyStarting_.store(false);
    MigrationStrategy newStrategy;
    obj.pidsToMigrate_.clear();

    MOCKER_CPP(&MigrationExecutor::IsLowWatermarkExceeded, bool (*)(void *)).stubs().will(returnValue(true));
    uint32_t ret = obj.MigrationExecutor::ExecuteNewMigrationStrategy(newStrategy);

    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(MigrationExecutorTest, ExecuteNewMigrationStrategy_RecordPids_ERR_TEST)
{
    MigrationExecutor obj;
    obj.alreadyStarting_.store(false);
    MigrationStrategy newStrategy;

    MOCKER_CPP(&MigrationExecutor::IsLowWatermarkExceeded, bool (*)(void *)).stubs().will(returnValue(false));
    MOCKER_CPP(&MigrationExecutor::RecordPids, uint32_t(*)(void *)).stubs().will(returnValue(UCACHE_ERR));
    uint32_t ret = obj.MigrationExecutor::ExecuteNewMigrationStrategy(newStrategy);

    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(MigrationExecutorTest, ExecuteNewMigrationStrategy_OK_TEST)
{
    MigrationExecutor obj;
    obj.alreadyStarting_.store(false);
    MigrationStrategy newStrategy;
    obj.pidsToMigrate_.push_back(1);

    MOCKER_CPP(&MigrationExecutor::IsLowWatermarkExceeded, bool (*)(void *)).stubs().will(returnValue(false));
    MOCKER_CPP(&MigrationExecutor::RecordPids, uint32_t(*)(void *)).stubs().will(returnValue(UCACHE_OK));
    uint32_t ret = obj.ExecuteNewMigrationStrategy(newStrategy);

    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(MigrationExecutorTest, ExecuteNewMigrationStrategyAddtion)
{
    MigrationExecutor obj;
    obj.alreadyStarting_.store(false);
    MigrationStrategy newStrategy;
    MOCKER_CPP(&MigrationExecutor::RecordPids, uint32_t(*)(void *)).stubs().will(returnValue(UCACHE_ERR));
    uint32_t ret = obj.ExecuteNewMigrationStrategy(newStrategy);

    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(MigrationExecutorTest, StopMigrationWorker)
{
    MigrationExecutor obj;
    obj.StopMigrationWorker();
}

TEST_F(MigrationExecutorTest, MigrationWorkerTask)
{
    MigrationExecutor obj;
    obj.shouldRun_.store(true);
    obj.StartMigrationWorker();
    sleep(1);
    obj.StopMigrationWorker();
}

TEST_F(MigrationExecutorTest, MigrationWorkerTask_has_value_TEST)
{
    MigrationExecutor obj;
    obj.shouldRun_.store(true);
    MigrationStrategy newStrategy = {
        .dstNid = 0,
        .highWatermarkPages = 1,
        .lowWatermarkPages = 0,
        .dockerIds = {},
        .srcNids = {0, 1},
    };
    obj.currentStrategy_ = newStrategy;
    obj.StartMigrationWorker();
    sleep(1);
    obj.StopMigrationWorker();
}

TEST_F(MigrationExecutorTest, MigrationWorkerTask_PerformMigration_TEST)
{
    MigrationExecutor obj;
    obj.shouldRun_.store(true);
    MigrationStrategy newStrategy = {
        .dstNid = 0,
        .highWatermarkPages = 1,
        .lowWatermarkPages = 0,
        .dockerIds = {},
        .srcNids = {0, 1},
    };
    obj.currentStrategy_ = newStrategy;

    MOCKER_CPP(&MigrationExecutor::IsHighWatermarkExceeded, bool (*)(void *)).stubs().will(returnValue(false));

    obj.StartMigrationWorker();
    sleep(1);
    obj.StopMigrationWorker();
}

TEST_F(MigrationExecutorTest, IsLowWatermarkExceeded)
{
    MigrationExecutor obj;
    MigrationStrategy newStrategy;
    obj.IsLowWatermarkExceeded(newStrategy);
}

TEST_F(MigrationExecutorTest, PerformMigration)
{
    MigrationExecutor obj;
    obj.shouldRun_.store(true);
    obj.pidsToMigrate_ = {1, 2, 3, 4, 5};
    MigrationStrategy strategy;
    strategy.srcNids = {1, 2, 3, 4, 5};

    MOCKER_CPP(&DriverInteraction::MigrateFoliosInfo, uint32_t(*)(void *)).stubs().will(returnValue(UCACHE_ERR));

    obj.PerformMigration(strategy);
}

TEST_F(MigrationExecutorTest, GetDockerPid_popen_NULL_TEST)
{
    MigrationExecutor obj;
    std::string dockerId;
    FILE *pipe = static_cast<FILE *>(nullptr);

    MOCKER(popen).defaults().will(returnValue(pipe));

    uint32_t ret = obj.MigrationExecutor::GetDockerPid(dockerId);

    EXPECT_EQ(ret, -1);
}

TEST_F(MigrationExecutorTest, GetDockerPid_fgets_NULL_TEST)
{
    MigrationExecutor obj;
    std::string dockerId;
    FILE *pipe = reinterpret_cast<FILE *>(0x12345678);
    char *str = nullptr;

    MOCKER(popen).defaults().will(returnValue(pipe));
    MOCKER(fgets).defaults().will(returnValue(str));
    MOCKER(pclose).defaults().will(returnValue(0));

    uint32_t ret = obj.MigrationExecutor::GetDockerPid(dockerId);

    EXPECT_EQ(ret, -1);
}

TEST_F(MigrationExecutorTest, GetDockerPid_DockerID_Empty_TEST)
{
    MigrationExecutor obj;
    std::string dockerId;
    FILE *pipe = reinterpret_cast<FILE *>(0x12345678);
    char buffer[] = "\n";
    char *str = "1";

    MOCKER(popen).defaults().will(returnValue(pipe));
    MOCKER(fgets)
        .defaults()
        .with(outBoundP(buffer, sizeof(buffer)), mockcpp::any(), mockcpp::any())
        .will(returnValue(str));
    MOCKER(pclose).defaults().will(returnValue(0));

    uint32_t ret = obj.MigrationExecutor::GetDockerPid(dockerId);

    EXPECT_EQ(ret, -1);
}

TEST_F(MigrationExecutorTest, GetDockerPid_DockerID_Invalid_TEST)
{
    MigrationExecutor obj;
    std::string dockerId;
    FILE *pipe = reinterpret_cast<FILE *>(0x12345678);
    char buffer[] = "abc\n";
    char *str = "1";

    MOCKER(popen).defaults().will(returnValue(pipe));
    MOCKER(fgets)
        .defaults()
        .with(outBoundP(buffer, sizeof(buffer)), mockcpp::any(), mockcpp::any())
        .will(returnValue(str));
    MOCKER(pclose).defaults().will(returnValue(0));

    uint32_t ret = obj.MigrationExecutor::GetDockerPid(dockerId);

    EXPECT_EQ(ret, -1);
}

TEST_F(MigrationExecutorTest, GetDockerPid_pclose_OutofRange_TEST)
{
    MigrationExecutor obj;
    std::string dockerId;
    FILE *pipe = reinterpret_cast<FILE *>(0x12345678);
    char buffer[] = "2147483648\n";
    char *str = "1\n";
    int k = sizeof(buffer);

    MOCKER(popen).defaults().will(returnValue(pipe));
    MOCKER(fgets)
        .defaults()
        .with(outBoundP(buffer, sizeof(buffer)), mockcpp::any(), mockcpp::any())
        .will(returnValue(str));
    MOCKER(pclose).defaults().will(returnValue(0));

    uint32_t ret = obj.MigrationExecutor::GetDockerPid(dockerId);

    EXPECT_EQ(ret, -1);
}

TEST_F(MigrationExecutorTest, RecordPids)
{
    MigrationExecutor obj;
    MigrationStrategy newStrategy = {
        .dstNid = 0,
        .highWatermarkPages = 1,
        .lowWatermarkPages = 0,
        .dockerIds = {"0"},
        .srcNids = {},
    };
    MOCKER_CPP(&MigrationExecutor::GetDockerPid, pid_t(*)(void *)).stubs().will(returnValue(-1)).then(returnValue(1));
    uint32_t ret = obj.MigrationExecutor::RecordPids(newStrategy);
    EXPECT_EQ(ret, UCACHE_ERR);
    ret = obj.MigrationExecutor::RecordPids(newStrategy);
    EXPECT_EQ(ret, UCACHE_OK);
}

TEST_F(MigrationExecutorTest, StartMigrationWorker)
{
    MigrationExecutor executor;

    executor.StartMigrationWorker();
    std::string log_output = "MockTURBO_LOG_INFO: Module Name: UCACHE_MODULE_NAME, Module Code: UCACHE_MODULE_CODE";
    std::cout << "Log Output: " << log_output << std::endl;

    EXPECT_NE(log_output.find("MockTURBO_LOG_INFO"), std::string::npos);
}

TEST_F(MigrationExecutorTest, MigrationWorkerTask_ShouldStopWhenShouldRunFalse)
{
    MigrationExecutor executor;

    executor.shouldRun_.store(false);

    executor.MigrationWorkerTask();

    EXPECT_FALSE(executor.shouldRun_);
}

TEST_F(MigrationExecutorTest, MigrationWorkerTask_ShouldStopWhenShouldRunTrue)
{
    MigrationExecutor executor;

    executor.shouldRun_.store(true);

    executor.MigrationWorkerTask();

    EXPECT_TRUE(executor.shouldRun_);
}