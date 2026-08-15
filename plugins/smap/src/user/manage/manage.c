/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>

#include "smap_user_log.h"
#include "securec.h"
#include "device.h"
#include "access_ioctl.h"
#include "advanced-strategy/scene.h"
#include "smap_config.h"
#include "strategy/strategy_config.h"
#include "strategy/strategy.h"
#include "strategy/migration.h"
#include "manage.h"
#include "manage_internal.h"

#define UPDATE_CRITICAL_ERR_COUNT 5

static struct ProcessManager g_processManager;

static char g_mmapTypeName[][MMAP_TYPE_STRING_LEN] = { "mmap_private", "mmap_shared" };

uint32_t g_pageSizeNormal;
uint32_t g_pageSizeHuge;

EnvAtomic g_forbiddenNodes[MAX_NODES];
RunMode g_runMode;

uint8_t g_criticalErrNodes[REMOTE_NUMA_BITS];

RunMode GetRunMode(void)
{
    return g_runMode;
}

void SetRunMode(RunMode runMode)
{
    g_runMode = runMode;
}

uint32_t GetNormalPageSize(void)
{
    return g_pageSizeNormal;
}

uint32_t GetHugePageSize(void)
{
    return g_pageSizeHuge;
}

uint32_t GetPageSize(void)
{
    return g_processManager.tracking.pageSize;
}

static void RemoteNumaInfoInit(void)
{
    EnvMutexInit(&g_processManager.remoteNumaInfo.lock);
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        g_processManager.remoteNumaInfo.sharedSize[j] = 0;
        g_processManager.remoteNumaInfo.usedInfo[j].ifUsedFreshed = false;
        g_processManager.remoteNumaInfo.usedInfo[j].used = 0;
        g_processManager.remoteNumaInfo.usedInfo[j].size = 0;
        for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
            g_processManager.remoteNumaInfo.privateSize[i][j] = 0;
            g_processManager.remoteNumaInfo.privateUsedInfo[i][j].ifUsedFreshed = false;
            g_processManager.remoteNumaInfo.privateUsedInfo[i][j].used = 0;
            g_processManager.remoteNumaInfo.privateUsedInfo[i][j].size = 0;
        }
    }
}

int GetNrLocalNuma(void)
{
    return g_processManager.nrLocalNuma;
}

uint32_t BuildAllLocalNumaMask(void)
{
    int nrLocalNuma = GetNrLocalNuma();
    if (nrLocalNuma <= 0 || nrLocalNuma > LOCAL_NUMA_NUM) {
        return 0;
    }
    return (1U << nrLocalNuma) - 1U;
}

static uint32_t BuildResidentLocalMask(const ProcessAttr *attr)
{
    if (!attr) {
        return 0;
    }

    uint32_t residentLocalMask = 0;
    int nrLocalNuma = GetNrLocalNuma();
    for (int localNid = 0; localNid < nrLocalNuma && localNid < LOCAL_NUMA_NUM; localNid++) {
        if (attr->walkPage.nrPages[localNid] != 0) {
            AddL1(&residentLocalMask, localNid);
        }
    }
    return residentLocalMask;
}

uint32_t BuildAccountLocalMask(const ProcessAttr *attr, int remoteIndex)
{
    if (!attr || remoteIndex < 0 || remoteIndex >= REMOTE_NUMA_NUM) {
        return 0;
    }

    uint32_t accountLocalMask = 0;
    int nrLocalNuma = GetNrLocalNuma();
    for (int localNid = 0; localNid < nrLocalNuma && localNid < LOCAL_NUMA_NUM; localNid++) {
        if (attr->strategyAttr.remoteNrPagesAfterMigrate[localNid][remoteIndex] != 0) {
            AddL1(&accountLocalMask, localNid);
        }
    }
    return accountLocalMask;
}

int ApplyManagedLocalObservation(ProcessAttr *attr, const ManagedLocalObservation *observation, bool fullReplacement)
{
    if (!attr || !observation || (!observation->affinityValid && !observation->residentValid)) {
        return -EINVAL;
    }

    uint32_t allLocalMask = BuildAllLocalNumaMask();
    if (allLocalMask == 0) {
        SMAP_LOGGER_ERROR("Invalid local NUMA layout for pid %d.", attr->pid);
        return -EINVAL;
    }

    ManagedLocalState state = attr->managedLocalState;
    state.residentLocalMask = observation->residentValid ? observation->residentLocalMask & allLocalMask : 0;
    if (observation->affinitySampled) {
        state.affinityRefreshElapsedMs = 0;
        state.affinitySampled = true;
    }
    if (observation->affinityValid) {
        state.affinityLocalMask = observation->affinityLocalMask & allLocalMask;
        state.affinityValid = true;
    }
    state.observedLocalMask = (state.affinityValid ? state.affinityLocalMask : 0) | state.residentLocalMask;
    if (state.observedLocalMask == 0) {
        state.observedLocalMask = allLocalMask;
    }

    uint32_t allAccountLocalMask = 0;
    for (int remoteIndex = 0; remoteIndex < REMOTE_NUMA_NUM; remoteIndex++) {
        state.accountLocalMask[remoteIndex] = BuildAccountLocalMask(attr, remoteIndex);
        allAccountLocalMask |= state.accountLocalMask[remoteIndex];
    }

    uint32_t managedLocalMask = state.observedLocalMask | allAccountLocalMask;
    if (!fullReplacement) {
        managedLocalMask |= attr->managedLocalState.managedLocalMask;
    }
    state.managedLocalMask = managedLocalMask & allLocalMask;
    attr->managedLocalState = state;

    SMAP_LOGGER_DEBUG("Refresh pid %d local state: managed=%#x observed=%#x "
                      "resident=%#x account=%#x full=%d.",
                      attr->pid, state.managedLocalMask, state.observedLocalMask, state.residentLocalMask,
                      allAccountLocalMask, fullReplacement);
    return 0;
}

static uint32_t GetManagedLocalRefreshPeriodMs(void)
{
    if (g_processManager.migPeriod != 0) {
        return g_processManager.migPeriod;
    }
    return DEFAULT_MIGRATE_PERIOD;
}

static void AdvanceManagedLocalAffinityRefresh(ProcessAttr *attr)
{
    uint32_t elapsed = attr->managedLocalState.affinityRefreshElapsedMs;
    uint32_t period = GetManagedLocalRefreshPeriodMs();
    if (elapsed >= MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS ||
        period >= MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS - elapsed) {
        attr->managedLocalState.affinityRefreshElapsedMs = MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS;
        return;
    }
    attr->managedLocalState.affinityRefreshElapsedMs = elapsed + period;
}

