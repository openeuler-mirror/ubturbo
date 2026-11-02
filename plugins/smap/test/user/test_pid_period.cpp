/* Per-PID attributes and effective period priority integration tests. */
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include "gtest/gtest.h"
#include "manage/access_ioctl.h"
#include "manage/thread.h"
#include "mockcpp/mokc.h"
#include "smap_inner_interface.h"
#include "strategy/migration.h"
#include "strategy/strategy_config.h"

extern "C" EnvAtomic g_status;
extern "C" int RestartPidScan(pid_t pid);
extern "C" void UpdateProcessPeriods(ProcessAttr *attr);

namespace {
AccessAddPidPayload captured[2];
bool fileEnabled;
uint32_t fileScan, fileMigrate;
int failures;
bool FileEnabled()
{
    return fileEnabled;
}
uint32_t FileScan()
{
    return fileScan;
}
uint32_t FileMigrate()
{
    return fileMigrate;
}
int CaptureIoctl(int, unsigned long cmd, void *arg)
{
    EXPECT_EQ(static_cast<unsigned long>(SMAP_ACCESS_ADD_PID), cmd);
    if (failures > 0) {
        failures--;
        errno = EIO;
        return -1;
    }
    auto *msg = static_cast<AccessAddPidMsg *>(arg);
    for (int i = 0; i < msg->count && i < 2; i++)
        captured[i] = msg->payload[i];
    return 0;
}
} // namespace

class PidPeriodTest : public ::testing::Test {
protected:
    ProcessManager *manager;
    int oldStatus, oldEpfd;
    uint32_t oldPeriod;
    void SetUp() override
    {
        manager = GetProcessManager();
        oldStatus = EnvAtomicRead(&g_status);
        oldEpfd = manager->eventLoop.epfd;
        oldPeriod = manager->migPeriod;
        PidSlotInit(manager);
        manager->eventLoop.epfd = -1;
        manager->migPeriod = 2000;
        EnvAtomicSet(&g_status, 1);
        fileEnabled = false;
        fileScan = 1000;
        fileMigrate = 10000;
        failures = 0;
        memset(captured, 0, sizeof(captured));
        MOCKER(GetFileConfSwitchConfig).stubs().will(invoke(FileEnabled));
        MOCKER(GetScanPeriodConfig).stubs().will(invoke(FileScan));
        MOCKER(GetMigratePeriodConfig).stubs().will(invoke(FileMigrate));
        MOCKER((int (*)(int, unsigned long, void *))ioctl).stubs().will(invoke(CaptureIoctl));
    }
    void TearDown() override
    {
        for (int i = 0; i < MAX_PID_SLOTS; i++) {
            if (EnvAtomicRead(&manager->slots[i].state) == PID_SLOT_INUSE)
                PidSlotRemove(manager, manager->slots[i].pid);
        }
        PidSlotDestroy(manager);
        manager->eventLoop.epfd = oldEpfd;
        manager->migPeriod = oldPeriod;
        EnvAtomicSet(&g_status, oldStatus);
        GlobalMockObject::verify();
    }
    ProcessAttr *Add(pid_t pid, ScanType mode = NORMAL_SCAN)
    {
        auto *attr = static_cast<ProcessAttr *>(calloc(1, sizeof(ProcessAttr)));
        attr->pid = pid;
        attr->scanType = mode;
        attr->state = PROC_IDLE;
        attr->type = PROCESS_TYPE;
        EXPECT_GE(PidSlotAdd(manager, attr), 0);
        return attr;
    }
};

TEST_F(PidPeriodTest, RejectsInvalidRequests)
{
    Add(100);
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(100, 49, 2000));
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(100, 60001, 100000));
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(100, 50, 499));
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(100, 50, 300001));
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(100, 2000, 1000));
    EXPECT_EQ(-EINVAL, SmapSetPidPeriod(0, 200, 2000));
    EXPECT_EQ(-ENOENT, SmapSetPidPeriod(101, 200, 2000));
    EXPECT_EQ(-ENOENT, SmapResetPidPeriod(101));
    Add(102, STATISTIC_SCAN);
    Add(103, HAM_SCAN);
    EXPECT_EQ(-EOPNOTSUPP, SmapSetPidPeriod(102, 200, 2000));
    EXPECT_EQ(-EOPNOTSUPP, SmapResetPidPeriod(103));
    EnvAtomicSet(&g_status, 0);
    EXPECT_EQ(-EPERM, SmapSetPidPeriod(100, 200, 2000));
    EXPECT_EQ(-EPERM, SmapResetPidPeriod(100));
}

