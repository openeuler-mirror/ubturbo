/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap5.0 user manage ut code
 * Create: 2024-09-18
 */

#include <cstdlib>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include "manage/access_ioctl.h"
#include "manage/smap_config.h"
#include "manage/manage.h"
#include "strategy/period_config.h"
#include "securec.h"

using namespace std;

static cpu_set_t g_fake_cpu_mask;

static int fake_sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
    (void)pid;
    (void)cpusetsize;
    *mask = g_fake_cpu_mask;
    return 0;
}

class ManageTest : public ::testing::Test {
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

extern "C" struct ProcessManager g_processManager;
extern "C" uint32_t g_pageSizeHuge;
extern "C" RunMode g_runMode;
extern "C" void RemoteNumaInfoInit();
TEST_F(ManageTest, TestRemoteNumaInfoInit)
{
    g_processManager.remoteNumaInfo.usedInfo[0].size = 10;
    RemoteNumaInfoInit();
    EXPECT_EQ(0,  g_processManager.remoteNumaInfo.usedInfo[0].size);
}

extern "C" errno_t memset_s(void *dest, size_t destMax, int c, size_t count);
extern "C" int ProcessManagerInit(uint32_t pageType);
extern "C" int EnvMutexInit(EnvMutex *mutex);
TEST_F(ManageTest, TestProcessManagerInit)
{
    uint32_t period;
    int ret = 0;
    uint32_t pageType = PAGETYPE_NORMAL;
    MOCKER(memset_s).stubs().will(returnValue(1));
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestProcessManagerInitTwo)
{
    uint32_t period;
    int ret = 0;
    uint32_t pageType = PAGETYPE_HUGE;
    MOCKER(EnvMutexInit).stubs().will(returnValue(0));
    g_processManager.threadCtx[0] = (void*)&period;
    g_processManager.processes = (ProcessAttr*)&period;
    ret = ProcessManagerInit(pageType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(nullptr, g_processManager.processes);
}

TEST_F(ManageTest, TestLoadMangerNrProcessNum)
{
    g_processManager.nr[PROCESS_TYPE] = 1;
    int ret = LoadMangerNrProcessNum();
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestLoadMangerNrVmNum)
{
    g_processManager.nr[VM_TYPE] = 1;
    int ret = LoadMangerNrVmNum();
    EXPECT_EQ(1, ret);
}

extern "C" int sscanf_s(const char *buffer, const char *format, ...);
extern "C" int snprintf_s(char *strDest, unsigned long destMax, unsigned long count, const char *format, ...);
extern "C" int access(const char *__name, int __type);
TEST_F(ManageTest, TestPidIsValid)
{
    bool ret;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(access).stubs().will(returnValue(0));
    ret = PidIsValid(1);
    EXPECT_EQ(ret, true);
    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    ret = PidIsValid(1);
    EXPECT_EQ(ret, false);
}

extern "C" int snprintf_s(char *strDest, unsigned long destMax, unsigned long count, const char *format, ...);
extern "C" FILE *fopen(const char *__restrict __filename, const char *__restrict __modes);
extern "C" char *fgets(char *__restrict __s, int __n, FILE *__restrict __stream);
extern "C" int fclose(FILE *__stream);
extern "C" int strncmp(const char *cs, const char *ct, size_t count);
extern "C" int IsQemuTask(pid_t pid);
TEST_F(ManageTest, TestIsQemuTaskPath)
{
    int ret;
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1));
    ret = IsQemuTask(1);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    ret = IsQemuTask(1);
    EXPECT_EQ(-1, ret);
}

TEST_F(ManageTest, TestIsQemuTaskFile)
{
    int ret;

    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    static FILE fake_file;
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    MOCKER(fgets).stubs().will(returnValue(static_cast<char *>(nullptr)));
    MOCKER(fclose).stubs().will(returnValue(0));
    ret = IsQemuTask(1);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(0));
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    char buf[] = "1";
    MOCKER(fgets).stubs().will(returnValue(&buf[0]));
    MOCKER(fclose).stubs().will(returnValue(0));
    ret = IsQemuTask(1);
    EXPECT_EQ(0, ret);
}

extern "C" void LinkedListRemove(ProcessAttr **remove, ProcessAttr **head);
extern "C" void FreeProceccesAttr(ProcessAttr *attr);
TEST_F(ManageTest, TestLinkedListRemoveInputNull)
{
    ProcessAttr *remove = nullptr;
    ProcessAttr *head = nullptr;
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, remove);
    EXPECT_EQ(nullptr, head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, remove);
    free(head);

    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head = nullptr;
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, head);
    free(remove);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = nullptr;
    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    LinkedListRemove(&remove, &head);
    EXPECT_EQ(nullptr, head->next);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = nullptr;
    remove = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    LinkedListRemove(&head, &head);
    EXPECT_EQ(nullptr, head);
}

TEST_F(ManageTest, TestLinkedListRemove)
{
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    ProcessAttr *head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    ProcessAttr *middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    ProcessAttr *tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    LinkedListRemove(&middle, &head);
    EXPECT_EQ(middle, nullptr);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    LinkedListRemove(&tail, &head);
    EXPECT_EQ(tail, nullptr);
    EXPECT_EQ(middle->next, nullptr);
    free(head);

    head = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    middle = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    tail = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    head->next = middle;
    middle->next = tail;
    tail->next = nullptr;
    ProcessAttr *attr = head;
    LinkedListRemove(&attr, &head);
    EXPECT_EQ(head, middle);
    free(head);
}