int RefreshManagedLocalState(ProcessAttr *attr, bool fullReplacement)
{
    if (!attr) {
        return -EINVAL;
    }

    ManagedLocalObservation observation = {
        .residentLocalMask = BuildResidentLocalMask(attr),
        /*
         * RefreshManagedLocalState is called only after a page snapshot has
         * been filled. An empty resident mask is therefore a valid
         * observation, not an unavailable data source.
         */
        .residentValid = true,
    };

    if (!fullReplacement) {
        AdvanceManagedLocalAffinityRefresh(attr);
    }
    bool refreshAffinity = fullReplacement || !attr->managedLocalState.affinitySampled ||
                           attr->managedLocalState.affinityRefreshElapsedMs >=
                               MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS;
    if (refreshAffinity) {
        int ret = SetLocalNumaByCpu(attr->pid, &observation.affinityLocalMask);
        observation.affinitySampled = true;
        /*
         * Reset the elapsed time on both success and failure. A failed
         * sample keeps the last valid affinity mask and is retried after
         * the normal interval instead of on every migration cycle.
         */
        if (ret) {
            SMAP_LOGGER_WARNING("Refresh pid %d affinity local NUMA failed: %d.", attr->pid, ret);
        } else {
            observation.affinityValid = true;
        }
    } else if (attr->managedLocalState.affinityValid) {
        observation.affinityLocalMask = attr->managedLocalState.affinityLocalMask;
        observation.affinityValid = true;
    }

    return ApplyManagedLocalObservation(attr, &observation, fullReplacement);
}

uint32_t BuildManagedTrackingNodes(const ProcessAttr *attr)
{
    if (!attr) {
        return 0;
    }

    uint32_t allLocalMask = BuildAllLocalNumaMask();
    if (allLocalMask == 0) {
        return 0;
    }

    /*
     * Rebuild the remote tracking scope from the current target and resident
     * page state. An omitted remote must remain tracked while it still owns
     * pages, but stale bits must not survive after reconciliation reaches zero.
     */
    uint32_t localBitmapMask = (1U << LOCAL_NUMA_BITS) - 1U;
    /* A zero total-page count means no fresh pagemap snapshot is available. */
    bool pageSnapshotValid = attr->walkPage.nrPage != 0;
    uint32_t numaNodes = pageSnapshotValid ? 0 : (attr->numaAttr.numaNodes & ~localBitmapMask);
    numaNodes |= attr->managedLocalState.managedLocalMask & allLocalMask;

    uint32_t targetCount = attr->targetConfig.count;
    if (targetCount > REMOTE_NUMA_NUM) {
        SMAP_LOGGER_WARNING("Pid %d target count %u exceeds limit.", attr->pid, targetCount);
        targetCount = REMOTE_NUMA_NUM;
    }
    for (uint32_t i = 0; i < targetCount; i++) {
        int remoteIndex;
        int remoteNid = attr->targetConfig.targets[i].remoteNid;
        if (RemoteNidToIndex(remoteNid, GetNrLocalNuma(), &remoteIndex) == 0) {
            AddL2ByNid(&numaNodes, remoteNid);
        }
    }
    for (int remoteIndex = 0; remoteIndex < REMOTE_NUMA_NUM; remoteIndex++) {
        int remoteNid = GetNrLocalNuma() + remoteIndex;
        bool hasResidentPages = remoteNid < MAX_NODES && attr->walkPage.nrPages[remoteNid] != 0;
        bool hasAccount = attr->managedLocalState.accountLocalMask[remoteIndex] != 0;
        if (!hasResidentPages && !hasAccount) {
            continue;
        }
        AddL2ByNid(&numaNodes, remoteNid);
    }
    return numaNodes;
}

/*
 * Validate that nid belongs to the configured remote NUMA id range. This is a
 * range check only; callers that require online-node validation should do that
 * separately.
 */
bool IsRemoteNidValid(int nid)
{
    struct ProcessManager *manager = GetProcessManager();
    if (!manager) {
        SMAP_LOGGER_ERROR("process manager is null.");
        return false;
    }

    return nid >= manager->nrLocalNuma && nid < (REMOTE_NUMA_BITS + manager->nrLocalNuma);
}

void UpdateRemoteNumaCriticalErr(void)
{
    static uint8_t count = 0;
    if (count < UPDATE_CRITICAL_ERR_COUNT) {
        count++;
        return;
    }

    int nrLocalNuma = GetNrLocalNuma();
    int maxNid = nrLocalNuma + REMOTE_NUMA_BITS;
    for (int nid = nrLocalNuma; nid < maxNid; nid++) {
        g_criticalErrNodes[nid - nrLocalNuma] = IsNumaCriticalErr(nid) ? 1 : 0;
        SMAP_LOGGER_DEBUG("Update remote numa critical error: nid=%d, critical=%d.", nid,
                          g_criticalErrNodes[nid - nrLocalNuma]);
    }
    count = 0;
}

bool IsRemoteNumaCriticalErr(int nid)
{
    int nrLocalNuma = GetNrLocalNuma();
    if (nid < nrLocalNuma || nid >= (nrLocalNuma + REMOTE_NUMA_BITS)) {
        return false;
    }
    return g_criticalErrNodes[nid - nrLocalNuma] == 1;
}

int ProcessManagerInit(uint32_t pageType)
{
    int i;
    int ret = memset_s(&g_processManager, sizeof(struct ProcessManager), 0, sizeof(struct ProcessManager));
    if (ret != EOK) {
        SMAP_LOGGER_ERROR("Clear process manager memory failed: %d.", ret);
        return -ret;
    }
    ret = memset_s(g_criticalErrNodes, sizeof(g_criticalErrNodes), 0, sizeof(g_criticalErrNodes));
    if (ret != EOK) {
        SMAP_LOGGER_ERROR("Clear critical error nodes memory failed: %d.", ret);
        return -ret;
    }
    ret = GenerateStrategyConfigFile(STRATEGY_CONFIG_PATH);
    if (ret != 0) {
        SMAP_LOGGER_ERROR("Generate strategy config file failed, ret is %d.", ret);
    }
    StrategyConfigRead(STRATEGY_CONFIG_PATH);
    int size = sysconf(_SC_PAGESIZE);
    if (size != PAGESIZE_4K && size != PAGESIZE_64K) {
        SMAP_LOGGER_ERROR("Get pagesize failed.");
        return -EINVAL;
    }
    g_pageSizeNormal = size;
    g_pageSizeHuge = PAGESIZE_2M;
    g_processManager.tracking.pageSize = (pageType == PAGETYPE_NORMAL) ? g_pageSizeNormal : g_pageSizeHuge;
    g_processManager.migPeriod = IsHugeMode() ? LIGHT_STABLE_MIGRATE_CYCLE : PROCESS_LIGHT_STABLE_MIGRATE_CYCLE;
    if (GetFileConfSwitchConfig()) {
        g_processManager.migPeriod = GetMigratePeriodConfig();
    }

    for (i = 0; i < MAX_NODES; i++) {
        g_processManager.fds.nodes[i] = DEFAULT_FD;
    }
    g_processManager.fds.migrate = DEFAULT_FD;
    g_processManager.fds.access = DEFAULT_FD;
    g_processManager.fds.lock = DEFAULT_FD;
    g_processManager.ubBwMonitor.ubBwThreshold = GetUbBwThresholdConfig();
    g_processManager.ubBwMonitor.currentFluxRet = -ENODATA;
    RemoteNumaInfoInit();
    PidSlotInit(&g_processManager);
    EnvMutexInit(&g_processManager.threadLock);
    InitSceneInfo(&g_processManager.sceneInfo, (PageType)pageType);
    g_runMode = WATERLINE_MODE;
    return 0;
}

