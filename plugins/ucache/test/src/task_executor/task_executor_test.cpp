/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "resource_collector.h"
#include "task_executor.h"
#include "turbo_ipc_server.h"
#include "turbo_serialize.h"
#include "turbo_strategy_executor.h"
#include "ucache_turbo_config.h"
#include "ucache_turbo_error.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace std;
using namespace turbo::ucache;
using namespace turbo::ipc::server;
using namespace turbo::serialize;

namespace turbo::ucache {
uint64_t GetCurrentTimestamp();
uint32_t HandleCollectResource(const TaskRequest &tReq, TaskResponse &tResp);
MigrationStrategy ConvertToMigrationStrategy(const MigrationStrategyParam &param);
uint32_t HandleMigrationStrategy(const TaskRequest &tReq, TaskResponse &tResp);
uint32_t ValidateCollectResourceParam(const std::string &payload);
bool ValidateStrategyParam(const MigrationStrategyParam &param);
uint32_t ValidateMigrationStrategyParams(const std::string &payload);
uint32_t ValidateTaskParams(const TaskRequest &tReq);
} // namespace turbo::ucache

constexpr int HIGH_WATERMARK_PAGES = 1000;
constexpr int LOW_WATERMARK_PAGES = 500;
constexpr int MB_TO_KB = 1024;
constexpr int INVALID_TASK_TYPES = 999;
constexpr int TARGET_NODE_ID = 2;
constexpr int TARGET_LEN = 10;
class TaskExecutorTest : public ::testing::Test {
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

TEST_F(TaskExecutorTest, Init_Success)
{
    MOCKER(UBTurboRegIpcService).stubs().will(returnValue(0));

    uint32_t result = TaskExecutor().Init();
    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, GetCurrentTimestamp_TEST)
{
    time_t testTime = 1234567890;
    MOCKER_CPP(std::time, time_t(*)(time_t *)).stubs().will(returnValue(testTime));

    uint64_t result = GetCurrentTimestamp();
    EXPECT_EQ(result, static_cast<uint64_t>(testTime));
}

uint32_t mockGetNumaInfo(std::unordered_map<std::string, NodeInfo> &numaInfos)
{
    NodeInfo nodeInfo0;
    nodeInfo0.memTotalBytes = 0;
    nodeInfo0.memFreeBytes = 0;
    nodeInfo0.memUsedBytes = 0;
    nodeInfo0.activeFileBytes = 0;
    nodeInfo0.inactiveFileBytes = 0;
    nodeInfo0.isRemote = 0;
    numaInfos["node0"] = nodeInfo0;

    NodeInfo nodeInfo1;
    nodeInfo1.memTotalBytes = 1;
    nodeInfo1.memFreeBytes = 1;
    nodeInfo1.memUsedBytes = 1;
    nodeInfo1.activeFileBytes = 1;
    nodeInfo1.inactiveFileBytes = 1;
    nodeInfo1.isRemote = 1;
    numaInfos["node1"] = nodeInfo1;
    return UCACHE_OK;
}

uint32_t mockGetCgroupsInfo(std::unordered_map<std::string, CgroupInfos> &cgroupInfos)
{
    CgroupIoInfo ioInfo;
    ioInfo.ioReadBytes = 0;
    ioInfo.ioWriteBytes = 0;
    ioInfo.ioReadTimes = 0;
    ioInfo.ioWriteTimes = 0;

    CgroupPageCacheInfo pageCacheInfo;
    pageCacheInfo.totalActiveFileBytes = 0;
    pageCacheInfo.totalInactiveFileBytes = 0;
    pageCacheInfo.pageCacheIn = 0;
    pageCacheInfo.pageCacheOut = 0;
    return UCACHE_OK;
}

TEST_F(TaskExecutorTest, HandleCollectResource_NumaInfo_Success)
{
    TaskRequest tReq;
    TaskResponse tResp;

    UCacheOutStream out;
    ResourceQueryType qType = ResourceQueryType::NUMA_INFO;
    out << qType;
    tReq.payload = out.Str();

    MOCKER(GetNumaInfo).stubs().will(MOCKCPP_NS::invoke(mockGetNumaInfo));
    MOCKER(GetCurrentTimestamp).stubs().will(returnValue(1234567890UL));

    uint32_t result = HandleCollectResource(tReq, tResp);

    EXPECT_EQ(result, UCACHE_OK);
    EXPECT_EQ(tResp.resCode, UCACHE_OK);
    EXPECT_FALSE(tResp.payload.empty());
    EXPECT_EQ(tResp.timestamp, 1234567890UL);
}

TEST_F(TaskExecutorTest, HandleCollectResource_CgroupInfo)
{
    TaskRequest tReq;
    TaskResponse tResp;

    ResourceQueryType qType = ResourceQueryType::CGROUP_INFO;
    UCacheOutStream out;
    out << qType;
    tReq.payload = out.Str();

    MOCKER(GetCgroupsInfo).expects(once()).will(MOCKCPP_NS::invoke(mockGetCgroupsInfo));

    uint32_t result = HandleCollectResource(tReq, tResp);

    EXPECT_EQ(result, UCACHE_OK);
    EXPECT_EQ(tResp.resCode, UCACHE_OK);
}

TEST_F(TaskExecutorTest, HandleCollectResource_MemWatermark)
{
    TaskRequest tReq;
    TaskResponse tResp;

    ResourceQueryType qType = ResourceQueryType::MEM_WATERMARK;
    UCacheOutStream out;
    out << qType;
    tReq.payload = out.Str();

    MemWatermarkInfo memWatermark;
    memWatermark.minFreeKBytes = MB_TO_KB;

    MOCKER(GetMemWatermarkInfo).expects(once()).will(returnValue(UCACHE_OK));

    uint32_t result = HandleCollectResource(tReq, tResp);

    EXPECT_EQ(result, UCACHE_OK);
    EXPECT_EQ(tResp.resCode, UCACHE_OK);
}

TEST_F(TaskExecutorTest, HandleCollectResource_InvalidResourceType)
{
    TaskRequest tReq;
    TaskResponse tResp;

    ResourceQueryType qType = static_cast<ResourceQueryType>(INVALID_TASK_TYPE);
    UCacheOutStream out;
    out << qType;
    tReq.payload = out.Str();

    uint32_t result = HandleCollectResource(tReq, tResp);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ConvertToMigrationStrategy_normal)
{
    MigrationStrategyParam param;
    param.dstNid = 1;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1", "docker2"};
    param.srcNids = {0, 1};

    MigrationStrategy strategy = ConvertToMigrationStrategy(param);

    EXPECT_EQ(strategy.dstNid, param.dstNid);
    EXPECT_EQ(strategy.highWatermarkPages, param.highWatermarkPages);
    EXPECT_EQ(strategy.lowWatermarkPages, param.lowWatermarkPages);
    EXPECT_EQ(strategy.dockerIds, param.dockerIds);
    EXPECT_EQ(strategy.srcNids, param.srcNids);
}

TEST_F(TaskExecutorTest, HandleMigrationStrategy_Success)
{
    TaskRequest tReq;
    TaskResponse tResp;

    MigrationStrategyParam param;
    param.dstNid = 1;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    tReq.payload = out.Str();

    MOCKER_CPP(TurboStrategyExecutor::ExecuteMigrationStrategy, uint32_t(*)(const MigrationStrategy &))
        .stubs()
        .will(returnValue(UCACHE_OK));

    uint32_t result = HandleMigrationStrategy(tReq, tResp);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, HandleMigrationStrategy_ExecuteFailed)
{
    TaskRequest tReq;
    TaskResponse tResp;

    MigrationStrategyParam param;
    param.dstNid = 1;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    tReq.payload = out.Str();

    MOCKER_CPP(TurboStrategyExecutor::ExecuteMigrationStrategy, uint32_t(*)(const MigrationStrategy &))
        .stubs()
        .will(returnValue(UCACHE_ERR));

    uint32_t result = HandleMigrationStrategy(tReq, tResp);

    EXPECT_NE(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ValidateCollectResourceParam_Valid)
{
    ResourceQueryType qType = ResourceQueryType::NUMA_INFO;
    UCacheOutStream out;
    out << qType;
    std::string payload = out.Str();

    MOCKER_CPP(IsValidResourceQueryType, bool (*)(ResourceQueryType)).stubs().will(returnValue(true));

    uint32_t result = ValidateCollectResourceParam(payload);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_Invalid)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0, 1};