extern "C" int CheckPid(pid_t pid);
TEST_F(ManageTest, TestCheckPid)
{
    int ret;
    pid_t pid;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(PidIsValid).stubs().will(returnValue(false));
    ret = CheckPid(pid);
    EXPECT_EQ(-ESRCH, ret);

    GlobalMockObject::verify();
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(PidIsValid).stubs().will(returnValue(true));
    MOCKER(IsQemuTask).stubs().will(returnValue((int)PROCESS_TYPE));
    ret = CheckPid(pid);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" ProcessAttr *GetProcessAttr(pid_t pid);
TEST_F(ManageTest, TestGetProcessAttr)
{
    ProcessAttr *ret, current;
    g_processManager.processes = &current;
    current.pid = 123;
    ret = GetProcessAttr(123);
    EXPECT_EQ(123, ret->pid);
}

extern "C" ProcessAttr *GetProcessAttrLocked(pid_t pid);
TEST_F(ManageTest, TestGetProcessAttrLocked)
{
    ProcessAttr pid1 = { .pid = 1 };
    ProcessAttr pid2 = { .pid = 2, .next = &pid1 };
    g_processManager.processes = &pid2;
    ProcessAttr *ret = GetProcessAttrLocked(pid1.pid);
    EXPECT_EQ(pid1.pid, ret->pid);
    g_processManager.processes = nullptr;
}

extern "C" int ParseMmapType(int domainId, MmapType *mmapType);
TEST_F(ManageTest, TestParseMmapTypeFailed)
{
    int domainId = 0;
    MmapType mmapType;
    int ret;

    MOCKER(GetXMLByDomainId).stubs().will(returnValue((char *)nullptr));
    ret = ParseMmapType(domainId, &mmapType);

    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestParseMmapTypePrivate)
{
    int domainId = 0;
    MmapType mmapType = (MmapType)0;
    char rawXml[] = "memAccess='private'";
    int length = strlen(rawXml) + 1;
    char *xml = (char *)malloc(length);
    int ret;

    memcpy_s(xml, length, rawXml, length);

    MOCKER(GetXMLByDomainId).stubs().will(returnValue(xml));

    ret = ParseMmapType(domainId, &mmapType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0, mmapType);
}

TEST_F(ManageTest, TestParseMmapTypeShared)
{
    int domainId = 0;
    MmapType mmapType;
    char rawXml[] = "memAccess='shared'";
    int length = strlen(rawXml) + 1;
    char *xml = (char *)malloc(length);
    int ret;

    memcpy_s(xml, length, rawXml, length);

    MOCKER(GetXMLByDomainId).stubs().will(returnValue(xml));

    ret = ParseMmapType(domainId, &mmapType);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, mmapType);
}

extern "C" int VMPreprocess(pid_t pid, ProcessAttr *attr);
TEST_F(ManageTest, TestVMProcessNormal)
{
    pid_t pid;
    ProcessAttr attr;
    int ret;

    MOCKER(GetPidType).stubs().will(returnValue(0));

    ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestVMProcessReadDomainIdFailed)
{
    pid_t pid;
    ProcessAttr attr;
    int ret;

    g_processManager.tracking.pageSize = PAGESIZE_2M;
    MOCKER(ReadDomainIdByPid).stubs().will(returnValue(-1));

    ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestVMProcessParseMmapTypeFailed)
{
    pid_t pid;
    ProcessAttr attr;
    int ret;

    MOCKER(IsQemuTask).stubs().will(returnValue(1));
    MOCKER(ReadDomainIdByPid).stubs().will(returnValue(0));
    MOCKER(ParseMmapType).stubs().will(returnValue(-EINVAL));

    ret = VMPreprocess(pid, &attr);
    EXPECT_EQ(0, ret);
}

extern "C" void SetProcessConfig(ProcessAttr *attr, ProcessParam *param);
TEST_F(ManageTest, TestSetProcessConfig)
{
    g_processManager.nrLocalNuma = 4;
    ProcessAttr attr;
    ProcessParam param = {
        .pid = 1,
        .count = 1,
    };
    param.numaParam[0].nid = 4;
    param.numaParam[0].ratio = 50;
    attr.numaAttr.numaNodes = 1;
    SetProcessConfig(&attr, &param);
    EXPECT_EQ(attr.pid, 1);
    EXPECT_EQ(attr.strategyAttr.initRemoteMemRatio[0][0], 50);
    EXPECT_EQ(attr.numaAttr.numaNodes, 17);
}

extern "C" FILE* OpenNumaMaps(pid_t pid);
TEST_F(ManageTest, TestOpenNumaMaps)
{
    int pid = 1;
    MOCKER(fopen).stubs().will(returnValue(reinterpret_cast<FILE*>(0x1234)));

    FILE* ret = OpenNumaMaps(pid);
    EXPECT_NE(ret, nullptr);
}

extern "C" int GetPidNumaPagesFromNumaMaps(pid_t pid, uint64_t numaPages[MAX_NODES], bool onlyHuge);
TEST_F(ManageTest, TestGetPidNumaPagesFromNumaMapsOpenFailure)
{
    uint64_t numaPages[MAX_NODES] = { 0 };
    MOCKER(OpenNumaMaps).expects(once()).will(returnValue(static_cast<FILE *>(nullptr)));
    int ret = GetPidNumaPagesFromNumaMaps(1234, numaPages, false);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" bool IsNumaMapLineHuge(char *line);
TEST_F(ManageTest, TestIsNumaMapLineHuge)
{
    char line[] = "abcdesgsasdfskernelpagesize_kB=2048";
    bool ret = IsNumaMapLineHuge(line);
    EXPECT_EQ(ret, true);
}

extern "C" void SetLocalByNumaMaps(char *line, uint32_t *nodeBitmap, bool hugeFlag);
#define BIT(i) (1U << (i))
TEST_F(ManageTest, TestSetLocalByNumaMaps)
{
    char line[] = "00100000 N0=1 N2=3 kernelpagesize_kB=2048";
    uint32_t nodeBitmap = 0;
    SetLocalByNumaMaps(line, &nodeBitmap, true);
    EXPECT_EQ(nodeBitmap, BIT(0) | BIT(2));
}

extern "C" int SetProcessLocalNuma(pid_t pid, uint32_t *nodeBitmap, bool hugeFlag);
extern "C" int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
extern "C" int GetNodeFromCpu(int cpu);
TEST_F(ManageTest, TestSetProcessLocalNuma)
{
    int pid = 1;
    uint32_t nodeBitmap = 0;
    CPU_ZERO(&g_fake_cpu_mask);
    CPU_SET(1, &g_fake_cpu_mask);
    CPU_SET(2, &g_fake_cpu_mask);
    MOCKER(sched_getaffinity).stubs().will(invoke(fake_sched_getaffinity));
    MOCKER(GetNodeFromCpu).stubs().will(returnValue(1)).then(returnValue(2));
    int ret = SetProcessLocalNuma(pid, &nodeBitmap, true);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(nodeBitmap, BIT(1) | BIT(2));
}

extern "C" int ProcessAddManage(ProcessParam *param, uint32_t *nodeBitmap);
extern "C" int IsQemuTask(pid_t pid);
TEST_F(ManageTest, TestProcessAddManageResetPidConfig)
{
    int ret;
    pid_t pid = 123;
    uint32_t localNodeBitmap = 1;
    ProcessAttr mockProcess;
    mockProcess.pid = pid;
    mockProcess.numaAttr.numaNodes = 31;

    g_processManager.processes = &mockProcess;
    ProcessParam param = {
        .pid = pid,
        .scanTime = 50,
        .duration = 1,
        .count = 1,
    };
    param.numaParam[0].nid = 5;
    param.numaParam[0].ratio = 50;
    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&mockProcess));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);

    mockProcess.numaAttr.numaNodes = 47;
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(param.scanTime, g_processManager.processes->scanTime);
    EXPECT_EQ(param.duration, g_processManager.processes->duration);
    EXPECT_EQ(50., g_processManager.processes->initLocalMemRatio);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestProcessAddManageNewPid)
{
    int ret;
    ProcessParam param = {
        .pid = 123,
        .scanTime = 50,
        .duration = 1,
        .scanType = NORMAL_SCAN,
        .count = 1,
    };
    param.numaParam[0].nid = 4;
    param.numaParam[0].ratio = 50;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(0));
    MOCKER(GetPidNrPages).stubs().will(returnValue(0x100));
    MOCKER(EnvMutexLock).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    MOCKER(EnvMutexUnlock).stubs().will(ignoreReturnValue());
    MOCKER(sched_getaffinity).stubs().will(returnValue(0));
    MOCKER(GetNodeFromCpu).stubs().will(returnValue(4));

    ret = ProcessAddManage(&param, nullptr);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD, g_processManager.processes->scanTime);  // 首次扫描使用DEFAULT_SCAN_PERIOD
    EXPECT_EQ(param.duration, g_processManager.processes->duration);
    EXPECT_EQ(50, g_processManager.processes->initLocalMemRatio);
    EXPECT_EQ(0x10, g_processManager.processes->numaAttr.numaNodes);

    // when scanType is HAM_SCAN/STATISTIC_SCAN, state should be set to PROC_MOVE
    // when nodeBitmap is not null, local numanodes should be updated
    uint32_t localNodeBitmap = 1;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    param.scanType = HAM_SCAN;
    ret = ProcessAddManage(&param, &localNodeBitmap);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(DEFAULT_SCAN_PERIOD, g_processManager.processes->scanTime);  // 首次扫描使用DEFAULT_SCAN_PERIOD
    EXPECT_EQ(param.duration, g_processManager.processes->duration);
    EXPECT_EQ(50, g_processManager.processes->initLocalMemRatio);
    EXPECT_EQ(0x10, g_processManager.processes->numaAttr.numaNodes);
    EXPECT_EQ(PROC_MOVE, g_processManager.processes->state);
}