TEST_F(PidPeriodTest, InnerDirectlyChangesOnlyTargetAttributes)
{
    auto *first = Add(100);
    auto *second = Add(101);
    EXPECT_EQ(0, SmapSetPidPeriod(100, 300, 1000));
    EXPECT_EQ(300u, first->scanTime);
    EXPECT_EQ(1000u, first->migPeriod);
    EXPECT_EQ(200u, second->scanTime);
    EXPECT_EQ(2000u, second->migPeriod);
    EXPECT_EQ(0u, captured[0].nTimes); // setter performs no ioctl
    UpdateProcessPeriods(first);
    EXPECT_EQ(1000u, captured[0].duration);
    EXPECT_EQ(3u, captured[0].nTimes);
    EXPECT_EQ(2000u, manager->migPeriod);
}

TEST_F(PidPeriodTest, TrackingModeAndDurationDoNotOverwriteMigrationPeriod)
{
    auto *attr = Add(100);
    attr->state = PROC_MOVE;
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    ASSERT_EQ(0, UpdateManagedProcessTrackingMode(attr, HAM_SCAN, 300, 123));
    EXPECT_EQ(123u, attr->duration);
    EXPECT_EQ(5000u, attr->migPeriod);
    AccessAddPidPayload payload = {};
    payload.pid = 100;
    payload.type = HAM_SCAN;
    payload.scanTime = attr->scanTime;
    payload.duration = attr->duration;
    ASSERT_EQ(0, AccessIoctlAddPid(1, &payload));
    EXPECT_EQ(123u, captured[0].duration);
    EXPECT_EQ(6u, captured[0].nTimes);

    ASSERT_EQ(0, UpdateManagedProcessTrackingMode(attr, STATISTIC_SCAN, 100, 2));
    EXPECT_EQ(2u, attr->duration);
    EXPECT_EQ(5000u, attr->migPeriod);
    payload.type = STATISTIC_SCAN;
    payload.scanTime = attr->scanTime;
    payload.duration = attr->duration;
    ASSERT_EQ(0, AccessIoctlAddPid(1, &payload));
    EXPECT_EQ(2u, captured[0].duration);
    EXPECT_EQ(20u, captured[0].nTimes);

    ASSERT_EQ(0, UpdateManagedProcessTrackingMode(attr, NORMAL_SCAN, 500, 120));
    EXPECT_EQ(5000u, attr->migPeriod);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(5000u, captured[0].duration);
    EXPECT_EQ(10u, captured[0].nTimes);
    EXPECT_EQ(120u, attr->duration);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 600, 6000));
    EXPECT_EQ(120u, attr->duration);
    ASSERT_EQ(0, SmapResetPidPeriod(100));
    EXPECT_EQ(120u, attr->duration);
    EXPECT_EQ(2000u, attr->migPeriod);
}

TEST_F(PidPeriodTest, SlotDefaultsPreserveScanDuration)
{
    auto *attr = static_cast<ProcessAttr *>(calloc(1, sizeof(ProcessAttr)));
    attr->pid = 100;
    attr->scanType = NORMAL_SCAN;
    attr->duration = 123;
    ASSERT_GE(PidSlotAdd(manager, attr), 0);
    EXPECT_EQ(123u, attr->duration);
    EXPECT_EQ(DEFAULT_MIGRATE_PERIOD, attr->migPeriod);
}

TEST_F(PidPeriodTest, FileOverridesInnerAndChangesApplyToExistingPid)
{
    auto *attr = Add(100);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    fileEnabled = true;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(1000u, captured[0].scanTime);
    EXPECT_EQ(10000u, captured[0].duration);
    EXPECT_EQ(10u, captured[0].nTimes);
    EXPECT_EQ(500u, attr->scanTime);
    fileScan = 700;
    fileMigrate = 7000;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(700u, captured[0].scanTime);
    EXPECT_EQ(7000u, captured[0].duration);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 600, 6000));
    UpdateProcessPeriods(attr);
    EXPECT_EQ(700u, captured[0].scanTime);
    EXPECT_EQ(7000u, captured[0].duration);
    fileEnabled = false;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(600u, captured[0].scanTime);
    EXPECT_EQ(6000u, captured[0].duration);
    EXPECT_EQ(10u, captured[0].nTimes);
}