int LoadMangerNrProcessNum(void)
{
    return g_processManager.nr[PROCESS_TYPE];
}

int LoadMangerNrVmNum(void)
{
    return g_processManager.nr[VM_TYPE];
}

bool PidIsValid(pid_t pid)
{
    char path[32];
    int ret = snprintf_s(path, sizeof(path), sizeof(path), "/proc/%d", pid);
    if (ret == -1) {
        return false;
    }
    return access(path, F_OK) == 0;
}

int GetPidTypeFromComm(pid_t pid)
{
    char comm[BUFFER_SIZE];
    char cmdBuf[BUFFER_SIZE];
    int ret = snprintf_s(cmdBuf, sizeof(cmdBuf), sizeof(cmdBuf) - 1, "%s %d comm %s", CAT_SCRIPT_CAT_PATH, pid,
                         CAT_SCRIPT_TAIL);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Failed to generate cmd string, ret is %d.", ret);
        return -EINVAL;
    }
    SMAP_LOGGER_INFO("Before open comm file");
    FILE *file = popen(cmdBuf, "r");
    if (!file) {
        SMAP_LOGGER_ERROR("Failed to open file, errno is %d.", errno);
        return -EINVAL;
    }
    bool foundLine = false;
    while (fgets(comm, sizeof(comm), file) != NULL) {
        foundLine = true;
        if ((strncmp(comm, VM_NAME_STR, PID_NAME_LEN) == 0) ||
            (strncmp(comm, VM_KVM_NAME_STR, PID_KVM_NAME_LEN) == 0)) {
            pclose(file);
            return VM_TYPE;
        }
    }
    SMAP_LOGGER_ERROR("Error occur in fgets comm file");
    (void)pclose(file);
    if (foundLine) {
        return PROCESS_TYPE;
    }
    return -1;
}

static int AtomicFetchAdd(EnvAtomic *a, int v)
{
    int oldv;
    int newv;
    do {
        oldv = EnvAtomicRead(a);
        newv = oldv + v;
    } while (EnvAtomicCmpAndSwap(oldv, newv, a) != oldv);
    return oldv;
}

static int AtomicFetchSub(EnvAtomic *a, int v)
{
    int oldv;
    int newv;
    do {
        oldv = EnvAtomicRead(a);
        newv = oldv - v;
    } while (EnvAtomicCmpAndSwap(oldv, newv, a) != oldv);
    return oldv;
}

void ResetActcData(ActcData *actcData[], int len)
{
    /* actcData[nid]按偏移指向同一连续缓冲区，第一个非空指针即缓冲区起始，仅释放一次 */
    for (int i = 0; i < len; i++) {
        if (actcData[i] != NULL) {
            free(actcData[i]);
            break;
        }
    }
    for (int i = 0; i < len; i++) {
        actcData[i] = NULL;
    }
}

static void FreeProceccesAttr(ProcessAttr *attr)
{
    if (attr == NULL) {
        return;
    }
    if (attr->scanAttr.actcData) {
        ResetActcData(attr->scanAttr.actcData, MAX_NODES);
    }
    free(attr);
}

void PidSlotInit(struct ProcessManager *manager)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        EnvAtomicSet(&manager->slots[i].state, PID_SLOT_FREE);
        manager->slots[i].pid = 0;
        manager->slots[i].attr = NULL;
        EnvAtomicSet(&manager->slots[i].refs, 0);
        EnvMutexInit(&manager->slots[i].attrLock);
    }
}

void PidSlotDestroy(struct ProcessManager *manager)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        EnvMutexDestroy(&manager->slots[i].attrLock);
    }
}

/* 引用归零后唯一清理者：释放 attr 并把槽位恢复为 FREE */
static void PidSlotTryReclaim(struct ProcessManager *manager, int i)
{
    struct PidSlot *s = &manager->slots[i];
    if (EnvAtomicRead(&s->refs) != 0) {
        return;
    }
    if (EnvAtomicCmpAndSwap(PID_SLOT_REMOVING, PID_SLOT_FREE, &s->state) == PID_SLOT_REMOVING) {
        FreeProceccesAttr(s->attr);
        s->attr = NULL;
        s->pid = 0;
    }
}

int PidSlotAdd(struct ProcessManager *manager, ProcessAttr *attr)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        struct PidSlot *s = &manager->slots[i];
        if (EnvAtomicCmpAndSwap(PID_SLOT_FREE, PID_SLOT_RESERVED, &s->state) != PID_SLOT_FREE) {
            continue;
        }
        s->pid = attr->pid;
        s->attr = attr;
        EnvAtomicSet(&s->refs, 1);
        EnvAtomicSet(&s->state, PID_SLOT_INUSE);
        return i;
    }
    return -1;
}

void PidSlotRemove(struct ProcessManager *manager, pid_t pid)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        struct PidSlot *s = &manager->slots[i];
        if (s->pid != pid) {
            continue;
        }
        if (EnvAtomicCmpAndSwap(PID_SLOT_INUSE, PID_SLOT_REMOVING, &s->state) != PID_SLOT_INUSE) {
            return;
        }
        if (AtomicFetchSub(&s->refs, 1) == 1) {
            PidSlotTryReclaim(manager, i);
        }
        return;
    }
}

bool PidSlotEmpty(struct ProcessManager *manager)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        if (EnvAtomicRead(&manager->slots[i].state) == PID_SLOT_INUSE) {
            return false;
        }
    }
    return true;
}

