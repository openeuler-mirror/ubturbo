/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Description: smap_user_log ut code
 */
#include <cstring>
#include <string>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "smap_user_log.h"

using namespace std;

static int g_logCallCount = 0;
static int g_lastLogLevel = 0;
static string g_lastLogMsg;
static string g_lastLogModule;

static void MockLogCallback(int level, const char *str, const char *moduleName)
{
    g_logCallCount++;
    g_lastLogLevel = level;
    if (str) g_lastLogMsg = str;
    if (moduleName) g_lastLogModule = moduleName;
}

class SmapUserLogTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[SmapUserLogTest SetUp Begin]" << endl;
        g_logCallCount = 0;
        g_lastLogLevel = 0;
        g_lastLogMsg.clear();
        g_lastLogModule.clear();
        cout << "[SmapUserLogTest SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[SmapUserLogTest TearDown Begin]" << endl;
        SmapLoggerExit();
        GlobalMockObject::verify();
        cout << "[SmapUserLogTest TearDown End]" << endl;
    }
};

TEST_F(SmapUserLogTest, TestUpstreamSubscribeLogger)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "test message");
    EXPECT_EQ(1, g_logCallCount);
    EXPECT_EQ(1, g_lastLogLevel);
    EXPECT_TRUE(g_lastLogModule.find("SMAP") != string::npos);
}

TEST_F(SmapUserLogTest, TestUpstreamSubscribeLoggerNull)
{
    UpstreamSubscribeLogger(NULL);
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "test message");
    EXPECT_EQ(0, g_logCallCount);
}

TEST_F(SmapUserLogTest, TestSmapStartULogNullPath)
{
    int ret = SmapStartULog(NULL);
    EXPECT_EQ(-22, ret);
}

TEST_F(SmapUserLogTest, TestSmapStartULogValidPath)
{
    int ret = SmapStartULog("/tmp/test_smap_ulog");
    EXPECT_EQ(0, ret);
    SmapLoggerExit();
}

TEST_F(SmapUserLogTest, TestSMAP_UlogDebug)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_DEBUG, "testFunc", 10, "test.c", "debug message");
    EXPECT_EQ(1, g_logCallCount);
    EXPECT_EQ(0, g_lastLogLevel);
}

TEST_F(SmapUserLogTest, TestSMAP_UlogWarning)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_WARNING, "testFunc", 10, "test.c", "warning message");
    EXPECT_EQ(1, g_logCallCount);
    EXPECT_EQ(2, g_lastLogLevel);
}

TEST_F(SmapUserLogTest, TestSMAP_UlogError)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_ERROR, "testFunc", 10, "test.c", "error message");
    EXPECT_EQ(1, g_logCallCount);
    EXPECT_EQ(3, g_lastLogLevel);
}

TEST_F(SmapUserLogTest, TestSMAP_UlogInvalidLevel)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(999, "testFunc", 10, "test.c", "invalid level message");
    EXPECT_EQ(0, g_logCallCount);
}

TEST_F(SmapUserLogTest, TestSmapLoggerExitWithSubscriber)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SmapLoggerExit();
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "after exit message");
    EXPECT_EQ(0, g_logCallCount);
}

TEST_F(SmapUserLogTest, TestSMAP_UlogWithFormat)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "format test %d %s", 42, "string");
    EXPECT_EQ(1, g_logCallCount);
    EXPECT_TRUE(g_lastLogMsg.find("42") != string::npos);
    EXPECT_TRUE(g_lastLogMsg.find("string") != string::npos);
}

TEST_F(SmapUserLogTest, TestUpstreamSubscribeLoggerReplace)
{
    UpstreamSubscribeLogger(MockLogCallback);
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "first message");
    EXPECT_EQ(1, g_logCallCount);

    g_logCallCount = 0;
    UpstreamSubscribeLogger(NULL);
    SMAP_Ulog(SMAP_LOG_INFO, "testFunc", 10, "test.c", "second message");
    EXPECT_EQ(0, g_logCallCount);
}