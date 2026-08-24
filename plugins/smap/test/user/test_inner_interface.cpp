/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: smap5.0 user inner interface ut code
 */

#include <cstdlib>
#include <sys/time.h>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/manage.h"
#include "smap_inner_interface.h"
#include "smap_log_core.h"

const double TEST_DEFAULT_LOCAL_MEM_RATIO = 75.0;

using namespace std;
extern "C" EnvAtomic g_status;
extern "C" struct ProcessManager g_processManager;

class InnerInterfaceTest : public ::testing::Test {
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

TEST_F(InnerInterfaceTest, TestNullptrError)
{
    int ret;
    EnvAtomicSet(&g_status, 1);

    ret = SmapQueryVmMemRatio(nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" void SetAdaptMem(bool flag);
TEST_F(InnerInterfaceTest, TestSmapEnableAdaptMemOne)
{
    int ret;
    MOCKER(SetAdaptMem).stubs();
    ret = SmapEnableAdaptMem(0);
    EXPECT_EQ(0, ret);
}

TEST_F(InnerInterfaceTest, TestSmapEnableAdaptMemTwo)
{
    int ret;
    MOCKER(SetAdaptMem).stubs();
    ret = SmapEnableAdaptMem(1);
    EXPECT_EQ(0, ret);
}

TEST_F(InnerInterfaceTest, TestSmapEnableAdaptMemThree)
{
    int ret;
    MOCKER(SetAdaptMem).stubs();
    ret = SmapEnableAdaptMem(-1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(InnerInterfaceTest, TestSmapQueryVmMemRatio)
{
    EnvAtomicSet(&g_status, 0);
    int ret = SmapQueryVmMemRatio(nullptr);
    EXPECT_EQ(-EPERM, ret);

    EnvAtomicSet(&g_status, 1);
    struct VmRatioMsg msg;
    struct ProcessManager *manager = GetProcessManager();
    ProcessAttr current;
    current.numaAttr.numaNodes = 0b00010001;
    current.next = nullptr;
    current.type = VM_TYPE;
    current.pid = 1;
    current.strategyAttr.l3RemoteMemRatio[0][0] = TEST_DEFAULT_LOCAL_MEM_RATIO;
    memset(&manager->slots, 0, sizeof(manager->slots)); PidSlotAdd(manager, &current);
    ret = SmapQueryVmMemRatio(&msg);
    EXPECT_EQ(1, msg.vr[0].pid);
    EXPECT_EQ(HUNDRED - TEST_DEFAULT_LOCAL_MEM_RATIO, msg.vr[0].ratio);
}

/* --- SmapSetLogLevel tests --- */

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelValidDebug)
{
    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_INFO);
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_DEBUG);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_DEBUG, SmapLogCoreGetMinLogLevel());
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelValidInfo)
{
    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_DEBUG);
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_INFO);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_INFO, SmapLogCoreGetMinLogLevel());
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelValidWarn)
{
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_WARN);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_WARN, SmapLogCoreGetMinLogLevel());
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelValidError)
{
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_ERROR);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_ERROR, SmapLogCoreGetMinLogLevel());
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelInvalidNegative)
{
    int ret = SmapSetLogLevel(-1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelInvalidTooLarge)
{
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_BUTT);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(InnerInterfaceTest, TestSmapSetLogLevelRevertDebugToInfo)
{
    /* Set to DEBUG first */
    SmapLogCoreSetMinLogLevel(SMAP_LOG_CORE_DEBUG);
    EXPECT_EQ(SMAP_LOG_CORE_DEBUG, SmapLogCoreGetMinLogLevel());

    /* Revert to INFO */
    int ret = SmapSetLogLevel(SMAP_LOG_CORE_INFO);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(SMAP_LOG_CORE_INFO, SmapLogCoreGetMinLogLevel());
}