struct PidSlot *PidSlotGetRef(pid_t pid)
{
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        struct PidSlot *s = &g_processManager.slots[i];
        if (EnvAtomicRead(&s->state) != PID_SLOT_INUSE || s->pid != pid) {
            continue;
        }
        AtomicFetchAdd(&s->refs, 1);
        if (EnvAtomicRead(&s->state) != PID_SLOT_INUSE) {
            if (AtomicFetchSub(&s->refs, 1) == 1) {
                PidSlotTryReclaim(&g_processManager, i);
            }
            continue;
        }
        return s;
    }
    return NULL;
}

size_t PidSlotCollectRefs(struct ProcessManager *manager, struct PidSlot *arr[], size_t cap)
{
    size_t count = 0;
    for (int i = 0; i < MAX_PID_SLOTS && count < cap; i++) {
        struct PidSlot *s = &manager->slots[i];
        if (EnvAtomicRead(&s->state) != PID_SLOT_INUSE) {
            continue;
        }
        AtomicFetchAdd(&s->refs, 1);
        if (EnvAtomicRead(&s->state) != PID_SLOT_INUSE) {
            if (AtomicFetchSub(&s->refs, 1) == 1) {
                PidSlotTryReclaim(manager, i);
            }
            continue;
        }
        arr[count++] = s;
    }
    return count;
}

void PidSlotReleaseRefs(struct PidSlot *arr[], size_t n)
{
    for (size_t k = 0; k < n; k++) {
        struct PidSlot *s = arr[k];
        if (s == NULL) {
            continue;
        }
        if (AtomicFetchSub(&s->refs, 1) == 1) {
            PidSlotTryReclaim(&g_processManager, (int)(s - g_processManager.slots));
        }
    }
}

void PutProcessAttr(ProcessAttr *attr)
{
    if (attr == NULL) {
        return;
    }
    for (int i = 0; i < MAX_PID_SLOTS; i++) {
        struct PidSlot *s = &g_processManager.slots[i];
        if (s->attr != attr) {
            continue;
        }
        if (AtomicFetchSub(&s->refs, 1) == 1) {
            PidSlotTryReclaim(&g_processManager, i);
        }
        return;
    }
}

static unsigned long ProcessSmapsFile(pid_t pid, const char *targetLinePrefix, size_t prefixLength, size_t divisor)
{
    char filename[BUFFER_SIZE];
    int ret = snprintf_s(filename, sizeof(filename), sizeof(filename), "/proc/%d/smaps", pid);
    if (ret == -1) {
        return 0;
    }

    FILE *file = fopen(filename, "r");
    if (!file) {
        SMAP_LOGGER_ERROR("fopen /proc/%d/smaps failed.", pid);
        return 0;
    }

    char line[BUFFER_SIZE];
    unsigned long totalPages = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, targetLinePrefix, prefixLength) == 0) {
            unsigned long value;
            if (sscanf_s(line + prefixLength, "%lu", &value) != 1) {
                continue;
            }
            totalPages += value * KIB / divisor; // KB to pages
        }
    }

    ret = fclose(file);
    if (ret) {
        SMAP_LOGGER_ERROR("close smaps failed: %d.", ret);
    }
    return totalPages;
}

static unsigned long GetNormalPageCount(pid_t pid)
{
    return ProcessSmapsFile(pid, RSS_LINE_PREFIX, RSS_LINE_PREFIX_LENGTH, g_pageSizeNormal);
}

static unsigned long GetHugePageCount(pid_t pid)
{
    return ProcessSmapsFile(pid, HUGETLB_LINE_PREFIX, HUGETLB_LINE_PREFIX_LENGTH, g_pageSizeHuge);
}

unsigned long GetPidNrPages(pid_t pid)
{
    return (g_processManager.tracking.pageSize == g_pageSizeHuge) ? GetHugePageCount(pid) : GetNormalPageCount(pid);
}

static int GetNodeFromCpu(int cpu)
{
    int ret;
    char path[BUFFER_SIZE];
    for (int node = 0; node < MAX_NODES; node++) {
        ret = snprintf_s(path, sizeof(path), sizeof(path), CPU_NUMA_PATH, cpu, node);
        if (ret == -1) {
            return -EINVAL;
        }
        if (access(path, F_OK) == 0) {
            return node;
        }
    }
    SMAP_LOGGER_ERROR("open cpu %d node failed.", cpu);
    return -EINVAL;
}

int GetNumaNodesForPid(pid_t pid, int *node)
{
    int ret;
    int cpuNode;
    cpu_set_t mask;
    int i;

    CPU_ZERO(&mask);
    ret = sched_getaffinity(pid, sizeof(cpu_set_t), &mask);
    if (ret) {
        SMAP_LOGGER_ERROR("pid %d sched_getaffinity failed: %d.", pid, ret);
        return -EINVAL;
    }
    for (i = 0; i < sizeof(cpu_set_t) * BIT_TO_BYTE; i++) {
        if (CPU_ISSET(i, &mask)) {
            cpuNode = GetNodeFromCpu(i);
            if (cpuNode == -EINVAL) {
                SMAP_LOGGER_ERROR("pid % get node from cpu failed: %d.", pid, ret);
                return -EINVAL;
            }
            *node = cpuNode;
            break;
        }
    }
    return 0;
}

bool IsHugeMode(void)
{
    return g_processManager.tracking.pageSize == g_pageSizeHuge;
}

bool IsHugeAligned(uint64_t addr)
{
    return (addr & (g_pageSizeHuge - 1)) == 0;
}

int IsHugePageRange(const char *line)
{
    return strstr(line, "hugepage") != NULL;
}

/*
 * 校验 pid 有效并返回其身份(VM_TYPE/PROCESS_TYPE)，失败返回 -ESRCH/-EINVAL。
 * 身份由 GetPidTypeFromComm 自动判别，不再由 caller 声明——公共 API 仅传 pageType(页大小)。
 */
int DetectPidType(pid_t pid)
{
    if (!PidIsValid(pid)) {
        SMAP_LOGGER_ERROR("Input pid %d is invalid.", pid);
        return -ESRCH;
    }
    int ret = GetPidTypeFromComm(pid);
    if (ret != VM_TYPE && ret != PROCESS_TYPE) {
        SMAP_LOGGER_ERROR("Pid %d type detect failed: %d.", pid, ret);
        return -EINVAL;
    }
    return ret;
}

/* 取引用：返回 attr（持 1 引用）；用完须 PutProcessAttr 释放。未找到返回 NULL */
ProcessAttr *GetProcessAttr(pid_t pid)
{
    struct PidSlot *s = PidSlotGetRef(pid);
    return PidSlotAttr(s);
}

