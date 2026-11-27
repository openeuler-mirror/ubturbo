/* Per-PID attributes and effective period priority integration tests. */
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include "gtest/gtest.h"
#include "manage/access_ioctl.h"
#include "manage/manage_internal.h"
#include "manage/thread.h"
#include "mockcpp/mokc.h"
#include "smap_inner_interface.h"
#include "strategy/migration.h"
#include "strategy/strategy_config.h"

extern "C" EnvAtomic g_status;
extern "C" int RestartPidScan(pid_t pid);
extern "C" void UpdateProcessPeriods(ProcessAttr *attr);
extern "C" void UpdateScenePerPid(ProcessAttr *attr);
extern "C" void ApplyUbBwStop(ProcessAttr *attr, ProcessManager *manager);
extern "C" int BuildPidPairPlans(struct ProcessManager *manager, ProcessAttr *process, uint32_t **plannedOut,
                                 int *plannedNrLocalOut);

namespace
{
AccessAddPidPayload captured[2];
bool fileEnabled;
bool adaptiveEnabled;
bool AdaptiveEnabled()
{
    return adaptiveEnabled;
}
uint32_t fileScan, fileMigrate;
int failures;
int restartCount;
int profileReads;
void ChangingAdaptivePeriods(bool, uint32_t *scan, uint32_t *migrate)
{
    profileReads++;
    *scan = 100 * profileReads;
    *migrate = 1000 * profileReads;
}
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
    for (int i = 0; i < msg->count && i < 2; i++) {
        auto *wire = reinterpret_cast<AccessAddPidWirePayload *>(reinterpret_cast<char *>(msg->payload) +
                                                                 static_cast<size_t>(i) * msg->stride);
        captured[i].pid = wire->pid;
        captured[i].scanTime = wire->scanTime;
        captured[i].duration = wire->duration;
        captured[i].type = wire->type;
        captured[i].nTimes = wire->nTimes;
    }
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
        adaptiveEnabled = false;
        MOCKER(GetAdaptivePeriodEnableConfig).stubs().will(invoke(AdaptiveEnabled));
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

TEST_F(PidPeriodTest, TargetPublicationPreservesManualPeriodsAndScanWindows)
{
    auto *attr = Add(100);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 3000, 6000));
    attr->adaptivePeriod.previousValid = true;
    attr->periodFeedbackReady = true;
    ProcessAttr candidate = *attr;

    PublishProcessTargetCandidate(attr, &candidate);

    EXPECT_EQ(3000u, attr->scanTime);
    EXPECT_EQ(6000u, attr->migPeriod);
    EXPECT_FALSE(attr->adaptivePeriod.previousValid);
    EXPECT_FALSE(attr->periodFeedbackReady);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(3000u, captured[0].scanTime);
    EXPECT_EQ(6000u, captured[0].duration);
    EXPECT_EQ(2u, captured[0].nTimes);
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

TEST_F(PidPeriodTest, AdaptiveUpdateResolvesOnceAndRecordsTheSubmittedPeriods)
{
    adaptiveEnabled = true;
    auto *attr = Add(100);
    profileReads = 0;
    MOCKER(GetAdaptivePeriods).expects(exactly(2)).will(invoke(ChangingAdaptivePeriods));
    for (int update = 1; update <= 2; update++) {
        // Cover both a steady enabled switch and a switch from disabled to enabled.
        if (update == 2) {
            attr->adaptivePeriod.appliedAdaptive = false;
        }
        UpdateProcessPeriods(attr);
        EXPECT_EQ(update, profileReads);
        EXPECT_EQ(attr->scanTime, captured[0].scanTime);
        EXPECT_EQ(attr->migPeriod, captured[0].duration);
        EXPECT_EQ(captured[0].scanTime, attr->adaptivePeriod.appliedScan);
        EXPECT_EQ(captured[0].duration, attr->adaptivePeriod.appliedMigrate);
        EXPECT_TRUE(attr->adaptivePeriod.appliedAdaptive);
    }
}

TEST_F(PidPeriodTest, UpdateIoctlUsesResolvedPayloadWithoutReadingProcessPeriods)
{
    Add(100);
    MOCKER(GetProcessPeriods).expects(never());
    AccessAddPidPayload payload = {};
    payload.pid = 100;
    payload.type = NORMAL_SCAN;
    payload.scanTime = 3000;
    payload.duration = 6000;
    ASSERT_EQ(0, AccessIoctlUpdatePid(1, &payload));
    EXPECT_EQ(3000u, captured[0].scanTime);
    EXPECT_EQ(6000u, captured[0].duration);
    EXPECT_EQ(2u, captured[0].nTimes);
}