TEST_F(ManageTest, TestProcessAddManageNewPidFailed)
{
    int ret;
    ProcessParam param = {
        .pid = 123,
        .scanType = NORMAL_SCAN,
        .count = 1,
    };
    param.numaParam[0].nid = 1;
    param.numaParam[0].ratio = 50;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(-EINVAL));
    ret = ProcessAddManage(&param, nullptr);
    EXPECT_EQ(-EINVAL, ret);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
}

static void FillPolicyForManageTest(GroupMigrationPolicy *policy)
{
    policy->enabled = true;
    policy->groupCount = 1;
    policy->groups[0].localCount = 1;
    policy->groups[0].locals[0].nid = 0;
    policy->groups[0].locals[0].localReservePages = 2;
    policy->groups[0].targetCount = 1;
    policy->groups[0].targets[0].nid = 4;
    policy->groups[0].targets[0].quotaPages = 8;
    policy->groups[0].targets[0].usedPages = 3;
}

extern "C" int ProcessAddGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);
TEST_F(ManageTest, TestProcessAddGroupedManageNewPid)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));

    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(0, ret);
    ASSERT_NE(nullptr, g_processManager.processes);
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
    EXPECT_EQ(1234, g_processManager.processes->pid);
    EXPECT_EQ((uint32_t)0x11, g_processManager.processes->numaAttr.numaNodes);
    EXPECT_TRUE(g_processManager.processes->groupPolicy.enabled);
    EXPECT_EQ((uint64_t)2, g_processManager.processes->groupPolicy.groups[0].locals[0].localReservePages);
    EXPECT_EQ((uint64_t)3, g_processManager.processes->groupPolicy.groups[0].targets[0].usedPages);

    free(g_processManager.processes);
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = 0;
}

TEST_F(ManageTest, TestProcessAddGroupedManageUpdateExistingPid)
{
    ProcessAttr current = {};
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);
    policy.groups[0].targets[0].usedPages = 6;

    current.pid = 1234;
    current.next = nullptr;
    g_processManager.processes = &current;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));

    int ret = ProcessAddGroupedManage(1234, 0x21, &policy);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1234, current.pid);
    EXPECT_EQ((uint32_t)0x21, current.numaAttr.numaNodes);
    EXPECT_TRUE(current.groupPolicy.enabled);
    EXPECT_EQ((uint64_t)6, current.groupPolicy.groups[0].targets[0].usedPages);
    g_processManager.processes = nullptr;
}

TEST_F(ManageTest, TestProcessAddGroupedManageRejectsInvalidInputs)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    MOCKER(CheckPid).stubs().will(returnValue(-EINVAL));
    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(CheckPid).stubs().will(returnValue(0));
    ret = ProcessAddGroupedManage(1234, 0x11, nullptr);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    policy.enabled = false;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestProcessAddGroupedManageRejectsLimitAndPreprocessFailure)
{
    GroupMigrationPolicy policy = {};
    FillPolicyForManageTest(&policy);

    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.processes = nullptr;
    g_processManager.nr[VM_TYPE] = MAX_2M_PROCESSES_CNT;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    int ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    g_processManager.nr[VM_TYPE] = 0;
    MOCKER(CheckPid).stubs().will(returnValue(0));
    MOCKER(VMPreprocess).stubs().will(returnValue(-ENOMEM));
    ret = ProcessAddGroupedManage(1234, 0x11, &policy);
    EXPECT_EQ(-ENOMEM, ret);
    EXPECT_EQ(nullptr, g_processManager.processes);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
}

