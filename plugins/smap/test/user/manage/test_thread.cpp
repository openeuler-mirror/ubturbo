/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user thread ut code
 */

#include <cerrno>
#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "manage/thread.h"

using namespace std;


class ThreadTest : public ::testing::Test {
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

extern "C" void EnvMutexLock(EnvMutex *mutex);
extern "C" void EnvMutexUnlock(EnvMutex *mutex);
extern "C" void *ThreadMain(void *args);

TEST_F(ThreadTest, TestThreadMainStoped)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    EnvAtomicSet(&pm.scanMigrateStop, 1);
    pm.migPeriod = 50;

    void *ret = ThreadMain(&pm);
    EXPECT_EQ(nullptr, ret);
}

TEST_F(ThreadTest, TestInitScanMigrateThread)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    uint32_t period = 100;
    int ret;

    MOCKER(pthread_create).stubs().will(returnValue(0));
    ret = InitScanMigrateThread(&pm, period);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(period, pm.migPeriod);
    EXPECT_EQ(0, EnvAtomicRead(&pm.scanMigrateStop));
}

TEST_F(ThreadTest, TestInitScanMigrateThreadCreateFailed)
{
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));
    uint32_t period = 50;
    int ret;

    MOCKER(pthread_create).stubs().will(returnValue(EAGAIN));
    ret = InitScanMigrateThread(&pm, period);
    EXPECT_EQ(-EAGAIN, ret);
}

TEST_F(ThreadTest, TestDestroyScanMigrateThread)
{
    int ret;
    struct ProcessManager pm; memset(&pm, 0, sizeof(pm));

    // pthread_t 非零时才执行 join，模拟线程已成功创建
    pm.scanMigrateThread = (pthread_t)1;
    MOCKER(pthread_join).expects(once()).will(ignoreReturnValue());
    ret = DestroyScanMigrateThread(&pm);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, EnvAtomicRead(&pm.scanMigrateStop));
}