TEST_F(PidPeriodTest, FailedIoctlDoesNotUndoAttributesAndNextWindowRetries)
{
    auto *attr = Add(100);
    ASSERT_EQ(0, SmapSetPidPeriod(100, 500, 5000));
    uint32_t appliedScan = attr->adaptivePeriod.appliedScan;
    uint32_t appliedMigrate = attr->adaptivePeriod.appliedMigrate;
    failures = 1;
    /* ioctl 失败时 mock 不捕获 payload，captured 保持 SetUp 的 memset 0 */
    UpdateProcessPeriods(attr);
    EXPECT_EQ(0u, captured[0].scanTime);
    EXPECT_EQ(500u, attr->scanTime);
    EXPECT_EQ(appliedScan, attr->adaptivePeriod.appliedScan);
    EXPECT_EQ(appliedMigrate, attr->adaptivePeriod.appliedMigrate);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(500u, captured[0].scanTime);
    EXPECT_EQ(captured[0].scanTime, attr->adaptivePeriod.appliedScan);
    EXPECT_EQ(captured[0].duration, attr->adaptivePeriod.appliedMigrate);
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
    for (bool reset : { false, true }) {
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

TEST_F(PidPeriodTest, WorkerObservesOldWindowThenAppliesNewGearBeforeRestart)
{
    adaptiveEnabled = true;
    auto *attr = Add(100);
    MOCKER(BuildPidData).stubs().will(invoke(+[](ProcessAttr *process) -> int {
        process->periodFeedbackReady = true;
        process->periodFeedback.localHotPages = 1;
        process->periodFeedback.localFreq = 1;
        process->periodFeedback.remoteHotRatio = 0.10;
        return 0;
    }));
    MOCKER(UpdateScenePerPid).stubs();
    MOCKER(ApplyUbBwStop).stubs();
    /* 跳过规划流程，仅验证"旧窗口观测→新档位生效→重启扫描"的顺序 */
    MOCKER(BuildPidPairPlans).stubs().will(returnValue(-EINVAL));
    MOCKER(RestartPidScan).expects(exactly(3)).will(invoke(+[](pid_t) -> int {
        EXPECT_NE(0u, captured[0].scanTime);
        return 0;
    }));
    for (int window = 0; window < 3; window++) {
        auto *slot = PidSlotGetRef(100);
        PidMigrationWork(manager, slot, 100);
        EXPECT_EQ(window == 0 ? 200u : 3000u, captured[0].scanTime);
        EXPECT_EQ(window == 0 ? 10u : 1u, captured[0].nTimes);
    }
    EXPECT_TRUE(attr->adaptivePeriod.slow);
    EXPECT_TRUE(attr->adaptivePeriod.previousValid); // first new slow window only
}

TEST_F(PidPeriodTest, AutoSwitchIsIndependentOfFileSwitchAndOverridesFixedPeriods)
{
    adaptiveEnabled = true;
    fileEnabled = true;
    auto *attr = Add(100);
    UpdateProcessPeriods(attr);
    EXPECT_EQ(200u, captured[0].scanTime);
    EXPECT_EQ(2000u, captured[0].duration);
    adaptiveEnabled = false;
    UpdateProcessPeriods(attr);
    EXPECT_EQ(fileScan, captured[0].scanTime);
    EXPECT_EQ(fileMigrate, captured[0].duration);
}

TEST_F(PidPeriodTest, WorkerRestartsMultipleWindowsWithLiveEpollWithoutRelockingSlot)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    ASSERT_GE(epfd, 0);
    manager->eventLoop.epfd = epfd;
    int oldStop = EnvAtomicRead(&manager->eventLoop.stop);
    EnvAtomicSet(&manager->eventLoop.stop, 0);
    restartCount = 0;
    MOCKER(RestartPidScan).stubs().will(invoke(+[](pid_t) -> int {
        restartCount++;
        return 0;
    }));
    for (bool enabled : { false, true }) {
        adaptiveEnabled = enabled;
        pid_t pid = enabled ? 101 : 100;
        auto *attr = Add(pid);
        attr->state = PROC_MOVE; // isolate the period-update/restart tail of the worker
        auto *slot = PidSlotGetRef(pid);
        slot->memFreqFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        EXPECT_GE(slot->memFreqFd, 0);
        struct epoll_event event = {};
        event.events = EPOLLIN;
        event.data.ptr = slot;
        EXPECT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_ADD, slot->memFreqFd, &event));
        PidSlotReleaseRefs(&slot, 1);
        pid_t child = fork();
        EXPECT_GE(child, 0);
        if (child == 0) {
            // The old recursive lock is caught by SIGALRM instead of hanging the test suite.
            alarm(2);
            for (int window = 0; window < 2; window++) {
                auto *workSlot = PidSlotGetRef(pid);
                PidMigrationWork(manager, workSlot, pid);
                if (captured[0].pid != pid || captured[0].scanTime != 200) {
                    _exit(1);
                }
            }
            _exit(restartCount == 2 ? 0 : 1);
        }
        if (child > 0) {
            int status = 0;
            EXPECT_EQ(child, waitpid(child, &status, 0));
            EXPECT_EQ(0, status) << "worker blocked or failed before completing two scan restarts";
        }
    }
    EnvAtomicSet(&manager->eventLoop.stop, oldStop);
    manager->eventLoop.epfd = -1;
    close(epfd);
}