extern "C" void CalcActcStats(ProcessAttr *attr);
extern "C" uint32_t GetRemoteHotThreshold(void);
TEST_F(ManageTest, TestCalcActcStats)
{
    ProcessAttr attr = {};
    ActcData data[3] = {};

    data[0].freq = 0;
    data[1].freq = 3;
    data[1].isWhiteListPage = true;
    data[2].freq = 10;
    attr.scanAttr.actcData[4] = data;
    attr.scanAttr.actcLen[4] = 3;
    attr.scanAttr.actCount[1].pageNum = 99;
    MOCKER(GetRemoteHotThreshold).stubs().will(returnValue((uint32_t)5));

    CalcActcStats(&attr);
    EXPECT_EQ((uint64_t)0, attr.scanAttr.actCount[1].pageNum);
    EXPECT_EQ((uint64_t)3, attr.scanAttr.actCount[4].pageNum);
    EXPECT_EQ((uint16_t)10, attr.scanAttr.actCount[4].freqMax);
    EXPECT_EQ((uint16_t)0, attr.scanAttr.actCount[4].freqMin);
    EXPECT_EQ((uint64_t)2, attr.scanAttr.actCount[4].freqNum);
    EXPECT_EQ((uint64_t)13, attr.scanAttr.actCount[4].freqSum);
    EXPECT_EQ((uint32_t)1, attr.scanAttr.actCount[4].freqZero);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actCount[4].remoteHotNum);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actCount[4].whiteNum);
}

extern "C" int DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf);
TEST_F(ManageTest, TestDistributeActcData)
{
    ProcessAttr attr = {};
    struct ProcessMemBitmap pmb = {};
    ActcData oldData[1] = {};
    ActcData buf[3] = {};

    attr.pid = 1234;
    attr.scanAttr.actcData[0] = (ActcData *)malloc(sizeof(ActcData));
    ASSERT_NE(nullptr, attr.scanAttr.actcData[0]);
    attr.scanAttr.actcData[0][0] = oldData[0];
    pmb.nrPages[0] = 2;
    pmb.nrPages[2] = 1;
    buf[0].addr = 0x1000;
    buf[1].addr = 0x2000;
    buf[2].addr = 0x3000;

    int ret = DistributeActcData(&attr, &pmb, buf);
    EXPECT_EQ(0, ret);
    ASSERT_NE(nullptr, attr.scanAttr.actcData[0]);
    ASSERT_NE(nullptr, attr.scanAttr.actcData[2]);
    EXPECT_EQ((uint64_t)2, attr.scanAttr.actcLen[0]);
    EXPECT_EQ((uint64_t)1, attr.scanAttr.actcLen[2]);
    EXPECT_EQ((uint64_t)0x1000, attr.scanAttr.actcData[0][0].addr);
    EXPECT_EQ((uint64_t)0x2000, attr.scanAttr.actcData[0][1].addr);
    EXPECT_EQ((uint64_t)0x3000, attr.scanAttr.actcData[2][0].addr);

    free(attr.scanAttr.actcData[0]);
    free(attr.scanAttr.actcData[2]);
    attr.scanAttr.actcData[0] = nullptr;
    attr.scanAttr.actcData[2] = nullptr;
}

TEST_F(ManageTest, TestCheckAndRemoveInvalidProcess)
{
    ProcessAttr attr = { .pid = 1025 };
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = &attr;
    g_processManager.nr[VM_TYPE] = 2;
    MOCKER(PidIsValid).stubs().will(returnValue(false));
    MOCKER(AccessIoctlRemovePid).expects(once()).will(returnValue(0));
    MOCKER(LinkedListRemove).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    CheckAndRemoveInvalidProcess();
    EXPECT_EQ(1, g_processManager.nr[VM_TYPE]);
}

TEST_F(ManageTest, TestCheckAndRemoveInvalidProcessTwo)
{
    EnvMutexInit(&g_processManager.lock);
    g_processManager.processes = nullptr;
    CheckAndRemoveInvalidProcess();
    EXPECT_EQ(0, g_processManager.remoteNumaInfo.usedInfo[0].used);
}

extern "C" void CalRemoteMemUsed(void);
TEST_F(ManageTest, TestCalRemoteMemUsed)
{
    ProcessAttr attr;
    attr.next = nullptr;
    g_processManager.processes = &attr;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes->walkPage.nrPages[4] = 1;
    CalRemoteMemUsed();
    EXPECT_EQ(1, g_processManager.remoteNumaInfo.usedInfo[0].used);
    g_processManager.processes = nullptr;
}

extern "C" void RemoveManagedProcess(int nr, pid_t *pidArr);
extern "C" struct ProcessManager g_processManager;
extern "C" PidType GetPidType(struct ProcessManager *manager);
TEST_F(ManageTest, TestRemoveManagedProcessInvalidPid)
{
    pid_t pid = 123;
    ProcessAttr mockProcess;
    mockProcess.pid = pid;
    mockProcess.next = nullptr;

    g_processManager.processes = &mockProcess;
    g_processManager.nr[PROCESS_TYPE] = 1;
    pid_t pidArr[1] = {1};

    MOCKER(GetPidType).stubs().will(returnValue(PROCESS_TYPE));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(1, g_processManager.nr[PROCESS_TYPE]);
}

TEST_F(ManageTest, TestRemoveManagedProcessValidPid)
{
    pid_t pid = 123;
    ProcessAttr mockProcess;
    mockProcess.pid = pid;
    mockProcess.next = nullptr;
    g_processManager.processes = &mockProcess;
    pid_t pidArr[1] = { pid };
    int ret;

    g_processManager.nr[VM_TYPE] = 1;
    MOCKER(GetPidType).stubs().will(returnValue(VM_TYPE));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(0, g_processManager.nr[VM_TYPE]);
    EXPECT_EQ(g_processManager.processes, nullptr);

    GlobalMockObject::verify();
    g_processManager.processes = &mockProcess;
    g_processManager.nr[PROCESS_TYPE] = 1;
    MOCKER(GetPidType).stubs().will(returnValue(PROCESS_TYPE));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    RemoveManagedProcess(1, pidArr);
    EXPECT_EQ(0, g_processManager.nr[PROCESS_TYPE]);
    EXPECT_EQ(g_processManager.processes, nullptr);
}

extern "C" void RemoveAllManagedProcess(void);
TEST_F(ManageTest, TestRemoveAllManagedProcess)
{
    int ret;

    pid_t pid = 123;

    EnvMutexInit(&g_processManager.lock);

    ProcessAttr *mockProcess = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    mockProcess->pid = pid;
    mockProcess->next = nullptr;

    MOCKER(AccessIoctlRemoveAllPid).stubs().will(returnValue(0));
    MOCKER(FreeProceccesAttr).stubs().will(ignoreReturnValue());
    g_processManager.processes = mockProcess;
    RemoveAllManagedProcess();
    EXPECT_EQ(g_processManager.processes, nullptr);
    EXPECT_EQ(g_processManager.nr[VM_TYPE], 0);
    EXPECT_EQ(g_processManager.nr[PROCESS_TYPE], 0);

    g_processManager.processes = nullptr;
    RemoveAllManagedProcess();
    EXPECT_EQ(g_processManager.processes, nullptr);
    EXPECT_EQ(g_processManager.nr[VM_TYPE], 0);
    EXPECT_EQ(g_processManager.nr[PROCESS_TYPE], 0);
}