    bool result = ValidateStrategyParam(param);

    EXPECT_TRUE(result);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_EmptySrcNids)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {};

    bool result = ValidateStrategyParam(param);

    EXPECT_FALSE(result);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_EmptyDockerIds)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {};
    param.srcNids = {0};

    bool result = ValidateStrategyParam(param);

    EXPECT_FALSE(result);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_NegativeSrcNids)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {-1};

    bool result = ValidateStrategyParam(param);

    EXPECT_FALSE(result);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_NegativeDstNids)
{
    MigrationStrategyParam param;
    param.dstNid = -1;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    bool result = ValidateStrategyParam(param);

    EXPECT_FALSE(result);
}

TEST_F(TaskExecutorTest, ValidateStrategyParam_InvalidWatermark)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = LOW_WATERMARK_PAGES;
    param.lowWatermarkPages = HIGH_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    bool result = ValidateStrategyParam(param);

    EXPECT_FALSE(result);
}

TEST_F(TaskExecutorTest, ValidateMigrationStrategyParams_Valid)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = LOW_WATERMARK_PAGES;
    param.lowWatermarkPages = HIGH_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    std::string payload = out.Str();

    MOCKER_CPP(ValidateStrategyParam, bool (*)(const MigrationStrategyParam &)).expects(once()).will(returnValue(true));

    uint32_t result = ValidateMigrationStrategyParams(payload);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ValidateMigrationStrategyParams_Invalid)
{
    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = LOW_WATERMARK_PAGES;
    param.lowWatermarkPages = HIGH_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    std::string payload = out.Str();

    MOCKER_CPP(ValidateStrategyParam, bool (*)(const MigrationStrategyParam &))
        .expects(once())
        .will(returnValue(false));

    uint32_t result = ValidateMigrationStrategyParams(payload);

    EXPECT_EQ(result, EXECUTE_INVALID_PARAM);
}