TEST_F(PidPeriodTest, DefaultsAndResetRespectFilePriority)
{
    auto *attr = Add(100);
    EXPECT_EQ(200u, attr->scanTime);
    EXPECT_EQ(2000u, attr->migPeriod);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(200u, captured[0].scanTime);
    EXPECT_EQ(2000u, captured[0].duration);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    fileEnabled = true;
    ASSERT_EQ(0, SmapResetPidPeriod(100));
    EXPECT_EQ(200u, attr->scanTime);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(1000u, captured[0].scanTime);
    EXPECT_EQ(10000u, captured[0].duration);
    fileEnabled = false;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(200u, captured[0].scanTime);
    EXPECT_EQ(2000u, captured[0].duration);
}

TEST_F(PidPeriodTest, NonDivisibleMaximumWindowMatchesKernel)
{
    auto *attr = Add(100);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 700, 300000));
    UpdateProcessPeriods(attr);
    EXPECT_EQ(428u, captured[0].nTimes);
    fileEnabled = true;
    fileScan = 700;
    fileMigrate = 300000;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(428u, captured[0].nTimes);
}

TEST_F(PidPeriodTest, FailedIoctlDoesNotUndoAttributesAndNextWindowRetries)
{
    auto *attr = Add(100);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    failures = 1;
    /* ioctl 失败时 mock 不捕获 payload，captured 保持 SetUp 的 memset 0 */
    UpdateProcessPeriods(attr);
    EXPECT_EQ(0u, captured[0].scanTime);
    EXPECT_EQ(500u, attr->scanTime);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(500u, captured[0].scanTime);
}

TEST_F(PidPeriodTest, WorkerUpdatesPeriodsBeforeRestart)
{
    auto *attr = Add(100);
    attr->state = PROC_MOVE;
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    MOCKER(RestartPidScan).expects(once()).with(eq(pid_t(100))).will(returnValue(0));
    auto *slot = PidSlotGetRef(100);
    PidMigrationWork(manager, slot, 100);
    EXPECT_EQ(500u, captured[0].scanTime);
}

TEST_F(PidPeriodTest, StatisticsKeepSecondsAndHamKeepsMaintenancePeriod)
{
    fileEnabled = true;
    AccessAddPidPayload payload[2] = {};
    payload[0].type = STATISTIC_SCAN;
    payload[0].scanTime = 100;
    payload[0].duration = 2;
    payload[1].type = HAM_SCAN;
    payload[1].scanTime = 300;
    payload[1].duration = 123;
    ASSERT_EQ(0, AccessIoctlAddPid(2, payload));
    EXPECT_EQ(20u, captured[0].nTimes);
    EXPECT_EQ(2u, captured[0].duration);
    EXPECT_EQ(6u, captured[1].nTimes);
}

TEST_F(PidPeriodTest, NewPidKernelTrackingUsesFileBeforeSlotPublication)
{
    fileEnabled = true;
    AccessAddPidPayload payload = {};
    payload.pid = 100;
    payload.type = NORMAL_SCAN;
    ASSERT_EQ(0, AccessIoctlAddPid(1, &payload));
    EXPECT_EQ(1000u, captured[0].scanTime);
    EXPECT_EQ(10000u, captured[0].duration);
    EXPECT_EQ(10u, captured[0].nTimes);
}

TEST_F(PidPeriodTest, SetAndResetWaitForOwningSlotLock)
{
    auto *attr = Add(100);
    auto *slot = PidSlotGetRef(100);
    for (bool reset : {false, true}) {
        EnvMutexLock(&slot->attrLock);
        std::promise<void> started;
        auto worker = std::async(std::launch::async, [&started, reset] {
            started.set_value();
            return reset ? SmapResetPidPeriod(100) : SmapSetPidPeriod(100, 500, 5000);
        });
        started.get_future().wait();
        EXPECT_EQ(std::future_status::timeout, worker.wait_for(std::chrono::milliseconds(20)));
        EXPECT_EQ(reset ? 500u : 200u, attr->scanTime);
        EnvMutexUnlock(&slot->attrLock);
        EXPECT_EQ(0, worker.get());
        EXPECT_EQ(reset ? 200u : 500u, attr->scanTime);
        EXPECT_EQ(reset ? 2000u : 5000u, attr->migPeriod);
    }
    EXPECT_EQ(2, EnvAtomicRead(&slot->refs));
    PidSlotReleaseRefs(&slot, 1);
}