extern "C" int DestroyProcessManager();
TEST_F(ManageTest, TestDestroyProcessManager)
{
    int ret;
    MOCKER(memset_s).stubs().will(ignoreReturnValue());
    ret = DestroyProcessManager();
    EXPECT_EQ(0, ret);
}

extern "C" struct ProcessManager *GetProcessManager(void);
TEST_F(ManageTest, TestGetProcessManager)
{
    struct ProcessManager *ret = GetProcessManager();
    EXPECT_EQ(&g_processManager, ret);
}

extern "C" void ResetActcData(ActcData *actcData[], int len);
TEST_F(ManageTest, TestResetActcData)
{
    int len = 10;
    ActcData **data = (ActcData **)malloc(sizeof(ActcData*) * len);
    for (int i = 0; i < len; i++) {
        data[i] = (ActcData *)malloc(sizeof(ActcData));
    }
    ResetActcData(data, len);
    EXPECT_EQ(data[0], nullptr);
    for (int i = 0; i < len; i++) {
        free(data[i]);
    }
    free(data);
}

extern "C" errno_t memcpy_s(void *dest, size_t numberOfElements, const void *src, size_t count);

TEST_F(ManageTest, TestFreeProceccesAttr)
{
    ProcessAttr *attr = nullptr;

    MOCKER(ResetActcData).expects(never());
    FreeProceccesAttr(attr);
    GlobalMockObject::verify();

    attr = (ProcessAttr *)malloc(sizeof(*attr));
    ASSERT_NE(nullptr, attr);
    MOCKER(ResetActcData).stubs().will(ignoreReturnValue());
    FreeProceccesAttr(attr);
}

extern "C" unsigned long ProcessSmapsFile(pid_t pid, const char *targetLinePrefix, size_t prefixLength, size_t divisor);
TEST_F(ManageTest, TestProcessSmapsFile)
{
    int pid = 1234;
    char targetLinePrefix[] = "test";
    size_t prefixLength = 0;
    size_t divisor = 1;
    MOCKER((int (*)(char *, unsigned long, unsigned long, char const *, void *))snprintf_s)
        .stubs()
        .will(returnValue(-1)).then(returnValue(0));
    unsigned long ret = ProcessSmapsFile(pid, targetLinePrefix, prefixLength, divisor);
    EXPECT_EQ(ret, 0);
    static FILE fake_file;
    MOCKER(fopen).stubs().will(returnValue(&fake_file));
    char buf[] = "1";
    MOCKER(fgets).stubs().will(returnValue(&buf[0])).then(returnValue((static_cast<char *>(nullptr))));
    MOCKER(fclose).stubs().will(returnValue(1));
    MOCKER((int (*)(char const *, char const *, void *))sscanf_s)
        .stubs()
        .will(returnValue(0));
    ret = ProcessSmapsFile(pid, targetLinePrefix, prefixLength, divisor);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetNormalPageCount(pid_t pid);
TEST_F(ManageTest, TestGetNormalPageCount)
{
    int pid = 1234;
    unsigned long ret = GetNormalPageCount(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetHugePageCount(pid_t pid);
TEST_F(ManageTest, TestGetHugePageCount)
{
    int pid = 1234;
    unsigned long ret = GetHugePageCount(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" unsigned long GetPidNrPages(pid_t pid);
TEST_F(ManageTest, TestGetPidNrPages)
{
    int pid = 1234;
    unsigned long ret = GetPidNrPages(pid);
    EXPECT_EQ(ret, 0);
}

extern "C" int GetNodeFromCpu(int cpu);
TEST_F(ManageTest, TestGetNodeFromCpu)
{
    int cpu = 1234;
    int ret = GetNodeFromCpu(cpu);
    EXPECT_EQ(ret, -EINVAL);
    MOCKER(access).stubs().will(returnValue(0));
    ret = GetNodeFromCpu(cpu);
    EXPECT_EQ(ret, 0);
}

#ifdef CPU_ISSET
#undef CPU_ISSET
#endif
#define CPU_ISSET(x, y) ((x) == 0 ? 1 : 0)
TEST_F(ManageTest, TestGetNumaNodesForPid)
{
    int pid = 1234;
    int node = 0;
    int ret = GetNumaNodesForPid(pid, &node);
    EXPECT_EQ(ret, -EINVAL);
    MOCKER(sched_getaffinity).stubs().will(returnValue(0));
    MOCKER(GetNodeFromCpu).stubs().will(returnValue(-EINVAL)).then(returnValue(0));
    ret = GetNumaNodesForPid(pid, &node);
    EXPECT_EQ(ret, 0);
}

extern "C" bool IsHugeAligned(uint64_t addr);
TEST_F(ManageTest, TestIsHugeAligned)
{
    uint64_t addr = 0;
    bool ret = IsHugeAligned(addr);
    EXPECT_EQ(ret, true);
}

TEST_F(ManageTest, TestIsHugePageRange)
{
    const char *line = "hugepage";
    int ret = IsHugePageRange(line);
    EXPECT_EQ(ret, 1);
}

extern "C" ssize_t read(int fd, void *buf, size_t count);

extern "C" int open(const char *pathname, int flags);

extern "C" void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len);
TEST_F(ManageTest, TestSetPidNrPages)
{
    ProcessAttr *attr = (ProcessAttr *)malloc(sizeof(ProcessAttr));
    size_t *nrPages = (size_t *)malloc(sizeof(size_t) * 2);
    nrPages[0] = 1;
    nrPages[1] = 1;
    SetPidNrPages(attr, nrPages, 2);
    EXPECT_EQ(1, attr->walkPage.nrPages[0]);
    EXPECT_EQ(1, attr->walkPage.nrPages[1]);
    EXPECT_EQ(2, attr->walkPage.nrPage);
    free(attr);
    free(nrPages);
}

extern "C" void ClearRemoteMemUsed();
TEST_F(ManageTest, TestClearRemoteMemUsed)
{
    g_processManager.remoteNumaInfo.usedInfo[1].used = 10;
    ClearRemoteMemUsed();
    EXPECT_EQ(0, g_processManager.remoteNumaInfo.usedInfo[1].used);
}

extern "C" uint64_t CalcRemoteBorrowPages(uint64_t size);
TEST_F(ManageTest, TestCalcRemoteBorrowPages)
{
    uint32_t ret;
    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = CalcRemoteBorrowPages(100);
    EXPECT_EQ(25600, ret);

    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = CalcRemoteBorrowPages(100);
    EXPECT_EQ(50, ret);
}

extern "C" void NoAccountAlloc(int remoteNid, ProcessAttr *attr);
TEST_F(ManageTest, TestNoAccountAlloc)
{
    ProcessAttr attr;
    attr.walkPage.nrPages[1] = 1;
    attr.numaAttr.numaNodes = 0b00010001;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    NoAccountAlloc(1, &attr);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[0][0]);
}

TEST_F(ManageTest, TestNoAccountAllocLocalNumaLen2)
{
    ProcessAttr attr;
    attr.walkPage.nrPages[2] = 2;
    attr.numaAttr.numaNodes = 0b00010011;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(2));
    NoAccountAlloc(2, &attr);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[0][0]);
    EXPECT_EQ(1, attr.strategyAttr.allocRemoteNrPages[1][0]);
}

extern "C" void CalRemotePerLocalWithAccount(int j, ProcessAttr *attr);
TEST_F(ManageTest, TestCalRemotePerLocalWithAccount)
{
    uint32_t ret;
    ProcessAttr attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 0;
    attr.strategyAttr.allocRemoteNrPages[0][0] = 0;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(1));
    MOCKER(InAttrL1).stubs().will(returnValue(true));
    CalRemotePerLocalWithAccount(2, &attr);
    EXPECT_EQ(0, attr.strategyAttr.allocRemoteNrPages[0][0]);
}