int ReadCmdlineByPid(pid_t pid, char *buf, int len)
{
    char cmdBuf[BUFFER_SIZE];
    char skip[BUFFER_SIZE];
    int ret = snprintf_s(cmdBuf, sizeof(cmdBuf), sizeof(cmdBuf) - 1, "%s %d cmdline %s", CAT_SCRIPT_CAT_PATH, pid,
                         CAT_SCRIPT_TAIL);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Make pid %d cmdline cmd error.", pid);
        return -EINVAL;
    }
    FILE *file = popen(cmdBuf, "r");
    if (file == NULL) {
        SMAP_LOGGER_ERROR("Open pid %d cmdline error: %d.", pid, errno);
        return -errno;
    }
    if (fgets(skip, sizeof(skip), file) == NULL) {
        (void)pclose(file);
        SMAP_LOGGER_ERROR("Read pid %d cmdline skip-line failed.", pid);
        return -EIO;
    }
    if (fgets(buf, len, file) == NULL) {
        (void)pclose(file);
        SMAP_LOGGER_ERROR("Read pid %d cmdline content failed.", pid);
        return -EIO;
    }
    (void)pclose(file);
    /* /proc/<pid>/cmdline 不受 4096 字节限制: 大虚机参数可能更长。缓冲填满说明被截断,
     * share 标志可能落在窗口外导致误判 PRIVATE。保守返回错误, 由 ParseMmapType 置 SHARED。 */
    if (strlen(buf) >= (size_t)len - 1) {
        SMAP_LOGGER_ERROR("Pid %d cmdline exceeds %d bytes, may be truncated, default mmap_shared.", pid, len);
        return -E2BIG;
    }
    return 0;
}

/*
 * libvirt 生成的 QEMU cmdline 旧式为 -object memory-backend-file,...,share=on，
 * 新式为 JSON -object {"qom-type":"memory-backend-file",...,"share":true}；两者都匹配。
 * 与 libvirt domain XML 的 memAccess='shared'
 */
static int CmdlineHasSharedMem(const char *cmdline, int len)
{
    const char *tmp = cmdline;
    while (tmp != NULL && *tmp != '\0') {
        if (strstr(tmp, "\"share\":true") != NULL || strstr(tmp, "share=on") != NULL) {
            return 1;
        }
        tmp = strchr(tmp, '\0');
        if (tmp == NULL || tmp >= cmdline + len) {
            break;
        }
        tmp++;
    }
    return 0;
}

/*
 * 判定虚机内存映射 SHARED/PRIVATE，替代原 libvirt domain XML 路径。
 * 读 cmdline 失败时保守置 SHARED(与旧 libvirt no-xml 语义一致：失败倾向单线程迁出)。
 */
int ParseMmapType(pid_t pid, MmapType *mmapType)
{
    char cmdline[CMDLINE_LEN] = { 0 };
    int ret = ReadCmdlineByPid(pid, cmdline, sizeof(cmdline));
    if (ret) {
        *mmapType = MMAP_SHARED;
        SMAP_LOGGER_ERROR("Read cmdline of pid %d failed: %d, default mmap_shared.", pid, ret);
        return -EINVAL;
    }
    if (CmdlineHasSharedMem(cmdline, sizeof(cmdline))) {
        *mmapType = MMAP_SHARED;
    } else {
        *mmapType = MMAP_PARIVATE;
    }
    SMAP_LOGGER_INFO("Read Mmap type of pid %d: %s.", pid, g_mmapTypeName[*mmapType]);
    return 0;
}

int VMPreprocess(pid_t pid, ProcessAttr *attr)
{
    if (attr->type == VM_TYPE) {
        int ret = ParseMmapType(pid, &attr->vmPidAttr.mmapType);
        if (ret) {
            SMAP_LOGGER_ERROR("Parse mmap type of pid %d failed.", pid);
            return 0;
        }
    }
    return 0;
}

/* Set process attributes that are independent of its migration target. */
void SetBasicProcessConfig(ProcessAttr *attr, ProcessParam *param)
{
    attr->pid = param->pid;
    attr->duration = param->duration;
    attr->scanType = param->scanType;
    attr->isFirstScan = true;
    attr->enableSwap = true;

    if (time(&attr->scanStart) == (time_t)-1) {
        SMAP_LOGGER_ERROR("get time error");
    }

    int localNumaCnt = GetL1Count(attr->numaAttr.numaNodes);
    SMAP_LOGGER_INFO("attr->scanStart time: %s", ctime(&attr->scanStart));
    SMAP_LOGGER_INFO("Pid: %d local numa cnt: %d, remote numa cnt: %d.", attr->pid, localNumaCnt, attr->remoteNumaCnt);
}

void SetMultiNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma)
{
    for (int i = 0; i < param->count; i++) {
        int remoteNid = param->numaParam[i].nid;
        int l2Index = remoteNid - nrLocalNuma;

        attr->migrateParam[i].nid = remoteNid;
        attr->migrateParam[i].memSize = param->numaParam[i].memSize;
        SMAP_LOGGER_INFO("Multi-NUMA config destNid: %d, memSize: %lu", remoteNid, attr->migrateParam[i].memSize);

        /* Set the same ratio for all local NUMAs */
        for (int j = 0; j < nrLocalNuma && j < LOCAL_NUMA_NUM; j++) {
            attr->strategyAttr.initRemoteMemRatio[j][l2Index] = param->numaParam[i].ratio;
            SMAP_LOGGER_INFO("Multi-NUMA config destNid: %d, ratio: %d", remoteNid, param->numaParam[i].ratio);
        }
        AddAttrL2(attr, remoteNid);
    }
}

int AddProcess(ProcessParam *param, PidType type, uint32_t *nodeBitmap)
{
    int ret;
    (void)nodeBitmap;
    if (g_processManager.nr[VM_TYPE] + g_processManager.nr[PROCESS_TYPE] >= GetCurrentMaxNrPid()) {
        SMAP_LOGGER_ERROR("nr of pid is out of limit.");
        return -EINVAL;
    }

    ProcessAttr *attr = calloc(1, sizeof(ProcessAttr));
    if (!attr) {
        SMAP_LOGGER_ERROR("Alloc memory for process failed.");
        return -ENOMEM;
    }
    InitProcessMigrationTargetState(attr);
    attr->type = type;

    if (param->scanType == NORMAL_SCAN) {
        ret = VMPreprocess(param->pid, attr);
        if (ret) {
            SMAP_LOGGER_ERROR("Preprocess VM process %d attribute failed, return code: %d.", param->pid, ret);
            free(attr);
            return ret;
        }
    } else if (param->scanType == HAM_SCAN || param->scanType == STATISTIC_SCAN) {
        attr->state = PROC_MOVE;
        SMAP_LOGGER_INFO("Set pid %d state to %d.", param->pid, PROC_MOVE);
    }

    ret = SetProcessConfig(attr, param);
    if (ret) {
        SMAP_LOGGER_ERROR("Set process %d config failed: %d.", param->pid, ret);
        free(attr);
        return ret;
    }
    attr->scanTime = DEFAULT_SCAN_PERIOD;
    PidSlotAdd(&g_processManager, attr);
    SMAP_LOGGER_INFO("Set pid %d scan cycle to %ums.", attr->pid, attr->scanTime);
    g_processManager.nr[type]++;

    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", param->pid, ret);
    }
    SMAP_LOGGER_INFO("Add pid:%d success! localMemRatio:%d, migrateMode: %d.", param->pid, attr->initLocalMemRatio,
                     attr->migrateMode);

    return 0;
}

