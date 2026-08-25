/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Description: smap_log_core ut code
 */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <string>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "smap_log_core.h"

using namespace std;

class SmapLogCoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[SmapLogCoreTest SetUp Begin]" << endl;
        testLogFile = "/tmp/test_smap_log_core.log";
        cout << "[SmapLogCoreTest SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[SmapLogCoreTest TearDown Begin]" << endl;
        SmapLogCoreExit();
        if (access(testLogFile.c_str(), F_OK) == 0) {
            remove(testLogFile.c_str());
        }
        for (int i = 0; i < 10; i++) {
            string rotatedFile = testLogFile + "." + to_string(i);
            if (access(rotatedFile.c_str(), F_OK) == 0) {
                remove(rotatedFile.c_str());
            }
        }
        GlobalMockObject::verify();
        cout << "[SmapLogCoreTest TearDown End]" << endl;
    }

    string testLogFile;
};

TEST_F(SmapLogCoreTest, TestInitWithValidConfig)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_DEBUG;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestInitWithNullConfig)
{
    int ret = SmapLogCoreInit(NULL);
    EXPECT_EQ(-22, ret);
}

TEST_F(SmapLogCoreTest, TestInitWithEmptyPath)
{
    SmapLogConfig config;
    config.filePath[0] = '\0';
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_DEBUG;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(-22, ret);
}
extern "C" int GetTimestamp(char *buffer, size_t bufSize);
TEST_F(SmapLogCoreTest, TestWriteLog)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_DEBUG;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);
    MOCKER(GetTimestamp).stubs().will(returnValue(0));
    ret = SmapLogCoreWrite(SMAP_LOG_CORE_INFO, "test_prefix", "test_message");
    EXPECT_EQ(0, ret);

    SmapLogCoreExit();

    ifstream file(testLogFile);
    string line;
    if (file.is_open()) {
        while (getline(file, line)) {
            cout << "Log line: " << line << endl;
        }
        file.close();
    }
}

TEST_F(SmapLogCoreTest, TestWriteWithNullPrefix)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_DEBUG;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    ret = SmapLogCoreWrite(SMAP_LOG_CORE_INFO, NULL, "test_message");
    EXPECT_EQ(-22, ret);

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestWriteWithoutInit)
{
    SmapLogCoreExit();
    int ret = SmapLogCoreWrite(SMAP_LOG_CORE_INFO, "test_prefix", "test_message");
    EXPECT_EQ(-22, ret);
}

TEST_F(SmapLogCoreTest, TestDoubleInit)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_DEBUG;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestGetMinLogLevel)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_WARN;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    int level = SmapLogCoreGetMinLogLevel();
    EXPECT_EQ(SMAP_LOG_CORE_WARN, level);

    SmapLogCoreExit();
}

/* --- SmapLogCoreSetMinLogLevel tests --- */

TEST_F(SmapLogCoreTest, TestSetMinLogLevelValidTrace)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_INFO;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_TRACE);
    EXPECT_EQ(SMAP_LOG_CORE_TRACE, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestSetMinLogLevelValidDebug)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_INFO;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_DEBUG);
    EXPECT_EQ(SMAP_LOG_CORE_DEBUG, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestSetMinLogLevelInvalidNegative)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_INFO;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);
    int prevLevel = SmapLogCoreGetMinLogLevel();

    /* Invalid level < TRACE should be ignored */
    SmapLogCoreSetMinLogLevel(-1);
    EXPECT_EQ(prevLevel, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestSetMinLogLevelInvalidButt)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_INFO;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);
    int prevLevel = SmapLogCoreGetMinLogLevel();

    /* Invalid level >= BUTT should be ignored */
    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_BUTT);
    EXPECT_EQ(prevLevel, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestSetMinLogLevelRevertDebugToInfo)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_INFO;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_DEBUG);
    EXPECT_EQ(SMAP_LOG_CORE_DEBUG, SmapLogCoreGetMinLogLevel());

    /* Revert to INFO */
    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_INFO);
    EXPECT_EQ(SMAP_LOG_CORE_INFO, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
}

TEST_F(SmapLogCoreTest, TestExitResetsMinLogLevel)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_WARN;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_WARN, SmapLogCoreGetMinLogLevel());

    SmapLogCoreExit();
    /* After Exit, minLogLevel should reset to default INFO */
    EXPECT_EQ(SMAP_LOG_CORE_INFO, SmapLogCoreGetMinLogLevel());
}

TEST_F(SmapLogCoreTest, TestWriteFilteredByLevel)
{
    SmapLogConfig config;
    strncpy(config.filePath, testLogFile.c_str(), SMAP_LOG_MAX_PATH_LEN - 1);
    config.maxFileSize = 1024 * 1024;
    config.maxFileCount = 5;
    config.minLogLevel = SMAP_LOG_CORE_WARN;

    int ret = SmapLogCoreInit(&config);
    EXPECT_EQ(0, ret);

    /* INFO (2) < WARN (3) should be filtered */
    ret = SmapLogCoreWrite(SMAP_LOG_CORE_INFO, "prefix", "filtered message");
    EXPECT_EQ(0, ret);

    /* WARN (3) >= WARN (3) should pass */
    MOCKER(GetTimestamp).stubs().will(returnValue(0));
    ret = SmapLogCoreWrite(SMAP_LOG_CORE_WARN, "prefix", "pass message");
    EXPECT_EQ(0, ret);

    SmapLogCoreExit();
}