extern "C" void CalNrPagesLocalTotalPerPid(ProcessAttr *attr);
TEST_F(ManageTest, TestCalNrPagesLocalTotalPerPid)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][0] = 2;
    attr.strategyAttr.remoteNrPagesAfterMigrate[1][0] = 4;
    attr.strategyAttr.remoteNrPagesAfterMigrate[2][0] = 6;
    attr.strategyAttr.remoteNrPagesAfterMigrate[3][0] = 8;

    attr.walkPage.nrPages[0] = 2;
    attr.walkPage.nrPages[1] = 4;
    attr.walkPage.nrPages[2] = 6;
    attr.walkPage.nrPages[3] = 8;

    CalNrPagesLocalTotalPerPid(&attr);
    EXPECT_EQ(2, attr.strategyAttr.nrPagesPerLocalNuma[0]);
    EXPECT_EQ(4, attr.strategyAttr.nrPagesPerLocalNuma[1]);
    EXPECT_EQ(6, attr.strategyAttr.nrPagesPerLocalNuma[2]);
    EXPECT_EQ(8, attr.strategyAttr.nrPagesPerLocalNuma[3]);
}

TEST_F(ManageTest, TestCalNrPagesLocalTotalPerPidTwo)
{
    ProcessAttr attr = {};
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &attr;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][1] = 2;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][2] = 4;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][3] = 6;
    attr.strategyAttr.remoteNrPagesAfterMigrate[0][4] = 8;

    attr.walkPage.nrPages[0] = 1;
    attr.walkPage.nrPages[1] = 2;
    attr.walkPage.nrPages[2] = 3;
    attr.walkPage.nrPages[3] = 4;

    MOCKER(GetNrLocalNuma).stubs().will(returnValue(4));
    CalNrPagesLocalTotalPerPid(&attr);
    EXPECT_EQ(1, attr.strategyAttr.nrPagesPerLocalNuma[0]);
    EXPECT_EQ(2, attr.strategyAttr.nrPagesPerLocalNuma[1]);
    EXPECT_EQ(3, attr.strategyAttr.nrPagesPerLocalNuma[2]);
    EXPECT_EQ(4, attr.strategyAttr.nrPagesPerLocalNuma[3]);
}

extern "C" void CalNrPagesLocalTotal(void);
TEST_F(ManageTest, TestCalNrPagesLocalTotal)
{
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;
    g_processManager.processes->strategyAttr.remoteNrPagesAfterMigrate[0][0] = 2;
    g_processManager.processes->walkPage.nrPages[0] = 2;
    CalNrPagesLocalTotal();
    EXPECT_EQ(2, g_processManager.processes->strategyAttr.nrPagesPerLocalNuma[0]);
}

extern "C" void CalRemoteNumaAllocPerPid(int i, int j, uint32_t tmpNrPagesToUse,
    uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM]);
TEST_F(ManageTest, TestCalRemoteNumaAllocPerPid)
{
    uint32_t tmp[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = {0};

    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    g_processManager.nr[0] = 100;
    tmp[0][0] = 100;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;
    attr.strategyAttr.l2RemoteMemRatio[0][0] = 20;

    CalRemoteNumaAllocPerPid(0, 0, 100, tmp);

    EXPECT_EQ(50, attr.strategyAttr.l2RemoteMemRatio[0][0]);
}

extern "C" void CalRemoteNumaSizeAllocPerNuma(void);
TEST_F(ManageTest, TestCalRemoteNumaSizeAllocPerNuma)
{
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.remoteNumaInfo.privateSize[0][0] = 300;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;

    CalRemoteNumaSizeAllocPerNuma();
    EXPECT_EQ(50, g_processManager.processes->strategyAttr.l2RemoteMemRatio[0][0]);
}

extern "C" void CalcMigrateNrPagesPerPIDMuiltNuma(void);
TEST_F(ManageTest, TestCalcMigrateNrPagesPerPIDMuiltNuma)
{
    g_runMode = WATERLINE_MODE;
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 300;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    attr.strategyAttr.nrPagesPerLocalNuma[0] = 10;
    attr.strategyAttr.initRemoteMemRatio[0][0] = 50;

    CalcMigrateNrPagesPerPIDMuiltNuma();
    EXPECT_EQ(0, g_processManager.processes->strategyAttr.l2RemoteMemRatio[0][0]);
}

TEST_F(ManageTest, TestSetRemoteNumaInfo)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
        }
    }
    g_processManager.nrLocalNuma = 4;
    ASSERT_EQ(0, borrowMem->usedInfo[1].size);
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(0));
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(50, borrowMem->usedInfo[1].size);
    borrowMem->usedInfo[1].size = 0;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100 << 8, borrowMem->usedInfo[1].size);
    borrowMem->usedInfo[1].size = 0;
}