void DiscardProcessManageCandidate(ProcessManageCandidate *candidate)
{
    if (!candidate) {
        return;
    }

    PutProcessAttr(candidate->active);
    free(candidate->prepared);
    *candidate = (ProcessManageCandidate){ 0 };
}

/* Sum the configured migrate memSize (KB) of an already-managed pid. */
static uint64_t SumAttrMigrateMemSize(const ProcessAttr *attr)
{
    uint64_t total = 0;
    for (int i = 0; i < attr->remoteNumaCnt; i++) {
        total += attr->migrateParam[i].memSize;
    }
    return total;
}

int PrepareProcessManageCandidate(ProcessParam *param, PidType type, ProcessManageCandidate *candidate)
{
    if (!param || !candidate) {
        return -EINVAL;
    }
    *candidate = (ProcessManageCandidate){ 0 };

    ProcessTargetConfig config;
    int ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        return ret;
    }
    ret = DetectPidType(param->pid);
    if (ret < 0) {
        return ret;
    }

    ProcessAttr *active = GetProcessAttr(param->pid);
    ProcessAttr *prepared = NULL;
    if (active) {
        prepared = malloc(sizeof(ProcessAttr));
        if (!prepared) {
            PutProcessAttr(active);
            return -ENOMEM;
        }
        *prepared = *active;
    } else {
        if (g_processManager.nr[VM_TYPE] + g_processManager.nr[PROCESS_TYPE] >= GetCurrentMaxNrPid()) {
            SMAP_LOGGER_ERROR("nr of pid is out of limit.");
            return -EINVAL;
        }
        prepared = calloc(1, sizeof(ProcessAttr));
        if (!prepared) {
            return -ENOMEM;
        }
        InitProcessMigrationTargetState(prepared);
        prepared->pid = param->pid;
        prepared->type = type;
        if (param->scanType == NORMAL_SCAN) {
            ret = VMPreprocess(param->pid, prepared);
            if (ret) {
                free(prepared);
                return ret;
            }
        } else if (param->scanType == HAM_SCAN || param->scanType == STATISTIC_SCAN) {
            prepared->state = PROC_MOVE;
        }
        SetBasicProcessConfig(prepared, param);
    }

    candidate->active = active;
    candidate->prepared = prepared;
    candidate->isNew = active == NULL;
    candidate->isPending = active && active->state == PROC_MIGRATE;
    /* memSize 增大时重置 isFirstScan，使下一轮高频扫描快速迁移新增需求 */
    uint64_t oldMemSize = active ? SumAttrMigrateMemSize(active) : 0;
    ret = ConfigureMigrationTargetsWithCapacityPolicy(prepared, &config, param->ignoreRemoteCapacity);
    if (ret) {
        DiscardProcessManageCandidate(candidate);
        return ret;
    }
    if (active && !IsHugeMode()) {
        uint64_t newMemSize = SumAttrMigrateMemSize(prepared);
        if (newMemSize > oldMemSize) {
            /* memSize 增大时直接设置扫描周期为 DEFAULT_SCAN_PERIOD 立即生效，
             * 同时置 isFirstScan 防止正常扫描周期更新逻辑立即重置。
             * 设到 prepared 上，由 TrackMigrateOutCandidates 读取
             * prepared->scanTime 下发内核态，再由 PublishProcessTargetCandidate
             * 传播到 active。
             * 仅 4K 场景生效：2M 虚机场景扫描周期由内核态管理，无需用户态重置。
             */
            prepared->isFirstScan = true;
            prepared->scanTime = DEFAULT_SCAN_PERIOD;
            SMAP_LOGGER_INFO("pid %d memSize increased %lu->%lu, reset scan period to %ums.", prepared->pid, oldMemSize,
                             newMemSize, DEFAULT_SCAN_PERIOD);
        }
    }
    if (candidate->isNew) {
        prepared->scanTime = DEFAULT_SCAN_PERIOD;
    }
    return 0;
}

void PublishProcessManageCandidate(ProcessManageCandidate *candidate)
{
    if (!candidate || !candidate->prepared) {
        return;
    }

    ProcessAttr *prepared = candidate->prepared;
    if (candidate->isPending) {
        candidate->active->pendingTargetConfig = prepared->pendingTargetConfig;
        candidate->active->pendingTargetConfigValid = prepared->pendingTargetConfigValid;
        candidate->active->pendingIgnoreRemoteCapacity = prepared->pendingIgnoreRemoteCapacity;
        candidate->active->pendingTargetNumaNodes = prepared->pendingTargetNumaNodes;
        int ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize pending pid %d config maybe failed: %d.", prepared->pid, ret);
        }
        SMAP_LOGGER_INFO("Stage pid %d migration target update.", prepared->pid);
        DiscardProcessManageCandidate(candidate);
        return;
    }

    if (candidate->isNew) {
        PidSlotAdd(&g_processManager, prepared);
        g_processManager.nr[prepared->type]++;
        candidate->prepared = NULL;
        SMAP_LOGGER_INFO("Add pid %d to list done.", prepared->pid);
    } else {
        PublishProcessTargetCandidate(candidate->active, prepared);
        SMAP_LOGGER_INFO("Update pid %d migrate config.", prepared->pid);
    }

    int ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", prepared->pid, ret);
    }
    DiscardProcessManageCandidate(candidate);
}