TEST_F(TaskExecutorTest, ValidateTaskParams_CollectResource_Valid)
{
    TaskRequest tReq;
    tReq.type = TaskType::COLLECT_RESOURCE;

    ResourceQueryType qType = ResourceQueryType::NUMA_INFO;
    UCacheOutStream out;
    out << qType;
    tReq.payload = out.Str();

    MOCKER_CPP(IsValidTaskType, bool (*)(TaskType)).expects(once()).will(returnValue(true));

    MOCKER_CPP(ValidateCollectResourceParam, uint32_t(*)(const std::string &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    uint32_t result = ValidateTaskParams(tReq);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ValidateTaskParams_MigrationStrategy_Valid)
{
    TaskRequest tReq;
    tReq.type = TaskType::MIGRATION_STRATEGY;

    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    tReq.payload = out.Str();

    MOCKER_CPP(IsValidTaskType, bool (*)(TaskType)).expects(once()).will(returnValue(true));

    MOCKER_CPP(ValidateMigrationStrategyParams, uint32_t(*)(const std::string &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    uint32_t result = ValidateTaskParams(tReq);

    EXPECT_EQ(result, UCACHE_OK);
}

TEST_F(TaskExecutorTest, ValidateTaskParams_InvalidTaskType)
{
    TaskRequest tReq;
    tReq.type = static_cast<TaskType>(INVALID_TASK_TYPE);

    MOCKER_CPP(IsValidTaskType, bool (*)(TaskType)).expects(once()).will(returnValue(false));

    uint32_t result = ValidateTaskParams(tReq);

    EXPECT_EQ(result, INVALID_TASK_TYPE);
}

TEST_F(TaskExecutorTest, UCacheExecuteTask_InvalidBuffer)
{
    TurboByteBuffer req;
    req.data = nullptr;
    req.len = TARGET_LEN;

    TurboByteBuffer resp;

    uint32_t result = TaskExecutor().UCacheExecuteTask(req, resp);

    EXPECT_EQ(result, INVALID_BUFFER);
}

TEST_F(TaskExecutorTest, UCacheExecuteTask_EmptyBuffer)
{
    TurboByteBuffer req;
    req.data = nullptr;
    req.len = 0;

    TurboByteBuffer resp;

    uint32_t result = TaskExecutor().UCacheExecuteTask(req, resp);

    EXPECT_EQ(result, EMPTY_BUFFER);
}

TEST_F(TaskExecutorTest, UCacheExecuteTask_CollectResource)
{
    TaskRequest tReq;
    tReq.type = TaskType::COLLECT_RESOURCE;

    ResourceQueryType qType = ResourceQueryType::NUMA_INFO;
    UCacheOutStream out;
    out << qType;
    tReq.payload = out.Str();

    TurboByteBuffer req;
    UCacheOutStream reqStream;
    reqStream << tReq;
    req.len = reqStream.GetSize();
    req.data = new uint8_t[req.len];
    memcpy_s(req.data, req.len, reqStream.GetBufferPointer(), req.len);
    TurboByteBuffer resp;

    MOCKER_CPP(IsValidTaskType, bool (*)(TaskType)).expects(once()).will(returnValue(true));

    MOCKER_CPP(ValidateCollectResourceParam, uint32_t(*)(const std::string &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    MOCKER_CPP(HandleCollectResource, uint32_t(*)(const TaskRequest &, TaskResponse &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    uint32_t result = TaskExecutor().UCacheExecuteTask(req, resp);

    EXPECT_EQ(result, UCACHE_OK);
    EXPECT_NE(resp.data, nullptr);
    EXPECT_GT(resp.len, 0);

    if (resp.data) {
        delete[] resp.data;
    }
    delete[] req.data;
}

TEST_F(TaskExecutorTest, UCacheExecuteTask_MigrationStrategy)
{
    TaskRequest tReq;
    tReq.type = TaskType::MIGRATION_STRATEGY;

    MigrationStrategyParam param;
    param.dstNid = TARGET_NODE_ID;
    param.highWatermarkPages = HIGH_WATERMARK_PAGES;
    param.lowWatermarkPages = LOW_WATERMARK_PAGES;
    param.dockerIds = {"docker1"};
    param.srcNids = {0};

    UCacheOutStream out;
    out << param;
    tReq.payload = out.Str();

    TurboByteBuffer req;
    UCacheOutStream reqStream;
    reqStream << tReq;
    req.len = reqStream.GetSize();
    req.data = new uint8_t[req.len];
    memcpy_s(req.data, req.len, reqStream.GetBufferPointer(), req.len);

    TurboByteBuffer resp;

    MOCKER_CPP(IsValidTaskType, bool (*)(TaskType)).expects(once()).will(returnValue(true));

    MOCKER_CPP(ValidateMigrationStrategyParams, uint32_t(*)(const std::string &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    MOCKER_CPP(HandleMigrationStrategy, uint32_t(*)(const TaskRequest &, TaskResponse &))
        .expects(once())
        .will(returnValue(UCACHE_OK));

    uint32_t result = TaskExecutor().UCacheExecuteTask(req, resp);

    EXPECT_EQ(result, UCACHE_OK);
    EXPECT_NE(resp.data, nullptr);
    EXPECT_GT(resp.len, 0);

    if (resp.data) {
        delete[] resp.data;
    }
    delete[] req.data;
}