TEST_F(ManageTest, TestSetRemoteNumaInfoError)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
        }
    }
    g_processManager.nrLocalNuma = 4;
    ASSERT_EQ(0, borrowMem->usedInfo[1].size);
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(-EINVAL));
    ret = SetRemoteNumaInfo(0, 5, 100);
    EXPECT_EQ(-EBADF, ret);
    EXPECT_EQ(0, borrowMem->usedInfo[1].size);
}

TEST_F(ManageTest, TestSetRemoteNumaInfoShared)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            borrowMem->usedInfo[j].size = 0;
            borrowMem->sharedSize[j] = 0;
            borrowMem->privateSize[i][j] = 0;
        }
    }
    MOCKER(SyncOneNumaConfig).stubs().will(returnValue(0));
    g_processManager.tracking.pageSize = PAGESIZE_2M;
    ret = SetRemoteNumaInfo(NUMA_NO_NODE, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, borrowMem->sharedSize[1]);
    EXPECT_EQ(50, borrowMem->usedInfo[1].size);
    borrowMem->sharedSize[1] = 0;
    borrowMem->usedInfo[1].size = 0;

    g_processManager.tracking.pageSize = PAGESIZE_4K;
    ret = SetRemoteNumaInfo(NUMA_NO_NODE, 5, 100);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(100, borrowMem->sharedSize[1]);
    EXPECT_EQ(100 << 8, borrowMem->usedInfo[1].size);
    borrowMem->sharedSize[1] = 0;
    borrowMem->usedInfo[1].size = 0;
}

TEST_F(ManageTest, TestSetRemoteNumaInfoInvalidDestNid)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        borrowMem->usedInfo[j].size = 0;
        borrowMem->sharedSize[j] = 0;
    }
    ret = SetRemoteNumaInfo(0, 2, 100);
    EXPECT_EQ(-EBADF, ret);
    EXPECT_EQ(0, borrowMem->privateSize[0][2]);
}

TEST_F(ManageTest, TestSetRemoteNumaInfoInvalidSrcNid)
{
    int ret;
    struct RemoteNumaInfo *borrowMem = &g_processManager.remoteNumaInfo;
    g_processManager.nrLocalNuma = 4;
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        borrowMem->usedInfo[j].size = 0;
    }
    ret = SetRemoteNumaInfo(5, 5, 100);
    EXPECT_EQ(-EBADF, ret);
}

extern "C" bool CheckBorrowUsed(int destNid);
TEST_F(ManageTest, TestCheckBorrowUsed)
{
    bool ret;
    g_processManager.nrLocalNuma = 4;
    g_processManager.remoteNumaInfo.usedInfo[0].used = 10;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 5;
    MOCKER(EnvMsleep).stubs();
    ret = CheckBorrowUsed(4);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestCheckBorrowUsedTwo)
{
    bool ret;
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo.usedInfo[0].used = 1;
    g_processManager.remoteNumaInfo.usedInfo[0].size = 5;
    g_processManager.remoteNumaInfo.usedInfo[0].ifUsedFreshed = false;
    MOCKER(EnvMsleep).stubs();
    ret = CheckBorrowUsed(1);
    EXPECT_EQ(false, ret);
}

extern "C" bool CheckPrivateBorrowUsed(int destNid);
TEST_F(ManageTest, CheckPrivateBorrowUsedWithoutNuma)
{
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo = {};
    bool ret = CheckPrivateBorrowUsed(2);
    EXPECT_EQ(false, ret);
}

extern "C" bool CheckPrivateBorrowUsed(int destNid);
TEST_F(ManageTest, CheckPrivateBorrowUsed)
{
    g_processManager.nrLocalNuma = 1;
    g_processManager.remoteNumaInfo.privateUsedInfo[0][1].ifUsedFreshed = true;
    MOCKER(EnvMsleep).stubs();
    bool ret = CheckPrivateBorrowUsed(2);
    EXPECT_EQ(true, ret);
}

TEST_F(ManageTest, TestCheckReadyMigrateBack)
{
    bool ret;
    ProcessAttr attr = {};
    g_processManager.processes = &attr;
    g_processManager.processes->next = nullptr;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes->numaAttr.numaNodes = 1;
    g_processManager.remoteNumaInfo.sharedSize[1] = 1;
    EnvMutexInit(&g_processManager.lock);
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(true));
    MOCKER(CheckPrivateBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(true, ret);

    GlobalMockObject::verify();
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(false));
    MOCKER(CheckPrivateBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(false, ret);
}

TEST_F(ManageTest, TestCheckReadyMigrateBackTwo)
{
    bool ret;
    g_processManager.processes = nullptr;
    MOCKER(CheckBorrowUsed).stubs().will(returnValue(true));
    ret = CheckReadyMigrateBack(5);
    EXPECT_EQ(true, ret);
}

extern "C" int IsPidArrInState(pid_t *pidArr, int len, enum ProcessState state);
extern "C" int IsPidArrayStateChangeReady(pid_t *pidArr, int len, int enable);
TEST_F(ManageTest, TestIsPidArrInStateInvalid)
{
    ProcessAttr pid = { .state = PROC_IDLE };
    pid_t pidArr[] = { 1, 2 };

    int ret = IsPidArrayStateChangeReady(nullptr, 2, 1);
    EXPECT_EQ(-EINVAL, ret);
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue((ProcessAttr*)nullptr)).then(returnValue(&pid));
    ret = IsPidArrayStateChangeReady(pidArr, 2, 1);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsPidArrInStateNormal)
{
    ProcessAttr pid1 = { .state = PROC_MIGRATE };
    ProcessAttr pid2 = { .state = PROC_MOVE };
    pid_t pidArr[] = { 1, 2 };

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    int ret = IsPidArrayStateChangeReady(pidArr, 2, 0);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    pid2.state = PROC_IDLE;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    ret = IsPidArrayStateChangeReady(pidArr, 2, 1);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsPidArrInState)
{
    ProcessAttr pid1 = { .state = PROC_MOVE };
    ProcessAttr pid2 = { .state = PROC_MOVE };
    pid_t pidArr[] = { 1, 2 };

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    int ret = IsPidArrInState(pidArr, 2, PROC_MOVE);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    pid2.state = PROC_IDLE;
    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    ret = IsPidArrInState(pidArr, 2, PROC_MOVE);
    EXPECT_EQ(0, ret);
}