int SetLocalNumaByCpu(pid_t pid, uint32_t *nodeBitmap)
{
    int ret;
    int nid;
    cpu_set_t mask;

    if (!nodeBitmap) {
        SMAP_LOGGER_ERROR("Get pid %d nodeBitmap is null", pid);
        return -EINVAL;
    }

    CPU_ZERO(&mask);
    ret = sched_getaffinity(pid, sizeof(cpu_set_t), &mask);
    if (ret) {
        SMAP_LOGGER_ERROR("Get pid %d sched affinity failed: %d.", pid, ret);
        return -EINVAL;
    }
    for (int i = 0; i < sizeof(cpu_set_t) * BIT_TO_BYTE; i++) {
        if (!CPU_ISSET(i, &mask)) {
            continue;
        }
        nid = GetNodeFromCpu(i);
        if (nid == -EINVAL) {
            SMAP_LOGGER_ERROR("Get node from cpu%d failed: %d.", i, ret);
            return -EINVAL;
        }
        AddL1(nodeBitmap, nid);
    }
    return 0;
}

FILE *OpenNumaMaps(pid_t pid)
{
    char cmdBuf[BUFFER_SIZE];
    int ret = snprintf_s(cmdBuf, sizeof(cmdBuf), sizeof(cmdBuf) - 1, "%s %d numa_maps %s", CAT_SCRIPT_CAT_PATH, pid,
                         CAT_SCRIPT_TAIL);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("OpenNumaMaps for pid %d err.", pid);
        return NULL;
    }
    FILE *fp = popen(cmdBuf, "r");
    if (!fp) {
        SMAP_LOGGER_ERROR("OpenNumaMaps fopen failed: %d.", -errno);
    }
    return fp;
}

static int AddNumaPagesFromLine(char *line, uint64_t numaPages[MAX_NODES])
{
    char pattern[NUMA_MAPS_MAX_PATTERN_LEN];

    for (int nid = 0; nid < MAX_NODES; nid++) {
        int ret = snprintf_s(pattern, sizeof(pattern), sizeof(pattern) - 1, " N%d=", nid);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Set numa maps pattern failed, nid %d.", nid);
            return -EINVAL;
        }

        char *substr = strstr(line, pattern);
        if (!substr) {
            continue;
        }

        char *value = substr + strlen(pattern);
        char *end = NULL;
        errno = 0;
        uint64_t pages = strtoull(value, &end, 10);
        if (value == end || errno == ERANGE || UINT64_MAX - numaPages[nid] < pages) {
            SMAP_LOGGER_ERROR("Parse numa maps pages failed, nid %d, line %s.", nid, line);
            return -EINVAL;
        }
        numaPages[nid] += pages;
    }
    return 0;
}

int GetPidNumaPagesFromNumaMaps(pid_t pid, uint64_t numaPages[MAX_NODES], bool onlyHuge)
{
    char line[MAX_LINE_LENGTH];
    FILE *fp = OpenNumaMaps(pid);
    if (!fp) {
        SMAP_LOGGER_ERROR("Open pid %d numa maps failed.", pid);
        return -EINVAL;
    }

    int ret = 0;
    while (fgets(line, MAX_LINE_LENGTH, fp) != NULL) {
        if (onlyHuge && !IsNumaMapLineHuge(line)) {
            continue;
        }
        ret = AddNumaPagesFromLine(line, numaPages);
        if (ret) {
            break;
        }
    }
    if (pclose(fp)) {
        SMAP_LOGGER_WARNING("Close numa maps failed, pid=%d.", pid);
    }
    return ret;
}

bool IsPidUsingHugePages(pid_t pid)
{
    char line[MAX_LINE_LENGTH];
    FILE *fp = OpenNumaMaps(pid);
    if (!fp) {
        SMAP_LOGGER_ERROR("Open pid %d numa maps failed when probing page type.", pid);
        return false;
    }
    while (fgets(line, MAX_LINE_LENGTH, fp) != NULL) {
        if (IsNumaMapLineHuge(line)) {
            (void)pclose(fp);
            return true;
        }
    }
    if (pclose(fp)) {
        SMAP_LOGGER_WARNING("Close numa maps failed when probing page type, pid=%d.", pid);
    }
    return false;
}

static void SetLocalByNumaMaps(char *line, uint32_t *nodeBitmap, bool hugeFlag)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    char *substr = NULL;
    char pattern[NUMA_MAPS_MAX_PATTERN_LEN];

    /*
     * It's possible that there are multiple Nx= in one line,
     * so it's necessary to traverse all node
     */
    for (i = 0; i < nrLocalNuma; i++) {
        if (hugeFlag && !IsNumaMapLineHuge(line)) {
            continue;
        }
        int ret = snprintf_s(pattern, sizeof(pattern), sizeof(pattern) - 1, " N%d=", i);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Set local numa pattern failed, nid %d.", i);
            continue;
        }
        substr = strstr(line, pattern);
        if (substr) {
            AddL1(nodeBitmap, i);
        }
    }
}

int GetProcessNumaMapsObservation(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask, uint64_t numaPages[MAX_NODES])
{
    if (!residentLocalMask || !numaPages) {
        return -EINVAL;
    }

    FILE *fp = OpenNumaMaps(pid);
    if (!fp) {
        return -EINVAL;
    }

    char line[MAX_LINE_LENGTH];
    int ret = 0;
    while (fgets(line, MAX_LINE_LENGTH, fp) != NULL) {
        ret = AddNumaPagesFromLine(line, numaPages);
        if (ret) {
            break;
        }
        SetLocalByNumaMaps(line, residentLocalMask, hugeFlag);
    }
    if (pclose(fp)) {
        SMAP_LOGGER_WARNING("Close numa maps failed, pid=%d.", pid);
    }
    return ret;
}

int CollectProcessCandidateObservation(pid_t pid, bool hugeFlag, ManagedLocalObservation *observation)
{
    if (!observation) {
        return -EINVAL;
    }

    *observation = (ManagedLocalObservation){ 0 };
    int affinityRet = SetLocalNumaByCpu(pid, &observation->affinityLocalMask);
    observation->affinitySampled = true;
    if (affinityRet) {
        SMAP_LOGGER_WARNING("Set pid %d local numa by cpu failed: %d.", pid, affinityRet);
    } else {
        observation->affinityValid = true;
    }

    int residentRet =
        GetProcessNumaMapsObservation(pid, hugeFlag, &observation->residentLocalMask, observation->numaPages);
    if (residentRet) {
        SMAP_LOGGER_WARNING("Observe pid %d numa maps failed: %d.", pid, residentRet);
    } else {
        observation->residentValid = true;
    }

    if (!observation->affinityValid && !observation->residentValid) {
        return affinityRet ? affinityRet : residentRet;
    }
    return 0;
}

