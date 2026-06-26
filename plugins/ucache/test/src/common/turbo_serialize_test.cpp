/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#define private public
#include <string>
#include "task_executor.h"
#include "turbo_def.h"
#include "turbo_serialize.h"

using namespace std;
using namespace turbo::serialize;
using namespace turbo::ucache;

class TurboSerializeTest : public ::testing::Test {
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

TEST_F(TurboSerializeTest, UCacheOutStreamTest)
{
    TaskRequest tReq{};
    tReq.type = TaskType::MIGRATION_STRATEGY;
    UCacheOutStream out{};
    out << tReq;
    TurboByteBuffer req = {.data = out.GetBufferPointer(), .len = out.GetSize(), .freeFunc = nullptr};
    EXPECT_NE(req.data, nullptr);
    EXPECT_NE(req.len, 0);
    EXPECT_EQ(out.mFlag, true);
    delete[] req.data;
}

TEST_F(TurboSerializeTest, UCacheInStreamTest)
{
    TurboByteBuffer resp{};
    TaskResponse taskRes{};
    taskRes.resCode = 0;
    UCacheOutStream out;
    out << taskRes;
    resp.data = out.GetBufferPointer();
    resp.len = out.GetSize();

    TaskResponse tResp{};
    UCacheInStream in(resp.data, resp.len);
    in >> tResp;
    EXPECT_EQ(tResp.resCode, 0);
    EXPECT_EQ(in.Check(), true);
    delete[] resp.data;
}

TEST_F(TurboSerializeTest, UCacheInStreamTest_CheckIn_EmptyData_ExpectFalse)
{
    TurboByteBuffer resp{};
    UCacheOutStream out{};
    resp.data = out.GetBufferPointer();
    resp.len = out.GetSize();

    TaskResponse tResp{};
    UCacheInStream in(resp.data, resp.len);
    in >> tResp;
    EXPECT_EQ(in.Check(), false);
    delete[] resp.data;
}

TEST_F(TurboSerializeTest, UCacheInStreamTest_CheckIn_WrongData_ExepectFalse)
{
    TurboByteBuffer resp{};
    UCacheOutStream out{};
    MigrationStrategyParam param{};
    param.dockerIds = {"test1"};
    out << param;
    resp.data = out.GetBufferPointer();
    resp.len = out.GetSize();

    TaskResponse tResp{};
    UCacheInStream in(resp.data, resp.len);
    in >> tResp;
    EXPECT_EQ(in.Check(), false);
    delete[] resp.data;
}