extern "C" void SetPidArrState(pid_t *pidArr, int len, enum ProcessState state, int enable);
TEST_F(ManageTest, TestSetPidArrState)
{
    ProcessAttr pid1 = { .state = PROC_IDLE };
    ProcessAttr pid2 = { .state = PROC_IDLE };
    pid_t pidArr[] = { 1, 2 };

    MOCKER(GetProcessAttrLocked).stubs().will(returnValue(&pid1)).then(returnValue(&pid2));
    SetPidArrState(pidArr, 2, PROC_MOVE, 0);
    EXPECT_EQ(PROC_MOVE, pid1.state);
    EXPECT_EQ(PROC_MOVE, pid2.state);
}

TEST_F(ManageTest, TestChangePidRemoteByNuma)
{
    int srcNid = 4;
    int destNid = 6;
    int anotherNid = 5;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.numaAttr.numaNodes = 0b01010000;
    pid2.numaAttr.numaNodes = 0b00110000;
    pid1.next = &pid2;
    g_processManager.processes = &pid1;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetCurrentMaxNrPid).stubs().will(returnValue(2));
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(EINVAL));
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(AccessIoctlAddPid).expects(once()).will(returnValue(0));
    ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByNumaTwo)
{
    g_processManager.processes = nullptr;
    int srcNid = 4;
    int destNid = 6;
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByNumaSyncAllProcessConfigFail)
{
    int srcNid = 4;
    int destNid = 6;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.numaAttr.numaNodes = 0b01010000;
    pid2.numaAttr.numaNodes = 0b00110000;
    pid1.next = &pid2;
    g_processManager.processes = &pid1;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(GetCurrentMaxNrPid).stubs().will(returnValue(2));
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(0));
    MOCKER(SyncAllProcessConfig).expects(once()).will(returnValue(-EBADF));
    int ret = ChangePidRemoteByNuma(srcNid, destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestChangePidRemoteByPid)
{
    int srcNid = 4;
    int destNid = 6;
    int pidLen = 1;
    struct MigPidRemoteNumaIoctlMsg msg = {
        .pidCnt = 1,
    };
    msg.migResArray = (int *)calloc(1, sizeof(int));
    msg.payloads = (struct MigPayload *)malloc(sizeof(struct MigPayload));
    ProcessAttr pid1 = {};
    pid1.pid = 100;

    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.processes = &pid1;
    msg.payloads[0].pid = pid1.pid;
    msg.payloads[0].srcNid = 4;
    msg.payloads[0].destNid = 6;

    EnvMutexInit(&g_processManager.lock);
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(0));
    int ret = ChangePidRemoteByPid(&msg);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    MOCKER(AccessIoctlAddPid).stubs().will(returnValue(EBADF));
    ret = ChangePidRemoteByPid(&msg);
    EXPECT_EQ(EBADF, ret);
    free(msg.migResArray);
    free(msg.payloads);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableInvalid)
{
    pid_t pidArr[] = { 1 };

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(-EINVAL));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableRetryFail)
{
    pid_t pidArr[] = { 1 };

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(-ETIMEDOUT, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableRetrySuccess)
{
    pid_t pidArr[] = { 1 };

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(0)).then(returnValue(1));
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    MOCKER(SetPidArrState).expects(once());
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateDisableNormal)
{
    pid_t pidArr[] = { 1 };

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(1));
    MOCKER(SetPidArrState).expects(once());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, DISABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestEnableProcessMigrateEnableNormal)
{
    pid_t pidArr[] = { 1 };

    EnvMutexInit(&g_processManager.lock);
    MOCKER(IsPidArrayStateChangeReady).stubs().will(returnValue(1));
    MOCKER(SetPidArrState).expects(once());
    MOCKER(SyncAllProcessConfig).stubs().will(returnValue(0));
    int ret = EnableProcessMigrate(pidArr, 1, ENABLE_PROCESS_MIGRATE);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedInvalid)
{
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(0);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedFail)
{
    int destNid = 4;
    ProcessAttr pid1 = {};
    pid1.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.nrLocalNuma = 4;
    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMigrateBackAllowedSuccess)
{
    int destNid = 4;
    int anotherNid = 5;
    ProcessAttr pid1 = {};
    ProcessAttr pid2 = {};
    pid1.state = PROC_MOVE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid2.next = &pid1;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    g_processManager.processes = &pid2;
    ret = IsRemoteNumaMigrateBackAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedInvalid)
{
    g_processManager.nrLocalNuma = 4;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(0);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedFail)
{
    int destNid = 4;
    ProcessAttr pid1;
    pid1.state = PROC_MIGRATE;
    pid1.numaAttr.numaNodes = 0b00010001;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(0, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccess)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessTwo)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 5;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(-22, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessThree)
{
    int destNid = 7;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_MIGRATE;
    pid2.state = PROC_MOVE;
    pid1.numaAttr.numaNodes = 0b00100001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 2;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(1, ret);
}

TEST_F(ManageTest, TestIsRemoteNumaMoveAllowedSuccessFour)
{
    int destNid = 4;
    ProcessAttr pid1;
    ProcessAttr pid2;
    pid1.state = PROC_BACK;
    pid2.state = PROC_BACK;
    pid1.numaAttr.numaNodes = 0b00010001;
    pid2.numaAttr.numaNodes = 0b00010001;
    pid1.next = &pid2;
    pid2.next = nullptr;
    g_processManager.nrLocalNuma = 4;

    g_processManager.processes = &pid1;
    EnvMutexInit(&g_processManager.lock);
    int ret = IsRemoteNumaMoveAllowed(destNid);
    EXPECT_EQ(0, ret);
}

extern "C" bool MigOutIsDone(ProcessAttr *attr, bool *isMultiNumaPid);
const int NR_PAGES_L1 = 5;
const int NR_PAGE = 10;
const pid_t PID = 123;
TEST_F(ManageTest, TestMigOutIsDoneSuccess)
{
    bool ret;
    pid_t pid = PID;
    bool isMultiNumaPid = false;
    ProcessAttr attr = {};
    attr.walkPage.nrPages[0] = NR_PAGES_L1;
    attr.walkPage.nrPage = NR_PAGE;
    attr.pid = PID;
    attr.migrateMode = MIG_MEMSIZE_MODE;
    attr.strategyAttr.memSize[0][0] = 10240;
    g_processManager.processes = nullptr;
    ret = MigOutIsDone(&attr, &isMultiNumaPid);
    EXPECT_EQ(false, ret);

    attr.numaAttr.numaNodes = 0b00010001;
    attr.remoteNumaCnt = 1;
    g_processManager.processes = &attr;
    ret = MigOutIsDone(&attr, &isMultiNumaPid);
    EXPECT_EQ(true, ret);
}