int SetProcessLocalNuma(pid_t pid, uint32_t *nodeBitmap, bool hugeFlag)
{
    if (!nodeBitmap) {
        return -EINVAL;
    }

    ManagedLocalObservation observation;
    int ret = CollectProcessCandidateObservation(pid, hugeFlag, &observation);
    if (ret) {
        return ret;
    }

    uint32_t allLocalMask = BuildAllLocalNumaMask();
    if (allLocalMask == 0) {
        return -EINVAL;
    }
    uint32_t observedLocalMask = (observation.affinityValid ? observation.affinityLocalMask : 0) |
                                 (observation.residentValid ? observation.residentLocalMask : 0);
    observedLocalMask &= allLocalMask;
    if (observedLocalMask == 0) {
        observedLocalMask = allLocalMask;
    }
    *nodeBitmap |= observedLocalMask;
    return 0;
}

/* Sum the migrate memSize (KB) carried by a new migrate-out request. */
static uint64_t SumParamMigrateMemSize(const ProcessParam *param)
{
    uint64_t total = 0;
    for (int i = 0; i < param->count; i++) {
        total += param->numaParam[i].memSize;
    }
    return total;
}

int ProcessAddManage(ProcessParam *param, uint32_t *nodeBitmap)
{
    int ret;
    ProcessTargetConfig config;
    ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        SMAP_LOGGER_ERROR("pid %d target config invalid: %d.", param ? param->pid : -1, ret);
        return ret;
    }

    int pidType = DetectPidType(param->pid);
    if (pidType < 0) {
        SMAP_LOGGER_ERROR("pid %d check failed: %d.", param->pid, pidType);
        return pidType;
    }
    ProcessAttr *current = GetProcessAttr(param->pid);
    if (current) {
        ret = ConfigureMigrationTargets(current, &config);
        if (ret) {
            SMAP_LOGGER_ERROR("Configure pid %d target failed: %d.", current->pid, ret);
            PutProcessAttr(current);
            return ret;
        }
        bool pending = current->pendingTargetConfigValid;
        if (pending && nodeBitmap) {
            current->pendingTargetNumaNodes = *nodeBitmap;
        }
        SMAP_LOGGER_INFO("Update pid %d migrate config, migrateMode: %d, remoteNumaCnt: %d.", current->pid,
                         current->migrateMode, current->remoteNumaCnt);
        for (int i = 0; i < param->count; i++) {
            SMAP_LOGGER_INFO("Update pid:%d success! migrateMode: %d, destnid: %d, memSize: %llu.", current->pid,
                             current->migrateMode, current->migrateParam[i].nid, current->migrateParam[i].memSize);
        }
        if (pending) {
            ret = SyncAllProcessConfig();
            if (ret) {
                SMAP_LOGGER_WARNING("Synchronize pending pid %d config maybe failed: %d.", current->pid, ret);
            }
            SMAP_LOGGER_INFO("Stage pid %d migration target update.", current->pid);
            PutProcessAttr(current);
            return 0;
        }
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", current->pid, ret);
        }
        PutProcessAttr(current);
    } else {
        ret = AddProcess(param, pidType, nodeBitmap);
        if (ret) {
            SMAP_LOGGER_ERROR("Add pid %d to list failed: %d.", param->pid, ret);
            return ret;
        }
        SMAP_LOGGER_INFO("Add pid %d to list done.", param->pid);
    }

    return 0;
}

int UpdateManagedProcessTrackingMode(ProcessAttr *attr, ScanType scanType, uint32_t scanTime, uint32_t duration)
{
    if (!attr || scanType < HAM_SCAN || scanType >= SCAN_TYPE_MAX) {
        return -EINVAL;
    }
    if (attr->state != PROC_MOVE) {
        return -EBUSY;
    }

    attr->scanType = scanType;
    attr->scanTime = scanTime;
    attr->duration = duration;
    attr->isFirstScan = true;
    /* Tracking mode changes are only valid for PROC_MOVE processes. */
    attr->state = PROC_MOVE;
    return 0;
}

void CheckAndRemoveInvalidProcess(void)
{
    struct RemoteNumaInfo *numaInfo;
    struct PidSlot *all[MAX_PID_SLOTS];

    size_t n = PidSlotCollectRefs(&g_processManager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        pid_t pid = attr->pid;
        SMAP_LOGGER_INFO("check if pid %d is valid.", pid);
        if (!PidIsValid(pid)) {
            // send ioctl to remove pid
            struct AccessRemovePidPayload payload = { .pid = pid };
            int ret = AccessIoctlRemovePid(1, &payload);
            if (ret) {
                SMAP_LOGGER_ERROR("access ioctl remove pid %d error: %d.", pid, ret);
            }

            PidSlotRemove(&g_processManager, attr->pid);
            g_processManager.nr[attr->type]--;
            ret = SyncAllProcessConfig();
            if (ret) {
                SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", pid, ret);
            }
            SMAP_LOGGER_INFO("remove pid %d from managed process.", pid);
        }
    }
    if (PidSlotEmpty(&g_processManager)) {
        numaInfo = &g_processManager.remoteNumaInfo;
        EnvMutexLock(&numaInfo->lock);
        ClearRemoteMemUsed();
        SMAP_LOGGER_DEBUG("Remote memory usage cleared.");
        EnvMutexUnlock(&numaInfo->lock);
    }
    PidSlotReleaseRefs(all, n);
}

void RemoveManagedProcess(int nr, pid_t *pidArr)
{
    int ret;
    for (int i = 0; i < nr; i++) {
        ProcessAttr *attr = GetProcessAttr(pidArr[i]);
        if (!attr) {
            SMAP_LOGGER_WARNING("pid: %d, not exist, not need to remove.", pidArr[i]);
            continue;
        }
        PidSlotRemove(&g_processManager, attr->pid);
        SMAP_LOGGER_INFO("Remove pid: %d, from managed process.", pidArr[i]);
        g_processManager.nr[attr->type]--;
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", pidArr[i], ret);
        }
        PutProcessAttr(attr);
    }
}

void RemoveAllManagedProcess(void)
{
    int ret = AccessIoctlRemoveAllPid();
    if (ret) {
        SMAP_LOGGER_ERROR("access ioctl remove all pid error: %d.", ret);
    }
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(&g_processManager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        SMAP_LOGGER_INFO("During destruction remove pid: %d, from managed process.", attr->pid);
        PidSlotRemove(&g_processManager, attr->pid);
    }
    PidSlotReleaseRefs(all, n);
    g_processManager.nr[VM_TYPE] = g_processManager.nr[PROCESS_TYPE] = 0;
}

int DestroyProcessManager(void)
{
    RemoveAllManagedProcess();
    PidSlotDestroy(&g_processManager);
    EnvMutexDestroy(&g_processManager.threadLock);
    (void)memset_s(&g_processManager, sizeof(struct ProcessManager), 0, sizeof(struct ProcessManager));
    return 0;
}

struct ProcessManager *GetProcessManager(void)
{
    return &g_processManager;
}
