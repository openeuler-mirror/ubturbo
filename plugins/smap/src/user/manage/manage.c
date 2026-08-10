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

#define MAX_GROUP_TARGET_ENTRY (MAX_MIGRATION_GROUP_NUM * MAX_GROUP_REMOTE_NUMA)

#define UPDATE_CRITICAL_ERR_COUNT 5

static struct ProcessManager g_processManager;

static char g_mmapTypeName[][MMAP_TYPE_STRING_LEN] = { "mmap_private", "mmap_shared" };
static char *g_nodePattern[LOCAL_NUMA_NUM] = { " N0=", " N1=", " N2=", " N3=" };

uint32_t g_pageSizeNormal;
uint32_t g_pageSizeHuge;

EnvAtomic g_forbiddenNodes[MAX_NODES];
RunMode g_runMode;

uint8_t g_criticalErrNodes[REMOTE_NUMA_BITS];
typedef struct {
    uint32_t affinityLocalMask;
    uint32_t residentLocalMask;
    uint64_t numaPages[MAX_NODES];
    bool affinityValid;
    bool affinitySampled;
    bool residentValid;
} ManagedLocalObservation;

static int CollectProcessCandidateObservation(pid_t pid, bool hugeFlag, ManagedLocalObservation *observation);

RunMode GetRunMode(void)
{
    return g_runMode;
}

void SetRunMode(RunMode runMode)
{
    g_runMode = runMode;
}

PidType GetPidType(struct ProcessManager *manager)
{
    return manager->tracking.pageSize == g_pageSizeNormal ? PROCESS_TYPE : VM_TYPE;
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

void InitProcessTargetConfig(ProcessTargetConfig *config)
{
    if (!config) {
        return;
    }

    *config = (ProcessTargetConfig){ 0 };
}

void ClearProcessTargetConfig(ProcessTargetConfig *config)
{
    InitProcessTargetConfig(config);
}

int CopyProcessTargetConfig(ProcessTargetConfig *dest, const ProcessTargetConfig *src)
{
    if (!dest || !src || src->count > REMOTE_NUMA_NUM ||
        (src->migrateMode != MIG_RATIO_MODE && src->migrateMode != MIG_MEMSIZE_MODE)) {
        return -EINVAL;
    }

    *dest = *src;
    return 0;
}

bool RemoveProcessRemoteTarget(ProcessTargetConfig *config, int remoteNid)
{
    if (!config || config->count > REMOTE_NUMA_NUM) {
        return false;
    }

    for (uint32_t i = 0; i < config->count; i++) {
        if (config->targets[i].remoteNid != remoteNid) {
            continue;
        }
        for (uint32_t j = i + 1; j < config->count; j++) {
            config->targets[j - 1] = config->targets[j];
        }
        config->targets[--config->count] = (ProcessRemoteTarget){ 0 };
        return true;
    }
    return false;
}

int MoveProcessRemoteTarget(ProcessTargetConfig *config, int srcNid, int destNid, uint64_t memSizeKB, int ratio)
{
    if (!config || config->count > REMOTE_NUMA_NUM || srcNid == destNid ||
        (config->migrateMode != MIG_RATIO_MODE && config->migrateMode != MIG_MEMSIZE_MODE) || ratio < 0) {
        return -EINVAL;
    }

    int srcIndex = -1;
    int destIndex = -1;
    for (uint32_t i = 0; i < config->count; i++) {
        if (config->targets[i].remoteNid == srcNid) {
            srcIndex = (int)i;
        }
        if (config->targets[i].remoteNid == destNid) {
            destIndex = (int)i;
        }
    }
    if (srcIndex < 0) {
        return -ENOENT;
    }

    ProcessRemoteTarget *src = &config->targets[srcIndex];
    uint64_t moved = config->migrateMode == MIG_RATIO_MODE ? (uint64_t)ratio : memSizeKB;
    uint64_t source = config->migrateMode == MIG_RATIO_MODE ? src->ratio : src->memSizeKB;
    if (moved == 0) {
        return 0;
    }
    if (moved > source) {
        return -ERANGE;
    }

    if (destIndex < 0 && moved == source) {
        src->remoteNid = destNid;
        return 0;
    }
    if (destIndex < 0) {
        if (config->count == REMOTE_NUMA_NUM) {
            return -ENOSPC;
        }
        destIndex = (int)config->count++;
        config->targets[destIndex] = (ProcessRemoteTarget){ .remoteNid = destNid };
    }

    ProcessRemoteTarget *dest = &config->targets[destIndex];
    if (config->migrateMode == MIG_RATIO_MODE) {
        if (moved > HUNDRED || dest->ratio > HUNDRED - moved) {
            return -ERANGE;
        }
        src->ratio -= (uint32_t)moved;
        dest->ratio += (uint32_t)moved;
        if (src->ratio == 0) {
            (void)RemoveProcessRemoteTarget(config, srcNid);
        }
        return 0;
    }

    if (dest->memSizeKB > UINT64_MAX - moved) {
        return -EOVERFLOW;
    }
    src->memSizeKB -= moved;
    dest->memSizeKB += moved;
    if (src->memSizeKB == 0) {
        (void)RemoveProcessRemoteTarget(config, srcNid);
    }
    return 0;
}

static int ValidateProcessTargetConfig(const ProcessTargetConfig *config)
{
    ProcessTargetConfig copy;
    if (CopyProcessTargetConfig(&copy, config)) {
        return -EINVAL;
    }

    uint64_t totalRatio = 0;
    uint64_t pageSizeKB = (IsHugeMode() ? PAGESIZE_2M : PAGESIZE_4K) / KIB;
    for (uint32_t i = 0; i < copy.count; i++) {
        for (uint32_t j = i + 1; j < copy.count; j++) {
            if (copy.targets[i].remoteNid == copy.targets[j].remoteNid) {
                return -EINVAL;
            }
        }

        if (copy.migrateMode == MIG_RATIO_MODE) {
            if (copy.targets[i].ratio > HUNDRED) {
                return -EINVAL;
            }
            totalRatio += copy.targets[i].ratio;
            continue;
        }

        if (copy.targets[i].memSizeKB % pageSizeKB != 0 || copy.targets[i].memSizeKB / pageSizeKB > UINT32_MAX) {
            return -EINVAL;
        }
    }

    if (copy.migrateMode == MIG_RATIO_MODE && totalRatio > HUNDRED) {
        return -EINVAL;
    }
    return 0;
}

const ProcessRemoteTarget *FindProcessRemoteTarget(const ProcessTargetConfig *config, int remoteNid)
{
    if (!config || config->count > REMOTE_NUMA_NUM) {
        return NULL;
    }

    for (uint32_t i = 0; i < config->count; i++) {
        if (config->targets[i].remoteNid == remoteNid) {
            return &config->targets[i];
        }
    }
    return NULL;
}

int RemoteNidToIndex(int remoteNid, int nrLocalNuma, int *remoteIndex)
{
    if (!remoteIndex || nrLocalNuma <= 0 || nrLocalNuma > LOCAL_NUMA_NUM || remoteNid < nrLocalNuma ||
        remoteNid - nrLocalNuma >= REMOTE_NUMA_NUM) {
        return -EINVAL;
    }

    *remoteIndex = remoteNid - nrLocalNuma;
    return 0;
}

void InitProcessMigrationTargetState(ProcessAttr *attr)
{
    if (!attr) {
        return;
    }

    InitProcessTargetConfig(&attr->targetConfig);
    InitProcessTargetConfig(&attr->pendingTargetConfig);
    attr->ignoreRemoteCapacity = false;
    attr->pendingTargetConfigValid = false;
    attr->pendingIgnoreRemoteCapacity = false;
    attr->pendingTargetNumaNodes = 0;
    attr->managedLocalState = (ManagedLocalState){ 0 };
}

static uint32_t BuildAllLocalNumaMask(void)
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

static uint32_t BuildAccountLocalMask(const ProcessAttr *attr, int remoteIndex)
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

static int ApplyManagedLocalObservation(ProcessAttr *attr, const ManagedLocalObservation *observation,
                                        bool fullReplacement)
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
    ThreadCtx *ctx = g_processManager.threadCtx[0];
    if (ctx && ctx->period != 0) {
        return ctx->period;
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

static int RefreshManagedLocalState(ProcessAttr *attr, bool fullReplacement)
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

static uint32_t BuildManagedTrackingNodes(const ProcessAttr *attr)
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

    for (i = 0; i < MAX_NODES; i++) {
        g_processManager.fds.nodes[i] = DEFAULT_FD;
    }
    g_processManager.fds.migrate = DEFAULT_FD;
    g_processManager.fds.access = DEFAULT_FD;
    g_processManager.fds.lock = DEFAULT_FD;
    for (i = 0; i < MAX_THREADS; i++) {
        g_processManager.threadCtx[i] = NULL;
    }
    g_processManager.processes = NULL;
    g_processManager.ubBwMonitor.ubBwThreshold = GetUbBwThresholdConfig();
    g_processManager.ubBwMonitor.currentFluxRet = -ENODATA;
    RemoteNumaInfoInit();
    EnvMutexInit(&g_processManager.lock);
    EnvMutexInit(&g_processManager.threadLock);
    InitSceneInfo(&g_processManager.sceneInfo, pageType == PAGETYPE_HUGE);
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

int IsQemuTask(pid_t pid)
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
    if (fgets(comm, sizeof(comm), file)) {
        SMAP_LOGGER_DEBUG("Skip the first line of comm file.");
    }
    if (fgets(comm, sizeof(comm), file)) {
        SMAP_LOGGER_INFO("After fgets comm file");
        pclose(file);
        if ((strncmp(comm, VM_NAME_STR, PID_NAME_LEN) == 0) ||
            (strncmp(comm, VM_KVM_NAME_STR, PID_KVM_NAME_LEN) == 0)) {
            ret = VM_TYPE;
        } else {
            ret = PROCESS_TYPE;
        }
        return ret;
    }
    SMAP_LOGGER_ERROR("Error occur in fgets comm file");

    (void)pclose(file);
    return -1;
}

void LinkedListAdd(ProcessAttr **head, ProcessAttr **add)
{
    (*add)->next = *head;
    *head = *add;
}

static void ResetActcData(ActcData *actcData[], int len)
{
    for (int i = 0; i < len; i++) {
        if (actcData[i]) {
            free(actcData[i]);
            actcData[i] = NULL;
        }
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

void LinkedListRemove(ProcessAttr **remove, ProcessAttr **head)
{
    if (*head == NULL || *remove == NULL) {
        return;
    }

    ProcessAttr *toRemove = *remove;

    if (*head == toRemove) {
        *head = toRemove->next;
        toRemove->next = NULL;
        FreeProceccesAttr(toRemove);
        return;
    }

    ProcessAttr *prev = *head;
    while (prev->next != NULL && prev->next != toRemove) {
        prev = prev->next;
    }
    if (prev->next == toRemove) {
        prev->next = toRemove->next;
        toRemove->next = NULL;
        FreeProceccesAttr(toRemove);
        *remove = NULL;
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

static int CheckPid(pid_t pid)
{
    PidType type = GetPidType(&g_processManager);
    int ret;
    if (!PidIsValid(pid)) {
        SMAP_LOGGER_ERROR("Input pid %d is invalid.", pid);
        return -ESRCH;
    }
    ret = IsQemuTask(pid);
    if (ret != type) {
        SMAP_LOGGER_ERROR("Pid %d type(%d) conflict with current pid type(%d).", pid, ret, type);
        return -EINVAL;
    }
    return 0;
}

ProcessAttr *GetProcessAttr(pid_t pid)
{
    ProcessAttr *current = g_processManager.processes;
    EnvMutexLock(&g_processManager.lock);
    while (current && current->pid != pid) {
        current = current->next;
    }
    EnvMutexUnlock(&g_processManager.lock);
    return current;
}

/* 调用前必须持有锁g_processManager.lock */
ProcessAttr *GetProcessAttrLocked(pid_t pid)
{
    ProcessAttr *current = g_processManager.processes;
    while (current && current->pid != pid) {
        current = current->next;
    }
    return current;
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
    if (GetPidType(&g_processManager) != VM_TYPE) {
        return 0;
    }
    int ret = ParseMmapType(pid, &attr->vmPidAttr.mmapType);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse mmap type of pid %d failed.", pid);
        return 0;
    }
    return 0;
}

static int BuildProcessTargetConfigFromParam(const ProcessParam *param, ProcessTargetConfig *config)
{
    if (!param || !config || param->count < 0 || param->count > REMOTE_NUMA_NUM) {
        return -EINVAL;
    }

    if (param->targetConfigValid) {
        if (ValidateProcessTargetConfig(&param->targetConfig)) {
            return -EINVAL;
        }
        return CopyProcessTargetConfig(config, &param->targetConfig);
    }

    InitProcessTargetConfig(config);
    if (param->count == 0 || (param->count == 1 && param->numaParam[0].nid == DEFAULT_L2_NODE)) {
        return 0;
    }

    config->migrateMode = param->numaParam[0].migrateMode;
    for (int i = 0; i < param->count; i++) {
        if (param->numaParam[i].migrateMode != config->migrateMode ||
            FindProcessRemoteTarget(config, param->numaParam[i].nid)) {
            return -EINVAL;
        }
        ProcessRemoteTarget *target = &config->targets[config->count++];
        target->remoteNid = param->numaParam[i].nid;
        target->ratio = param->numaParam[i].ratio;
        target->memSizeKB = param->numaParam[i].memSize;
    }
    return ValidateProcessTargetConfig(config);
}

/* Set process attributes that are independent of its migration target. */
static void SetBasicProcessConfig(ProcessAttr *attr, ProcessParam *param)
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

/* Handle multi-NUMA VM scenario: VM with multiple local or remote NUMAs */
static void SetMultiNumaVmConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma)
{
    for (int i = 0; i < param->count; i++) {
        int remoteNid = param->numaParam[i].nid;
        int l2Index = remoteNid - nrLocalNuma;

        attr->migrateParam[i].nid = remoteNid;
        attr->migrateParam[i].memSize = param->numaParam[i].memSize;
        SMAP_LOGGER_INFO("Multi-NUMA VM destNid: %d, memSize: %lu", remoteNid, attr->migrateParam[i].memSize);

        /* Set the same ratio for all local NUMAs */
        for (int j = 0; j < nrLocalNuma && j < LOCAL_NUMA_NUM; j++) {
            attr->strategyAttr.initRemoteMemRatio[j][l2Index] = param->numaParam[i].ratio;
            SMAP_LOGGER_INFO("Multi-NUMA VM destNid: %d, ratio: %d", remoteNid, param->numaParam[i].ratio);
        }
        AddAttrL2(attr, remoteNid);
    }
}

/* Migrate additional pages to remote NUMA in forward order (NUMA0 -> NUMA1 -> ...) */
static void MigratePagesToRemote(ProcessAttr *attr, int l2Index, const uint64_t pagesPerNuma[MAX_NODES], uint64_t pages)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t pagesToMigrate = pages;

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM && pagesToMigrate > 0; i++) {
        if (InAttrL1(attr, i)) {
            uint64_t allocPages = MIN(pagesPerNuma[i], pagesToMigrate);
            attr->strategyAttr.memSize[i][l2Index] += allocPages * (pageSize / KIB);
            pagesToMigrate -= allocPages;
        }
    }
}

static void ClearSingleRemoteTarget(ProcessAttr *attr, int l2Index, int nrLocalNuma)
{
    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        attr->strategyAttr.memSize[i][l2Index] = 0;
        attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index] = 0;
    }
}

static void AccountExistingRemotePagesByOldAccount(ProcessAttr *attr, int l2Index, uint64_t remotePages,
                                                   uint64_t oldTotal, int nrLocalNuma)
{
    uint64_t nrLeft = remotePages;

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        if (!InAttrL1(attr, i)) {
            continue;
        }

        uint64_t oldPages = attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index];
        uint64_t nrPages = oldTotal == 0 ? 0 : remotePages * oldPages / oldTotal;
        if (nrPages > nrLeft) {
            nrPages = nrLeft;
        }
        attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index] = nrPages;
        nrLeft -= nrPages;
    }

    for (int i = nrLocalNuma - 1; i >= 0 && nrLeft > 0; i--) {
        if (!InAttrL1(attr, i)) {
            continue;
        }
        attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index] += nrLeft;
        break;
    }
}

static void AccountExistingRemotePagesByLocalPages(ProcessAttr *attr, int l2Index,
                                                   const uint64_t pagesPerNuma[MAX_NODES], uint64_t remotePages,
                                                   int nrLocalNuma)
{
    uint64_t localTotal = 0;
    uint64_t nrLeft = remotePages;

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        if (InAttrL1(attr, i)) {
            localTotal += pagesPerNuma[i];
        }
    }

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        if (!InAttrL1(attr, i)) {
            continue;
        }

        uint64_t nrPages = 0;
        if (localTotal > 0) {
            nrPages = remotePages * pagesPerNuma[i] / localTotal;
        }
        if (nrPages > nrLeft) {
            nrPages = nrLeft;
        }
        attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index] = nrPages;
        nrLeft -= nrPages;
    }

    for (int i = nrLocalNuma - 1; i >= 0 && nrLeft > 0; i--) {
        if (!InAttrL1(attr, i)) {
            continue;
        }
        attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index] += nrLeft;
        break;
    }
}

static void AccountExistingRemotePages(ProcessAttr *attr, int l2Index, const uint64_t pagesPerNuma[MAX_NODES],
                                       uint64_t remotePages, int nrLocalNuma)
{
    uint64_t oldTotal = 0;

    if (remotePages == 0) {
        return;
    }

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        if (InAttrL1(attr, i)) {
            oldTotal += attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index];
        }
    }

    if (oldTotal > 0) {
        AccountExistingRemotePagesByOldAccount(attr, l2Index, remotePages, oldTotal, nrLocalNuma);
    } else {
        AccountExistingRemotePagesByLocalPages(attr, l2Index, pagesPerNuma, remotePages, nrLocalNuma);
    }
}

static void SetSingleRemoteTargetPages(ProcessAttr *attr, int l2Index, const uint64_t pagesPerNuma[MAX_NODES],
                                       uint64_t targetPages, int nrLocalNuma)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    uint64_t nrLeft = targetPages;
    int lastLocal = NUMA_NO_NODE;

    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM && nrLeft > 0; i++) {
        if (!InAttrL1(attr, i)) {
            continue;
        }

        uint64_t accounted = attr->strategyAttr.remoteNrPagesAfterMigrate[i][l2Index];
        uint64_t capacity = accounted + pagesPerNuma[i];
        uint64_t nrPages = MIN(capacity, nrLeft);
        attr->strategyAttr.memSize[i][l2Index] = nrPages * (pageSize / KIB);
        nrLeft -= nrPages;
        lastLocal = i;
    }

    if (nrLeft > 0 && lastLocal != NUMA_NO_NODE) {
        attr->strategyAttr.memSize[lastLocal][l2Index] += nrLeft * (pageSize / KIB);
    }
}

/* Recall pages from remote NUMA in reverse order (NUMA(n-1) -> ... -> NUMA1 -> NUMA0) */
static void RecallPagesFromRemote(ProcessAttr *attr, int l2Index, uint64_t pages)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t pagesToRecall = pages;

    for (int i = nrLocalNuma - 1; i >= 0 && pagesToRecall > 0; i--) {
        if (InAttrL1(attr, i)) {
            uint64_t existingMemSizePages = attr->strategyAttr.memSize[i][l2Index] / (pageSize / KIB);
            uint64_t recallPages = MIN(existingMemSizePages, pagesToRecall);
            attr->strategyAttr.memSize[i][l2Index] -= recallPages * (pageSize / KIB);
            pagesToRecall -= recallPages;
        }
    }
}

/* Handle single remote NUMA scenario: single local+single remote, or multi-local+single remote */
static int SetSingleRemoteNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma,
                                     const uint64_t pagesPerNuma[MAX_NODES])
{
    if (!pagesPerNuma) {
        return -EINVAL;
    }

    int remoteNid = param->numaParam[0].nid;

    /* Validate remote NUMA node */
    if (remoteNid < nrLocalNuma || remoteNid >= nrLocalNuma + REMOTE_NUMA_NUM) {
        SMAP_LOGGER_WARNING("Invalid remote numa %d for pid %d, nrLocalNuma: %d.", remoteNid, attr->pid, nrLocalNuma);
        return -EINVAL;
    }

    int l2Index = remoteNid - nrLocalNuma;

    /* Calculate target pages and pages already on remote NUMA */
    uint64_t targetPages = IsHugeMode() ? KBToHugePage(param->numaParam[0].memSize) :
                                          KBToNormalPage(param->numaParam[0].memSize);
    uint64_t remoteExistingPages = pagesPerNuma[remoteNid];

    /* Set ratio for all local NUMAs */
    for (int i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        attr->strategyAttr.initRemoteMemRatio[i][l2Index] = param->numaParam[0].ratio;
    }

    ClearSingleRemoteTarget(attr, l2Index, nrLocalNuma);
    AccountExistingRemotePages(attr, l2Index, pagesPerNuma, remoteExistingPages, nrLocalNuma);
    SetSingleRemoteTargetPages(attr, l2Index, pagesPerNuma, targetPages, nrLocalNuma);

    attr->migrateParam[0].memSize = param->numaParam[0].memSize;
    attr->migrateParam[0].nid = remoteNid;
    /*
     * Keep omitted remotes in the scan scope until their existing pages have
     * converged to the new full-replacement target of zero.
     */
    AddAttrL2(attr, remoteNid);
    return 0;
}

static void ClearCompatibleProcessTargets(ProcessAttr *attr)
{
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        attr->migrateParam[i].nid = DEFAULT_L2_NODE;
        attr->migrateParam[i].memSize = 0;
        for (int j = 0; j < LOCAL_NUMA_NUM; j++) {
            attr->strategyAttr.initRemoteMemRatio[j][i] = 0;
            attr->strategyAttr.memSize[j][i] = 0;
            attr->strategyAttr.allocRemoteNrPages[j][i] = 0;
            attr->strategyAttr.l2RemoteMemRatio[j][i] = 0;
            attr->strategyAttr.l3RemoteMemRatio[j][i] = 0;
        }
    }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            attr->strategyAttr.nrMigratePages[i][j] = 0;
        }
    }
}

static void TargetConfigToProcessParam(const ProcessAttr *attr, const ProcessTargetConfig *config, ProcessParam *param)
{
    *param = (ProcessParam){
        .pid = attr->pid,
        .scanType = attr->scanType,
        .count = config->count,
    };
    for (uint32_t i = 0; i < config->count; i++) {
        param->numaParam[i].nid = config->targets[i].remoteNid;
        param->numaParam[i].ratio = config->targets[i].ratio;
        param->numaParam[i].memSize = config->targets[i].memSizeKB;
        param->numaParam[i].migrateMode = config->migrateMode;
    }
}

/* Generate compatibility runtime fields from the requested target. */
static int UpdateProcessMigrateConfig(ProcessAttr *attr, const ProcessTargetConfig *config,
                                      const ManagedLocalObservation *observation)
{
    int nrLocalNuma = GetNrLocalNuma();
    int localNumaCnt = GetL1Count(attr->numaAttr.numaNodes);
    bool isVm = GetPidType(&g_processManager) == VM_TYPE;
    ProcessParam param;

    attr->migrateMode = config->migrateMode;
    attr->remoteNumaCnt = config->count;
    int localRatio = HUNDRED;
    if (config->migrateMode == MIG_RATIO_MODE) {
        for (uint32_t i = 0; i < config->count; i++) {
            localRatio -= config->targets[i].ratio;
        }
    }
    attr->initLocalMemRatio = localRatio;
    ClearCompatibleProcessTargets(attr);
    if (config->count == 0) {
        return 0;
    }

    TargetConfigToProcessParam(attr, config, &param);
    if (isVm && (config->count > 1 || localNumaCnt > 1)) {
        SetMultiNumaVmConfig(attr, &param, nrLocalNuma);
        return 0;
    }
    if (!observation || !observation->residentValid) {
        SMAP_LOGGER_ERROR("Pid %d page residency is unavailable.", attr->pid);
        return -EINVAL;
    }
    return SetSingleRemoteNumaConfig(attr, &param, nrLocalNuma, observation->numaPages);
}

static bool IsZeroProcessTargetConfig(const ProcessTargetConfig *config)
{
    if (!config) {
        return false;
    }
    if (config->count == 0) {
        return true;
    }

    for (uint32_t i = 0; i < config->count; i++) {
        if (config->migrateMode == MIG_MEMSIZE_MODE) {
            if (config->targets[i].memSizeKB != 0) {
                return false;
            }
            continue;
        }
        if (config->targets[i].ratio != 0) {
            return false;
        }
    }
    return true;
}

static void UpdateAutoRemoveRemoteEmptyFlag(ProcessAttr *attr, const ProcessTargetConfig *config)
{
    if (!attr || attr->groupPolicy.enabled) {
        return;
    }
    if (attr->scanType != NORMAL_SCAN) {
        attr->autoRemoveWhenRemoteEmpty = false;
        return;
    }

    attr->autoRemoveWhenRemoteEmpty = IsZeroProcessTargetConfig(config);
    if (attr->autoRemoveWhenRemoteEmpty) {
        SMAP_LOGGER_INFO("Pid %d will be auto removed after all remote pages migrate back.", attr->pid);
    }
}

static int ValidateCandidateRemoteResidency(ProcessAttr *candidate, const ProcessTargetConfig *config,
                                            const ManagedLocalObservation *observation)
{
    if (!candidate || !config || !observation || !observation->residentValid) {
        return 0;
    }

    int nrLocalNuma = GetNrLocalNuma();
    for (int remoteNid = nrLocalNuma; remoteNid < nrLocalNuma + REMOTE_NUMA_NUM; remoteNid++) {
        if (observation->numaPages[remoteNid] == 0 || FindProcessRemoteTarget(config, remoteNid) ||
            InAttrL2(candidate, remoteNid)) {
            continue;
        }
        SMAP_LOGGER_ERROR("Pid %d has unmanaged remote node %d resident pages.", candidate->pid, remoteNid);
        return -EINVAL;
    }
    return 0;
}

static int PrepareProcessTargetCandidate(ProcessAttr *candidate, const ProcessTargetConfig *config,
                                         const ManagedLocalObservation *observation)
{
    ProcessTargetConfig targetConfig;
    int ret = CopyProcessTargetConfig(&targetConfig, config);
    if (ret) {
        return ret;
    }
    ret = ValidateCandidateRemoteResidency(candidate, &targetConfig, observation);
    if (ret) {
        return ret;
    }

    candidate->targetConfig = targetConfig;
    ret = ApplyManagedLocalObservation(candidate, observation, true);
    if (ret) {
        return ret;
    }
    candidate->numaAttr.numaNodes = BuildManagedTrackingNodes(candidate);
    ret = UpdateProcessMigrateConfig(candidate, &targetConfig, observation);
    if (ret) {
        return ret;
    }
    UpdateAutoRemoveRemoteEmptyFlag(candidate, &targetConfig);
    return 0;
}

static void PublishProcessTargetCandidate(ProcessAttr *attr, const ProcessAttr *candidate)
{
    attr->targetConfig = candidate->targetConfig;
    attr->migrateMode = candidate->migrateMode;
    attr->remoteNumaCnt = candidate->remoteNumaCnt;
    attr->initLocalMemRatio = candidate->initLocalMemRatio;
    attr->autoRemoveWhenRemoteEmpty = candidate->autoRemoveWhenRemoteEmpty;
    attr->ignoreRemoteCapacity = candidate->ignoreRemoteCapacity;
    attr->numaAttr = candidate->numaAttr;
    attr->managedLocalState = candidate->managedLocalState;
    attr->strategyAttr = candidate->strategyAttr;
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        attr->migrateParam[i] = candidate->migrateParam[i];
    }
}

static int ApplyProcessTargetConfig(ProcessAttr *attr, const ProcessTargetConfig *config)
{
    ManagedLocalObservation observation;
    int ret = CollectProcessCandidateObservation(attr->pid, attr->type == VM_TYPE, &observation);
    if (ret) {
        return ret;
    }

    ProcessAttr candidate = *attr;
    ret = PrepareProcessTargetCandidate(&candidate, config, &observation);
    if (ret) {
        return ret;
    }
    PublishProcessTargetCandidate(attr, &candidate);
    return 0;
}

static int StagePendingMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config, bool ignoreRemoteCapacity)
{
    ProcessTargetConfig targetConfig;
    if (!attr || ValidateProcessTargetConfig(config) || CopyProcessTargetConfig(&targetConfig, config)) {
        return -EINVAL;
    }

    ProcessAttr trackingCandidate = *attr;
    trackingCandidate.targetConfig = targetConfig;
    if (trackingCandidate.managedLocalState.managedLocalMask == 0) {
        uint32_t allLocalMask = BuildAllLocalNumaMask();
        trackingCandidate.managedLocalState.managedLocalMask = attr->numaAttr.numaNodes & allLocalMask;
        if (trackingCandidate.managedLocalState.managedLocalMask == 0) {
            trackingCandidate.managedLocalState.managedLocalMask = allLocalMask;
        }
    }
    attr->pendingTargetConfig = targetConfig;
    attr->pendingTargetConfigValid = true;
    attr->pendingIgnoreRemoteCapacity = ignoreRemoteCapacity;
    attr->pendingTargetNumaNodes = BuildManagedTrackingNodes(&trackingCandidate);
    SMAP_LOGGER_INFO("Save pending migration target for pid %d.", attr->pid);
    return 0;
}

static int ConfigureMigrationTargetsWithCapacityPolicy(ProcessAttr *attr, const ProcessTargetConfig *config,
                                                       bool ignoreRemoteCapacity)
{
    ProcessTargetConfig targetConfig;
    if (!attr || ValidateProcessTargetConfig(config) || CopyProcessTargetConfig(&targetConfig, config)) {
        return -EINVAL;
    }

    if (attr->state == PROC_MIGRATE) {
        return StagePendingMigrationTargets(attr, &targetConfig, ignoreRemoteCapacity);
    }

    int ret = ApplyProcessTargetConfig(attr, &targetConfig);
    if (!ret) {
        attr->ignoreRemoteCapacity = ignoreRemoteCapacity;
    }
    return ret;
}

int ConfigureMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config)
{
    return ConfigureMigrationTargetsWithCapacityPolicy(attr, config, false);
}

int ApplyPendingMigrationTargets(ProcessAttr *attr)
{
    if (!attr || !attr->pendingTargetConfigValid) {
        return 0;
    }

    ProcessTargetConfig config = attr->pendingTargetConfig;
    ManagedLocalObservation observation;
    int ret = CollectProcessCandidateObservation(attr->pid, attr->type == VM_TYPE, &observation);
    if (ret) {
        return ret;
    }

    ProcessAttr candidate = *attr;
    ret = PrepareProcessTargetCandidate(&candidate, &config, &observation);
    if (ret) {
        return ret;
    }

    struct AccessAddPidPayload payload = {
        .type = NORMAL_SCAN,
        .pid = attr->pid,
        .scanTime = attr->scanTime,
        .duration = attr->duration,
        .numaNodes = candidate.numaAttr.numaNodes,
    };
    ret = AccessIoctlAddPid(1, &payload);
    if (ret) {
        SMAP_LOGGER_ERROR("Update pending pid %d tracking failed: %d.", attr->pid, ret);
        return ret;
    }

    PublishProcessTargetCandidate(attr, &candidate);

    attr->ignoreRemoteCapacity = attr->pendingIgnoreRemoteCapacity;
    ClearProcessTargetConfig(&attr->pendingTargetConfig);
    attr->pendingTargetConfigValid = false;
    attr->pendingIgnoreRemoteCapacity = false;
    attr->pendingTargetNumaNodes = 0;
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pending pid %d config maybe failed: %d.", attr->pid, ret);
    }
    SMAP_LOGGER_INFO("Apply pending migration target for pid %d.", attr->pid);
    return 0;
}

static bool IsZeroRemoteTargetConfig(ProcessParam *param)
{
    ProcessTargetConfig config;
    if (BuildProcessTargetConfigFromParam(param, &config)) {
        return false;
    }
    return IsZeroProcessTargetConfig(&config);
}

static int SetProcessConfig(ProcessAttr *attr, ProcessParam *param)
{
    ProcessTargetConfig config;
    int ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        return ret;
    }

    SetBasicProcessConfig(attr, param);
    return ConfigureMigrationTargetsWithCapacityPolicy(attr, &config, param->ignoreRemoteCapacity);
}

static void SetGroupedProcessConfig(ProcessAttr *attr, pid_t pid, uint32_t nodeBitmap,
                                    const GroupMigrationPolicy *policy)
{
    attr->pid = pid;
    attr->scanTime = SCAN_TIME_2M;
    attr->duration = 0;
    attr->scanType = NORMAL_SCAN;
    attr->type = VM_TYPE;
    attr->migrateMode = MIG_MEMSIZE_MODE;
    attr->remoteNumaCnt = GetL2Count(nodeBitmap);
    attr->enableSwap = true;
    attr->initLocalMemRatio = HUNDRED;
    attr->numaAttr.numaNodes = nodeBitmap;
    attr->groupPolicy = *policy;
    attr->groupSwapLastTotalPages = 0;
    attr->groupSwapStableTotalRounds = 0;
    attr->groupSwapTotalPagesValid = false;
    attr->groupSwapFrozen = false;
    attr->pendingGroupPolicy.valid = false;
    attr->autoRemoveWhenRemoteEmpty = false;
    attr->syncWaitRemoteEmpty = false;
    if (time(&attr->scanStart) == (time_t)-1) {
        SMAP_LOGGER_ERROR("get time error");
    }
}

static void ResetGroupedPolicyRuntime(GroupMigrationPolicy *policy)
{
    if (!policy) {
        return;
    }
    /*
     * Pending policy is copied from a new user request. Rebuild runtime counters
     * from current numa_maps before it replaces the active policy.
     */
    for (int i = 0; i < policy->groupCount; i++) {
        MigrationGroupAttr *group = &policy->groups[i];
        group->swapCandidateRounds = 0;
        for (int j = 0; j < group->targetCount; j++) {
            group->targets[j].usedPages = 0;
        }
    }
}

int AddProcess(ProcessParam *param, PidType type, uint32_t *nodeBitmap)
{
    int ret;
    (void)nodeBitmap;
    if (g_processManager.nr[type] >= GetCurrentMaxNrPid()) {
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
    LinkedListAdd(&g_processManager.processes, &attr);
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

    free(candidate->prepared);
    *candidate = (ProcessManageCandidate){ 0 };
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
    ret = CheckPid(param->pid);
    if (ret) {
        return ret;
    }

    ProcessAttr *active = GetProcessAttrLocked(param->pid);
    ProcessAttr *prepared = NULL;
    if (active) {
        prepared = malloc(sizeof(ProcessAttr));
        if (!prepared) {
            return -ENOMEM;
        }
        *prepared = *active;
    } else {
        if (g_processManager.nr[type] >= GetCurrentMaxNrPid()) {
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
    ret = ConfigureMigrationTargetsWithCapacityPolicy(prepared, &config, param->ignoreRemoteCapacity);
    if (ret) {
        DiscardProcessManageCandidate(candidate);
        return ret;
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
        LinkedListAdd(&g_processManager.processes, &prepared);
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

static int CollectGroupedTargetEntries(GroupMigrationPolicy *policy, int targetNid,
                                       int groupIdx[MAX_GROUP_TARGET_ENTRY], int targetIdx[MAX_GROUP_TARGET_ENTRY])
{
    int count = 0;
    for (int i = 0; i < policy->groupCount; i++) {
        MigrationGroupAttr *group = &policy->groups[i];
        for (int j = 0; j < group->targetCount; j++) {
            if (group->targets[j].nid != targetNid) {
                continue;
            }
            if (count >= MAX_GROUP_TARGET_ENTRY) {
                SMAP_LOGGER_ERROR("Grouped target entry count exceeds limit.");
                return -EINVAL;
            }
            groupIdx[count] = i;
            targetIdx[count] = j;
            count++;
        }
    }
    return count;
}

static int InitGroupedTargetUsedPages(pid_t pid, GroupMigrationPolicy *policy, int targetNid, uint64_t residentPages)
{
    int groupIdx[MAX_GROUP_TARGET_ENTRY] = { 0 };
    int targetIdx[MAX_GROUP_TARGET_ENTRY] = { 0 };
    int entryCount = CollectGroupedTargetEntries(policy, targetNid, groupIdx, targetIdx);
    if (entryCount <= 0) {
        SMAP_LOGGER_ERROR("pid %d has unmanaged remote node %d resident pages %llu.", pid, targetNid, residentPages);
        return -EINVAL;
    }

    uint64_t quotaSum = 0;
    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[groupIdx[i]].targets[targetIdx[i]];
        if (UINT64_MAX - quotaSum < target->quotaPages) {
            SMAP_LOGGER_ERROR("pid %d remote node %d quota sum overflow.", pid, targetNid);
            return -EINVAL;
        }
        quotaSum += target->quotaPages;
    }
    if (quotaSum == 0) {
        SMAP_LOGGER_ERROR("pid %d remote node %d quota sum is zero.", pid, targetNid);
        return -EINVAL;
    }
    if (residentPages > quotaSum) {
        SMAP_LOGGER_ERROR("pid %d remote node %d resident pages %llu exceed quota sum %llu.", pid, targetNid,
                          residentPages, quotaSum);
        return -EINVAL;
    }

    uint64_t assignedPages = 0;
    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[groupIdx[i]].targets[targetIdx[i]];
        target->usedPages = (__uint128_t)residentPages * target->quotaPages / quotaSum;
        assignedPages += target->usedPages;
    }

    uint64_t remainingPages = residentPages - assignedPages;
    while (remainingPages > 0) {
        bool progressed = false;
        for (int i = 0; i < entryCount && remainingPages > 0; i++) {
            GroupTargetAttr *target = &policy->groups[groupIdx[i]].targets[targetIdx[i]];
            if (target->usedPages >= target->quotaPages) {
                continue;
            }
            target->usedPages++;
            remainingPages--;
            progressed = true;
        }
        if (!progressed) {
            SMAP_LOGGER_ERROR("pid %d remote node %d used pages cannot fit quota.", pid, targetNid);
            return -EINVAL;
        }
    }

    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[groupIdx[i]].targets[targetIdx[i]];
        if (target->usedPages > target->quotaPages) {
            SMAP_LOGGER_ERROR("pid %d remote node %d used pages %llu exceed quota %llu.", pid, targetNid,
                              target->usedPages, target->quotaPages);
            return -EINVAL;
        }
        SMAP_LOGGER_INFO("pid %d remote node %d group %d target used pages %llu.", pid, targetNid, groupIdx[i],
                         target->usedPages);
    }
    return 0;
}

int InitGroupedUsedPages(pid_t pid, GroupMigrationPolicy *policy, const uint64_t numaPages[MAX_NODES])
{
    int nrLocalNuma = GetNrLocalNuma();
    for (int nid = nrLocalNuma; nid < MAX_NODES; nid++) {
        if (numaPages[nid] == 0) {
            continue;
        }
        int ret = InitGroupedTargetUsedPages(pid, policy, nid, numaPages[nid]);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static void SetLocalByNumaMaps(char *line, uint32_t *nodeBitmap, bool hugeFlag)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    char *substr = NULL;

    /*
     * It's possible that there are multiple Nx= in one line,
     * so it's necessary to traverse all node
     */
    for (i = 0; i < nrLocalNuma; i++) {
        if (hugeFlag && !IsNumaMapLineHuge(line)) {
            continue;
        }
        substr = strstr(line, g_nodePattern[i]);
        if (substr) {
            AddL1(nodeBitmap, i);
        }
    }
}

static int GetProcessNumaMapsObservation(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                         uint64_t numaPages[MAX_NODES])
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

static int CollectProcessCandidateObservation(pid_t pid, bool hugeFlag, ManagedLocalObservation *observation)
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

int ProcessAddManage(ProcessParam *param, uint32_t *nodeBitmap)
{
    ProcessAttr *current = g_processManager.processes;
    PidType type = GetPidType(&g_processManager);
    ProcessTargetConfig config;
    int ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        SMAP_LOGGER_ERROR("pid %d target config invalid: %d.", param ? param->pid : -1, ret);
        return ret;
    }

    ret = CheckPid(param->pid);
    if (ret) {
        SMAP_LOGGER_ERROR("pid %d check failed: %d.", param->pid, ret);
        return ret;
    }
    current = GetProcessAttrLocked(param->pid);
    if (current) {
        ret = ConfigureMigrationTargets(current, &config);
        if (ret) {
            SMAP_LOGGER_ERROR("Configure pid %d target failed: %d.", current->pid, ret);
            return ret;
        }
        if (current->pendingTargetConfigValid) {
            if (nodeBitmap) {
                current->pendingTargetNumaNodes = *nodeBitmap;
            }
            ret = SyncAllProcessConfig();
            if (ret) {
                SMAP_LOGGER_WARNING("Synchronize pending pid %d config maybe failed: %d.", current->pid, ret);
            }
            SMAP_LOGGER_INFO("Stage pid %d migration target update.", current->pid);
            return 0;
        }
        SMAP_LOGGER_INFO("Update pid %d migrate config, migrateMode: %d, remoteNumaCnt: %d.", current->pid,
                         current->migrateMode, current->remoteNumaCnt);
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", param->pid, ret);
        }
        for (int i = 0; i < param->count; i++) {
            SMAP_LOGGER_INFO("Update pid:%d success! migrateMode: %d, destnid: %d, memSize: %llu.", current->pid,
                             current->migrateMode, current->migrateParam[i].nid, current->migrateParam[i].memSize);
        }
    } else {
        ret = AddProcess(param, type, nodeBitmap);
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

int ProcessAddGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy)
{
    int ret = CheckPid(pid);
    if (ret) {
        SMAP_LOGGER_ERROR("grouped pid %d check failed: %d.", pid, ret);
        return ret;
    }
    if (!policy || !policy->enabled) {
        SMAP_LOGGER_ERROR("grouped policy of pid %d is invalid.", pid);
        return -EINVAL;
    }

    ProcessAttr *current = GetProcessAttrLocked(pid);
    if (current) {
        SetGroupedProcessConfig(current, pid, nodeBitmap, policy);
        SMAP_LOGGER_INFO("Update grouped pid %d success, group count %d.", pid, policy->groupCount);
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize grouped pid %d config maybe failed: %d.", pid, ret);
        }
        return 0;
    }

    if (g_processManager.nr[VM_TYPE] >= GetCurrentMaxNrPid()) {
        SMAP_LOGGER_ERROR("nr of grouped vm pid is out of limit.");
        return -EINVAL;
    }

    ProcessAttr *attr = calloc(1, sizeof(ProcessAttr));
    if (!attr) {
        SMAP_LOGGER_ERROR("Alloc memory for grouped process failed.");
        return -ENOMEM;
    }
    InitProcessMigrationTargetState(attr);
    attr->numaAttr.numaNodes = nodeBitmap;
    ret = VMPreprocess(pid, attr);
    if (ret) {
        SMAP_LOGGER_ERROR("Preprocess grouped VM process %d failed: %d.", pid, ret);
        free(attr);
        return ret;
    }
    SetGroupedProcessConfig(attr, pid, nodeBitmap, policy);
    LinkedListAdd(&g_processManager.processes, &attr);
    g_processManager.nr[VM_TYPE]++;
    SMAP_LOGGER_INFO("Add grouped pid %d success, group count %d.", pid, policy->groupCount);
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize grouped pid %d config maybe failed: %d.", pid, ret);
    }
    return 0;
}

int ProcessSetPendingGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy)
{
    if (!policy || !policy->enabled) {
        SMAP_LOGGER_ERROR("pending grouped policy of pid %d is invalid.", pid);
        return -EINVAL;
    }

    ProcessAttr *current = GetProcessAttrLocked(pid);
    if (!current || !current->groupPolicy.enabled || current->state != PROC_MIGRATE) {
        SMAP_LOGGER_ERROR("pid %d cannot save pending grouped policy.", pid);
        return -EINVAL;
    }

    /* Only an already-managed grouped PID in PROC_MIGRATE can defer refresh. */
    current->pendingGroupPolicy.valid = true;
    current->pendingGroupPolicy.nodeBitmap = nodeBitmap;
    current->pendingGroupPolicy.policy = *policy;
    SMAP_LOGGER_INFO("Save pending grouped policy for pid %d, group count %d.", pid, policy->groupCount);
    return 0;
}

int ApplyPendingGroupedPolicy(ProcessAttr *attr)
{
    if (!attr || !attr->pendingGroupPolicy.valid) {
        return 0;
    }

    GroupMigrationPolicy policy = attr->pendingGroupPolicy.policy;
    uint32_t nodeBitmap = attr->pendingGroupPolicy.nodeBitmap;
    uint64_t numaPages[MAX_NODES] = { 0 };

    /* Apply is atomic at manager level: initialize the new policy first. */
    ResetGroupedPolicyRuntime(&policy);
    int ret = GetPidNumaPagesFromNumaMaps(attr->pid, numaPages, true);
    if (ret) {
        SMAP_LOGGER_ERROR("Get pending grouped pid %d numa pages failed: %d.", attr->pid, ret);
        attr->pendingGroupPolicy.valid = false;
        return ret;
    }

    ret = InitGroupedUsedPages(attr->pid, &policy, numaPages);
    if (ret) {
        SMAP_LOGGER_ERROR("Init pending grouped pid %d used pages failed: %d.", attr->pid, ret);
        attr->pendingGroupPolicy.valid = false;
        return ret;
    }

    struct AccessAddPidPayload payload = {
        .type = NORMAL_SCAN,
        .pid = attr->pid,
        .scanTime = SCAN_TIME_2M,
        .numaNodes = nodeBitmap,
    };
    ret = AccessIoctlAddPid(1, &payload);
    if (ret) {
        SMAP_LOGGER_ERROR("Update pending grouped pid %d tracking failed: %d.", attr->pid, ret);
        return ret;
    }

    /* Tracking has accepted the new node scope; publish policy to manager. */
    SetGroupedProcessConfig(attr, attr->pid, nodeBitmap, &policy);
    attr->pendingGroupPolicy.valid = false;
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pending grouped pid %d config maybe failed: %d.", attr->pid, ret);
    }
    SMAP_LOGGER_INFO("Apply pending grouped policy for pid %d success.", attr->pid);
    return 0;
}

static void ClearRemoteMemUsed(void)
{
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        g_processManager.remoteNumaInfo.usedInfo[j].used = 0;
        g_processManager.remoteNumaInfo.usedInfo[j].ifUsedFreshed = true;
        for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
            g_processManager.remoteNumaInfo.privateUsedInfo[i][j].used = 0;
            g_processManager.remoteNumaInfo.privateUsedInfo[i][j].ifUsedFreshed = true;
        }
    }
    SMAP_LOGGER_DEBUG("Smap clear remote mem used end.");
}

static void CalRemoteMemUsed(void)
{
    ProcessAttr *attr = g_processManager.processes;
    struct RemoteNumaInfo *remoteNumaInfo = &g_processManager.remoteNumaInfo;
    int i, j;

    int nrLocal = g_processManager.nrLocalNuma;
    // 计算每个本地远端对应按照ratio可迁出的最大量
    while (attr) {
        for (j = 0; j < REMOTE_NUMA_NUM; j++) {
            remoteNumaInfo->usedInfo[j].used += attr->walkPage.nrPages[j + nrLocal];
            SMAP_LOGGER_DEBUG("usedInfo[%d].used: %llu, nrPages[%d+%d]: %u.", j, remoteNumaInfo->usedInfo[j].used, j,
                              nrLocal, attr->walkPage.nrPages[j + nrLocal]);
            for (i = 0; i < LOCAL_NUMA_NUM; i++) {
                remoteNumaInfo->privateUsedInfo[i][j].used += attr->strategyAttr.allocRemoteNrPages[i][j];
                SMAP_LOGGER_DEBUG("privateUsedInfo[%d][%d].used: %llu, allocRemoteNrPages[%d][%d]: %u.", i, j,
                                  remoteNumaInfo->privateUsedInfo[i][j].used, i, j,
                                  attr->strategyAttr.allocRemoteNrPages[i][j]);
            }
        }
        attr = attr->next;
    }
}

void CheckAndRemoveInvalidProcess(void)
{
    struct RemoteNumaInfo *numaInfo;
    PidType type = GetPidType(&g_processManager);

    EnvMutexLock(&g_processManager.lock);
    for (ProcessAttr *attr = g_processManager.processes; attr;) {
        pid_t pid = attr->pid;
        ProcessAttr *next = attr->next;
        SMAP_LOGGER_INFO("check if pid %d is valid.", pid);
        if (!PidIsValid(pid)) {
            // send ioctl to remove pid
            struct AccessRemovePidPayload payload = { .pid = pid };
            int ret = AccessIoctlRemovePid(1, &payload);
            if (ret) {
                SMAP_LOGGER_ERROR("access ioctl remove pid %d error: %d.", pid, ret);
            }

            LinkedListRemove(&attr, &g_processManager.processes);
            g_processManager.nr[type]--;
            ret = SyncAllProcessConfig();
            if (ret) {
                SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", pid, ret);
            }
            SMAP_LOGGER_INFO("remove pid %d from managed process.", pid);
        }
        attr = next;
    }
    if (!g_processManager.processes) {
        numaInfo = &g_processManager.remoteNumaInfo;
        EnvMutexLock(&numaInfo->lock);
        ClearRemoteMemUsed();
        SMAP_LOGGER_DEBUG("Remote memory usage cleared.");
        EnvMutexUnlock(&numaInfo->lock);
    }
    EnvMutexUnlock(&g_processManager.lock);
}

void RemoveManagedProcess(int nr, pid_t *pidArr)
{
    int ret;
    PidType type = GetPidType(&g_processManager);
    for (int i = 0; i < nr; i++) {
        ProcessAttr *attr = g_processManager.processes;
        while (attr && attr->pid != pidArr[i]) {
            attr = attr->next;
        }
        if (!attr) {
            SMAP_LOGGER_WARNING("pid: %d, not exist, not need to remove.", pidArr[i]);
            continue;
        }
        LinkedListRemove(&attr, &g_processManager.processes);
        SMAP_LOGGER_INFO("Remove pid: %d, from managed process.", pidArr[i]);
        g_processManager.nr[type]--;
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize pid %d config maybe failed: %d.", pidArr[i], ret);
        }
    }
}

void RemoveAllManagedProcess(void)
{
    int ret = AccessIoctlRemoveAllPid();
    if (ret) {
        SMAP_LOGGER_ERROR("access ioctl remove all pid error: %d.", ret);
    }
    EnvMutexLock(&g_processManager.lock);
    ProcessAttr *attr = g_processManager.processes;
    while (attr) {
        SMAP_LOGGER_INFO("During destruction remove pid: %d, from managed process.", attr->pid);
        LinkedListRemove(&attr, &g_processManager.processes);
        attr = g_processManager.processes;
    }
    EnvMutexUnlock(&g_processManager.lock);
    g_processManager.processes = NULL;
    g_processManager.nr[VM_TYPE] = g_processManager.nr[PROCESS_TYPE] = 0;
}

int DestroyProcessManager(void)
{
    RemoveAllManagedProcess();
    EnvMutexDestroy(&g_processManager.lock);
    EnvMutexDestroy(&g_processManager.threadLock);
    (void)memset_s(&g_processManager, sizeof(struct ProcessManager), 0, sizeof(struct ProcessManager));
    return 0;
}

static void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len)
{
    attr->walkPage.nrPage = 0;
    for (int i = 0; i < len; i++) {
        attr->walkPage.nrPages[i] = nrPages[i];
        attr->walkPage.nrPage += nrPages[i];
    }
    SMAP_LOGGER_INFO("Pid %d nrPage %llu.", attr->pid, attr->walkPage.nrPage);
}

#define FREQ_FILE_PATH_LEN 50

/**
 * CalcActcStats - 从actc_data数组计算统计数据
 * @attr: ProcessAttr结构体指针
 *
 * 遍历actc_data数组，计算freqMax、freqMin、freqNum、freqSum等统计数据。
 */
static void CalcActcStats(ProcessAttr *attr)
{
    uint16_t remoteHotThreshold = GetRemoteHotThreshold();
    int nrLocalNuma = GetNrLocalNuma();

    for (int nid = 0; nid < MAX_NODES; nid++) {
        uint64_t actcLen = attr->scanAttr.actcLen[nid];
        ActcData *actc = attr->scanAttr.actcData[nid];
        ActCount *count = &attr->scanAttr.actCount[nid];

        memset(count->freqBuckets, 0, sizeof(count->freqBuckets));
        memset(attr->scanAttr.selectedBuckets[nid], 0, sizeof(attr->scanAttr.selectedBuckets[nid]));

        if (actcLen == 0 || !actc) {
            memset(count, 0, sizeof(*count));
            continue;
        }

        count->freqMax = 0;
        count->freqMin = UINT8_MAX;
        count->freqNum = 0;
        count->freqSum = 0;
        count->remoteHotNum = 0;
        count->whiteNum = 0;
        count->pageNum = actcLen;
        count->freqZero = 0;

        for (uint64_t i = 0; i < actcLen; i++) {
            actc_t freq = actc[i].freq;
            uint16_t bucketIdx = MIN(freq, FREQ_BUCKETS_SIZE - 1);
            if (nid >= nrLocalNuma || !actc[i].isWhiteListPage) {
                count->freqBuckets[bucketIdx]++;
            }
            if (freq != 0) {
                count->freqNum++;
                count->freqSum += freq;
            } else {
                count->freqZero++;
            }
            if (freq >= remoteHotThreshold) {
                count->remoteHotNum++;
            }
            if (actc[i].isWhiteListPage) {
                count->whiteNum++;
            }
            count->freqMax = MAX(count->freqMax, freq);
            count->freqMin = MIN(count->freqMin, freq);
        }

        SMAP_LOGGER_INFO("[pid_stats] pid=%d node=%d actcLen=%llu freqMax=%u freqMin=%u freqNum=%llu freqSum=%llu "
                         "remoteHotNum=%llu whiteNum=%llu",
                         attr->pid, nid, actcLen, count->freqMax, count->freqMin, count->freqNum, count->freqSum,
                         count->remoteHotNum, count->whiteNum);

        /* 打印各频次桶的页面数（仅本地NUMA，跳过页面数为零的） */
        if (nid < nrLocalNuma) {
            for (int f = 0; f < FREQ_BUCKETS_SIZE; f++) {
                if (count->freqBuckets[f] > 0) {
                    SMAP_LOGGER_DEBUG("Node%d freq=%d pages=%u", nid, f, count->freqBuckets[f]);
                }
            }
        }
    }
}

static uint32_t BuildPairCapacityLocalMask(const ProcessAttr *attr, int remoteIndex)
{
    uint32_t capacityLocalMask = 0;
    uint32_t managedLocalMask = attr->managedLocalState.managedLocalMask;
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;
    int nrLocalNuma = GetNrLocalNuma();

    EnvMutexLock(&numaInfo->lock);
    for (int local = 0; local < nrLocalNuma && local < LOCAL_NUMA_NUM; local++) {
        if (numaInfo->privateSize[local][remoteIndex] > 0) {
            AddL1(&capacityLocalMask, local);
        }
    }
    if (numaInfo->sharedSize[remoteIndex] > 0) {
        capacityLocalMask |= managedLocalMask;
    }
    EnvMutexUnlock(&numaInfo->lock);
    return capacityLocalMask;
}

static void DistributePairAccount(ProcessAttr *attr, int remoteIndex, uint32_t localMask,
                                  const uint32_t weights[LOCAL_NUMA_NUM], uint64_t weightTotal,
                                  uint32_t actualRemotePages)
{
    StrategyAttribute *sa = &attr->strategyAttr;
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t allocated = 0;

    for (int local = 0; local < nrLocalNuma && local < LOCAL_NUMA_NUM; local++) {
        sa->remoteNrPagesAfterMigrate[local][remoteIndex] = 0;
        if (!InL1(localMask, local)) {
            continue;
        }
        uint32_t pages = (uint64_t)actualRemotePages * weights[local] / weightTotal;
        sa->remoteNrPagesAfterMigrate[local][remoteIndex] = pages;
        allocated += pages;
    }

    uint32_t remainder = actualRemotePages - allocated;
    for (int local = 0; local < nrLocalNuma && local < LOCAL_NUMA_NUM && remainder > 0; local++) {
        if (!InL1(localMask, local) || weights[local] == 0) {
            continue;
        }
        sa->remoteNrPagesAfterMigrate[local][remoteIndex]++;
        remainder--;
    }
}

static void RebuildPairAccount(ProcessAttr *attr, int remoteIndex, uint32_t actualRemotePages)
{
    uint32_t allLocalMask = BuildAllLocalNumaMask();
    uint32_t managedLocalMask = attr->managedLocalState.managedLocalMask & allLocalMask;
    uint32_t capacityLocalMask = BuildPairCapacityLocalMask(attr, remoteIndex);
    uint32_t rebuildLocalMask = managedLocalMask & (capacityLocalMask | attr->managedLocalState.residentLocalMask);

    if (rebuildLocalMask == 0) {
        rebuildLocalMask = managedLocalMask;
    }
    if (rebuildLocalMask == 0) {
        rebuildLocalMask = allLocalMask;
        SMAP_LOGGER_WARNING("Pid %d remote %d has %u resident pages but no managed local; "
                            "rebuilding account on all online local nodes.",
                            attr->pid, remoteIndex + GetNrLocalNuma(), actualRemotePages);
    }

    uint32_t weights[LOCAL_NUMA_NUM] = { 0 };
    uint64_t weightTotal = 0;
    int localCount = 0;
    int nrLocalNuma = GetNrLocalNuma();
    for (int local = 0; local < nrLocalNuma && local < LOCAL_NUMA_NUM; local++) {
        if (!InL1(rebuildLocalMask, local)) {
            continue;
        }
        weights[local] = attr->walkPage.nrPages[local];
        weightTotal += weights[local];
        localCount++;
    }

    if (weightTotal == 0) {
        for (int local = 0; local < nrLocalNuma && local < LOCAL_NUMA_NUM; local++) {
            if (InL1(rebuildLocalMask, local)) {
                weights[local] = 1;
            }
        }
        weightTotal = localCount;
    }

    DistributePairAccount(attr, remoteIndex, rebuildLocalMask, weights, weightTotal, actualRemotePages);
}

/*
 * Reconcile the explanatory Pair account with one consistent page snapshot.
 * The caller must hold g_processManager.lock.
 */
void CalibratePairAccount(ProcessAttr *attr)
{
    if (!attr) {
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    int nrLocalNuma = GetNrLocalNuma();
    if (nrLocalNuma <= 0 || nrLocalNuma > LOCAL_NUMA_NUM) {
        SMAP_LOGGER_ERROR("Cannot calibrate pid %d Pair account with %d local NUMA nodes.", attr->pid, nrLocalNuma);
        return;
    }

    for (int remoteIndex = 0; remoteIndex < REMOTE_NUMA_NUM; remoteIndex++) {
        int remoteNid = nrLocalNuma + remoteIndex;
        if (remoteNid >= MAX_NODES) {
            break;
        }

        uint64_t accountTotal = 0;
        uint32_t accountLocalMask = 0;
        uint32_t weights[LOCAL_NUMA_NUM] = { 0 };
        for (int local = 0; local < nrLocalNuma; local++) {
            uint32_t pages = sa->remoteNrPagesAfterMigrate[local][remoteIndex];
            weights[local] = pages;
            accountTotal += pages;
            if (pages > 0) {
                AddL1(&accountLocalMask, local);
            }
        }
        for (int local = nrLocalNuma; local < LOCAL_NUMA_NUM; local++) {
            sa->remoteNrPagesAfterMigrate[local][remoteIndex] = 0;
        }

        uint32_t actualRemotePages = attr->walkPage.nrPages[remoteNid];
        if (accountTotal != actualRemotePages) {
            if (actualRemotePages == 0) {
                for (int local = 0; local < nrLocalNuma; local++) {
                    sa->remoteNrPagesAfterMigrate[local][remoteIndex] = 0;
                }
            } else if (accountTotal > 0) {
                DistributePairAccount(attr, remoteIndex, accountLocalMask, weights, accountTotal, actualRemotePages);
            } else {
                RebuildPairAccount(attr, remoteIndex, actualRemotePages);
            }
            SMAP_LOGGER_INFO("Calibrated pid %d remote %d Pair account from %llu to %u pages.", attr->pid, remoteNid,
                             accountTotal, actualRemotePages);
        }

        attr->managedLocalState.accountLocalMask[remoteIndex] = BuildAccountLocalMask(attr, remoteIndex);
        attr->managedLocalState.managedLocalMask |= attr->managedLocalState.accountLocalMask[remoteIndex];
    }
    attr->managedLocalState.managedLocalMask &= BuildAllLocalNumaMask();
}

static uint64_t PairRequestMulDiv(uint64_t value, uint64_t weight, uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    return (uint64_t)(((unsigned __int128)value * weight) / total);
}

static void DistributePairRequestPages(uint64_t pages, const uint64_t weights[], int count, uint64_t allocation[])
{
    uint64_t weightTotal = 0;
    for (int i = 0; i < count; i++) {
        weightTotal += weights[i];
    }
    pages = MIN(pages, weightTotal);

    uint64_t allocated = 0;
    for (int i = 0; i < count; i++) {
        allocation[i] = PairRequestMulDiv(pages, weights[i], weightTotal);
        allocated += allocation[i];
    }
    for (int i = 0; i < count && allocated < pages; i++) {
        if (allocation[i] >= weights[i]) {
            continue;
        }
        allocation[i]++;
        allocated++;
    }
}

static int ValidatePairRequestInput(const ProcessAttr *attr, const PairRequestContext *context)
{
    if (!attr || !context || context->nrLocalNuma <= 0 || context->nrLocalNuma > LOCAL_NUMA_NUM ||
        context->pageSizeKB == 0 || context->nrLocalNuma + REMOTE_NUMA_NUM > MAX_NODES) {
        return -EINVAL;
    }

    const ProcessTargetConfig *config = &attr->targetConfig;
    if (config->count > REMOTE_NUMA_NUM ||
        (config->migrateMode != MIG_RATIO_MODE && config->migrateMode != MIG_MEMSIZE_MODE)) {
        return -EINVAL;
    }

    bool remoteSeen[REMOTE_NUMA_NUM] = { false };
    uint64_t totalRatio = 0;
    for (uint32_t i = 0; i < config->count; i++) {
        int remoteIndex;
        if (RemoteNidToIndex(config->targets[i].remoteNid, context->nrLocalNuma, &remoteIndex) ||
            remoteSeen[remoteIndex]) {
            return -EINVAL;
        }
        remoteSeen[remoteIndex] = true;

        if (config->migrateMode == MIG_RATIO_MODE) {
            if (config->targets[i].ratio > HUNDRED) {
                return -EINVAL;
            }
            totalRatio += config->targets[i].ratio;
            continue;
        }

        if (config->targets[i].memSizeKB % context->pageSizeKB != 0 ||
            config->targets[i].memSizeKB / context->pageSizeKB > UINT32_MAX) {
            return -EINVAL;
        }
    }

    if (config->migrateMode == MIG_RATIO_MODE && totalRatio > HUNDRED) {
        return -EINVAL;
    }
    return 0;
}

static uint64_t BuildManagedTotalPages(const ProcessAttr *attr, int nrLocalNuma)
{
    uint64_t managedTotalPages = 0;
    uint32_t managedLocalMask = attr->managedLocalState.managedLocalMask;

    for (int local = 0; local < nrLocalNuma; local++) {
        if (InL1(managedLocalMask, local)) {
            managedTotalPages += attr->walkPage.nrPages[local];
        }
    }
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        managedTotalPages += attr->walkPage.nrPages[nrLocalNuma + remote];
    }
    return managedTotalPages;
}

static void BuildRequestedRemotePages(const ProcessAttr *attr, const PairRequestContext *context,
                                      PairRequestSummary *summary)
{
    const ProcessTargetConfig *config = &attr->targetConfig;

    for (uint32_t i = 0; i < config->count; i++) {
        int remoteIndex;
        (void)RemoteNidToIndex(config->targets[i].remoteNid, context->nrLocalNuma, &remoteIndex);
        if (config->migrateMode == MIG_RATIO_MODE) {
            summary->requestedRemotePages[remoteIndex] =
                PairRequestMulDiv(summary->managedTotalPages, config->targets[i].ratio, HUNDRED);
        } else {
            summary->requestedRemotePages[remoteIndex] = config->targets[i].memSizeKB / context->pageSizeKB;
        }
    }
}

static void BuildEffectiveRemotePages(const ProcessAttr *attr, const PairRequestContext *context,
                                      PairRequestSummary *summary)
{
    uint64_t remainingDemand[REMOTE_NUMA_NUM] = { 0 };
    uint64_t baselineTotal = 0;
    uint64_t demandTotal = 0;

    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        int remoteNid = context->nrLocalNuma + remote;
        uint64_t actualPages = attr->walkPage.nrPages[remoteNid];
        uint64_t requestedPages = summary->requestedRemotePages[remote];
        uint64_t baseline = MIN(actualPages, requestedPages);

        summary->effectiveRemotePages[remote] = baseline;
        remainingDemand[remote] = requestedPages - baseline;
        baselineTotal += baseline;
        demandTotal += remainingDemand[remote];
    }

    uint64_t availablePages = summary->managedTotalPages > baselineTotal ? summary->managedTotalPages - baselineTotal :
                                                                           0;
    uint64_t pagesToAllocate = MIN(availablePages, demandTotal);
    uint64_t allocation[REMOTE_NUMA_NUM] = { 0 };
    DistributePairRequestPages(pagesToAllocate, remainingDemand, REMOTE_NUMA_NUM, allocation);
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        summary->effectiveRemotePages[remote] += allocation[remote];
    }
}

static uint32_t BuildEligiblePairMask(const ProcessAttr *attr, const PairRequestContext *context, int remote)
{
    uint32_t allLocalMask = (1U << context->nrLocalNuma) - 1U;

    return attr->managedLocalState.managedLocalMask & context->capacityLocalMask[remote] & allLocalMask;
}

static void PreservePairAccounts(const ProcessAttr *attr, const PairRequestContext *context,
                                 const PairRequestSummary *summary,
                                 uint64_t pairRequests[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM])
{
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        uint64_t accountTotal = 0;
        uint64_t accounts[LOCAL_NUMA_NUM] = { 0 };
        uint32_t eligibleMask = BuildEligiblePairMask(attr, context, remote);
        for (int local = 0; local < context->nrLocalNuma; local++) {
            if (!InL1(eligibleMask, local)) {
                continue;
            }
            accounts[local] = attr->strategyAttr.remoteNrPagesAfterMigrate[local][remote];
            accountTotal += accounts[local];
        }

        uint64_t target = summary->effectiveRemotePages[remote];
        if (accountTotal <= target) {
            for (int local = 0; local < context->nrLocalNuma; local++) {
                pairRequests[local][remote] = accounts[local];
            }
            continue;
        }

        uint64_t allocation[LOCAL_NUMA_NUM] = { 0 };
        DistributePairRequestPages(target, accounts, context->nrLocalNuma, allocation);
        for (int local = 0; local < context->nrLocalNuma; local++) {
            pairRequests[local][remote] = allocation[local];
        }
    }
}

static void BuildLocalRemainingPages(const ProcessAttr *attr, const PairRequestContext *context,
                                     uint64_t pairRequests[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                     uint64_t remainingPages[LOCAL_NUMA_NUM])
{
    for (int local = 0; local < context->nrLocalNuma; local++) {
        uint64_t allocatablePages = attr->walkPage.nrPages[local];
        uint64_t requestedPages = 0;

        for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
            allocatablePages += attr->strategyAttr.remoteNrPagesAfterMigrate[local][remote];
            requestedPages += pairRequests[local][remote];
        }
        remainingPages[local] = allocatablePages > requestedPages ? allocatablePages - requestedPages : 0;
    }
}

static uint32_t BuildActivePairMask(const ProcessAttr *attr, const PairRequestContext *context, int remote)
{
    return BuildEligiblePairMask(attr, context, remote) & attr->managedLocalState.residentLocalMask;
}

static void AllocateRemotePairRequest(const ProcessAttr *attr, const PairRequestContext *context, int remote,
                                      uint64_t requestedPages, uint64_t pairRequests[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                      uint64_t remainingPages[LOCAL_NUMA_NUM])
{
    uint64_t currentPages = 0;
    uint64_t weights[LOCAL_NUMA_NUM] = { 0 };
    uint32_t activeMask = BuildActivePairMask(attr, context, remote);

    for (int local = 0; local < context->nrLocalNuma; local++) {
        currentPages += pairRequests[local][remote];
        if (!InL1(activeMask, local) || attr->walkPage.nrPages[local] == 0 ||
            pairRequests[local][remote] >= UINT32_MAX) {
            continue;
        }

        uint64_t pairRoom = UINT32_MAX - pairRequests[local][remote];
        weights[local] = MIN(remainingPages[local], pairRoom);
    }

    if (currentPages >= requestedPages) {
        return;
    }

    uint64_t allocation[LOCAL_NUMA_NUM] = { 0 };
    DistributePairRequestPages(requestedPages - currentPages, weights, context->nrLocalNuma, allocation);
    for (int local = 0; local < context->nrLocalNuma; local++) {
        pairRequests[local][remote] += allocation[local];
        remainingPages[local] -= allocation[local];
    }
}

enum {
    PAIR_FLOW_SOURCE = 0,
    PAIR_FLOW_LOCAL_BASE = 1,
    PAIR_FLOW_REMOTE_BASE = PAIR_FLOW_LOCAL_BASE + LOCAL_NUMA_NUM,
    PAIR_FLOW_SINK = PAIR_FLOW_REMOTE_BASE + REMOTE_NUMA_NUM,
    PAIR_FLOW_NODE_COUNT,
};

static bool FindPairRequestAugmentingPath(uint64_t residual[PAIR_FLOW_NODE_COUNT][PAIR_FLOW_NODE_COUNT],
                                          int parent[PAIR_FLOW_NODE_COUNT])
{
    bool visited[PAIR_FLOW_NODE_COUNT] = { false };
    int queue[PAIR_FLOW_NODE_COUNT] = { 0 };
    int head = 0;
    int tail = 0;

    parent[PAIR_FLOW_SOURCE] = -1;
    visited[PAIR_FLOW_SOURCE] = true;
    queue[tail++] = PAIR_FLOW_SOURCE;
    while (head < tail) {
        int current = queue[head++];
        for (int next = 0; next < PAIR_FLOW_NODE_COUNT; next++) {
            if (visited[next] || residual[current][next] == 0) {
                continue;
            }
            parent[next] = current;
            visited[next] = true;
            if (next == PAIR_FLOW_SINK) {
                return true;
            }
            queue[tail++] = next;
        }
    }
    return false;
}

static void CompletePairRequestFlow(uint64_t residual[PAIR_FLOW_NODE_COUNT][PAIR_FLOW_NODE_COUNT])
{
    int parent[PAIR_FLOW_NODE_COUNT] = { 0 };

    while (FindPairRequestAugmentingPath(residual, parent)) {
        uint64_t augment = UINT64_MAX;
        for (int node = PAIR_FLOW_SINK; node != PAIR_FLOW_SOURCE; node = parent[node]) {
            augment = MIN(augment, residual[parent[node]][node]);
        }
        for (int node = PAIR_FLOW_SINK; node != PAIR_FLOW_SOURCE; node = parent[node]) {
            int previous = parent[node];
            residual[previous][node] -= augment;
            residual[node][previous] += augment;
        }
    }
}

static int CompletePairRequestAllocation(const ProcessAttr *attr, const PairRequestContext *context,
                                         const PairRequestSummary *summary,
                                         uint64_t baseline[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                         uint64_t pairRequests[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                         uint64_t remainingPages[LOCAL_NUMA_NUM])
{
    uint64_t residual[PAIR_FLOW_NODE_COUNT][PAIR_FLOW_NODE_COUNT] = { 0 };
    uint64_t remoteFlow[REMOTE_NUMA_NUM] = { 0 };

    for (int local = 0; local < context->nrLocalNuma; local++) {
        uint64_t localFlow = 0;
        int localNode = PAIR_FLOW_LOCAL_BASE + local;
        for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
            if (pairRequests[local][remote] < baseline[local][remote]) {
                return -ERANGE;
            }
            uint64_t flow = pairRequests[local][remote] - baseline[local][remote];
            uint32_t activeMask = BuildActivePairMask(attr, context, remote);
            bool active = InL1(activeMask, local) && attr->walkPage.nrPages[local] > 0 &&
                          baseline[local][remote] < UINT32_MAX;
            if (flow > 0 && !active) {
                return -ERANGE;
            }
            if (!active) {
                continue;
            }
            int remoteNode = PAIR_FLOW_REMOTE_BASE + remote;
            residual[localNode][remoteNode] = UINT32_MAX - pairRequests[local][remote];
            residual[remoteNode][localNode] = flow;
            localFlow += flow;
            remoteFlow[remote] += flow;
        }
        residual[PAIR_FLOW_SOURCE][localNode] = remainingPages[local];
        residual[localNode][PAIR_FLOW_SOURCE] = localFlow;
    }

    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        uint64_t currentPages = 0;
        for (int local = 0; local < context->nrLocalNuma; local++) {
            currentPages += pairRequests[local][remote];
        }
        if (currentPages > summary->effectiveRemotePages[remote]) {
            return -ERANGE;
        }
        int remoteNode = PAIR_FLOW_REMOTE_BASE + remote;
        residual[remoteNode][PAIR_FLOW_SINK] = summary->effectiveRemotePages[remote] - currentPages;
        residual[PAIR_FLOW_SINK][remoteNode] = remoteFlow[remote];
    }

    CompletePairRequestFlow(residual);

    for (int local = 0; local < context->nrLocalNuma; local++) {
        int localNode = PAIR_FLOW_LOCAL_BASE + local;
        for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
            int remoteNode = PAIR_FLOW_REMOTE_BASE + remote;
            pairRequests[local][remote] = baseline[local][remote] + residual[remoteNode][localNode];
        }
    }
    return 0;
}

/*
 * Assign an aggregate request to currently eligible Pairs from one calibrated
 * page/account snapshot. Unassignable demand remains explicit in summary.
 * This function performs no I/O, locking, or global capacity consumption.
 */
int BuildPairRequestedTargets(const ProcessAttr *attr, const PairRequestContext *context, PairTarget targets[],
                              size_t targetCap, size_t *targetCnt, PairRequestSummary *summary)
{
    if (!targets || !targetCnt || !summary) {
        return -EINVAL;
    }
    *targetCnt = 0;

    int ret = ValidatePairRequestInput(attr, context);
    if (ret) {
        return ret;
    }

    PairRequestSummary result = { 0 };
    result.managedTotalPages = BuildManagedTotalPages(attr, context->nrLocalNuma);
    BuildRequestedRemotePages(attr, context, &result);
    BuildEffectiveRemotePages(attr, context, &result);

    uint64_t pairRequests[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    PreservePairAccounts(attr, context, &result, pairRequests);
    uint64_t baseline[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    ret = memcpy_s(baseline, sizeof(baseline), pairRequests, sizeof(pairRequests));
    if (ret != EOK) {
        return -ret;
    }

    uint64_t remainingPages[LOCAL_NUMA_NUM] = { 0 };
    BuildLocalRemainingPages(attr, context, pairRequests, remainingPages);
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        AllocateRemotePairRequest(attr, context, remote, result.effectiveRemotePages[remote], pairRequests,
                                  remainingPages);
    }
    ret = CompletePairRequestAllocation(attr, context, &result, baseline, pairRequests, remainingPages);
    if (ret) {
        return ret;
    }
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        uint64_t assignedPages = 0;
        for (int local = 0; local < context->nrLocalNuma; local++) {
            assignedPages += pairRequests[local][remote];
        }
        if (assignedPages > result.effectiveRemotePages[remote]) {
            return -ERANGE;
        }
        result.unassignedRequestedPages[remote] = result.effectiveRemotePages[remote] - assignedPages;
    }

    size_t requiredCount = 0;
    for (int local = 0; local < context->nrLocalNuma; local++) {
        for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
            uint32_t account = attr->strategyAttr.remoteNrPagesAfterMigrate[local][remote];
            if (account > 0 || pairRequests[local][remote] > 0) {
                requiredCount++;
            }
        }
    }
    if (requiredCount > targetCap) {
        return -ENOSPC;
    }

    size_t count = 0;
    for (int local = 0; local < context->nrLocalNuma; local++) {
        for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
            uint32_t account = attr->strategyAttr.remoteNrPagesAfterMigrate[local][remote];
            if (account == 0 && pairRequests[local][remote] == 0) {
                continue;
            }
            targets[count++] = (PairTarget){
                .pid = attr->pid,
                .localNid = local,
                .remoteNid = context->nrLocalNuma + remote,
                .requestedPages = (uint32_t)pairRequests[local][remote],
                .targetPages = 0,
            };
        }
    }

    *summary = result;
    *targetCnt = count;
    return 0;
}

typedef struct {
    PairTarget target;
    const ProcessAttr *process;
    uint32_t actualPages;
    uint32_t privatePages;
    uint32_t sharedPages;
} PairArbitrationEntry;

typedef enum {
    PAIR_PRIVATE_BASELINE,
    PAIR_PRIVATE_REMAINDER,
    PAIR_SHARED_BASELINE,
    PAIR_SHARED_REMAINDER,
} PairCapacityPhase;

static int ComparePairArbitrationEntry(const void *left, const void *right)
{
    const PairTarget *lhs = &((const PairArbitrationEntry *)left)->target;
    const PairTarget *rhs = &((const PairArbitrationEntry *)right)->target;

    if (lhs->pid != rhs->pid) {
        return lhs->pid < rhs->pid ? -1 : 1;
    }
    if (lhs->localNid != rhs->localNid) {
        return lhs->localNid < rhs->localNid ? -1 : 1;
    }
    if (lhs->remoteNid != rhs->remoteNid) {
        return lhs->remoteNid < rhs->remoteNid ? -1 : 1;
    }
    return 0;
}

static uint64_t PairCapacityDemand(const PairArbitrationEntry *entry, PairCapacityPhase phase)
{
    if (entry->process->ignoreRemoteCapacity) {
        /* Explicit sync targets must not consume configured Pair capacity. */
        return 0;
    }

    uint64_t requested = entry->target.requestedPages;
    uint64_t baseline = MIN(entry->actualPages, requested);
    uint64_t allocated;

    switch (phase) {
        case PAIR_PRIVATE_BASELINE:
            return baseline;
        case PAIR_PRIVATE_REMAINDER:
            return requested - entry->privatePages;
        case PAIR_SHARED_BASELINE:
            allocated = entry->privatePages + entry->sharedPages;
            return baseline > allocated ? baseline - allocated : 0;
        case PAIR_SHARED_REMAINDER:
            allocated = entry->privatePages + entry->sharedPages;
            return requested - allocated;
        default:
            return 0;
    }
}

static bool PairMatchesCapacityPool(const PairArbitrationEntry *entry, int localNid, int remoteNid)
{
    return entry->target.remoteNid == remoteNid && (localNid < 0 || entry->target.localNid == localNid);
}

static void AddPairCapacityAllocation(PairArbitrationEntry *entry, PairCapacityPhase phase, uint64_t pages)
{
    if (phase == PAIR_PRIVATE_BASELINE || phase == PAIR_PRIVATE_REMAINDER) {
        entry->privatePages += (uint32_t)pages;
    } else {
        entry->sharedPages += (uint32_t)pages;
    }
}

/*
 * Proportionally allocate one capacity tier. Entries are already sorted by
 * pid, local nid and remote nid, so integer remainders are deterministic and
 * independent of the process-list traversal order.
 */
static uint64_t AllocatePairCapacityPhase(PairArbitrationEntry entries[], size_t entryCount, int localNid,
                                          int remoteNid, uint64_t capacity, PairCapacityPhase phase)
{
    uint64_t totalDemand = 0;
    for (size_t i = 0; i < entryCount; i++) {
        if (!PairMatchesCapacityPool(&entries[i], localNid, remoteNid)) {
            continue;
        }
        totalDemand += PairCapacityDemand(&entries[i], phase);
    }

    uint64_t pagesToAllocate = MIN(capacity, totalDemand);
    if (pagesToAllocate == 0) {
        return 0;
    }

    uint64_t allocated = 0;
    for (size_t i = 0; i < entryCount; i++) {
        if (!PairMatchesCapacityPool(&entries[i], localNid, remoteNid)) {
            continue;
        }
        uint64_t demand = PairCapacityDemand(&entries[i], phase);
        uint64_t pages = (uint64_t)(((unsigned __int128)pagesToAllocate * demand) / totalDemand);
        AddPairCapacityAllocation(&entries[i], phase, pages);
        allocated += pages;
    }

    for (size_t i = 0; i < entryCount && allocated < pagesToAllocate; i++) {
        if (!PairMatchesCapacityPool(&entries[i], localNid, remoteNid) || PairCapacityDemand(&entries[i], phase) == 0) {
            continue;
        }
        AddPairCapacityAllocation(&entries[i], phase, 1);
        allocated++;
    }
    return allocated;
}

static int RemoteCapacityToPages(uint64_t sizeMB, uint64_t pageSize, uint64_t *pages)
{
    if (!pages || pageSize == 0 || sizeMB > UINT64_MAX / MIB) {
        return -EOVERFLOW;
    }
    *pages = sizeMB * MIB / pageSize;
    return 0;
}

static int BuildPairCapacitySnapshot(const struct ProcessManager *manager,
                                     uint64_t privatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                     uint64_t sharedPages[REMOTE_NUMA_NUM], uint64_t totalPages[REMOTE_NUMA_NUM])
{
    uint64_t pageSize = manager->tracking.pageSize;
    if (pageSize == 0 || pageSize % KIB != 0) {
        return -EINVAL;
    }

    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        int ret = RemoteCapacityToPages(manager->remoteNumaInfo.sharedSize[remote], pageSize, &sharedPages[remote]);
        if (ret) {
            return ret;
        }
        totalPages[remote] = sharedPages[remote];
        for (int local = 0; local < manager->nrLocalNuma; local++) {
            ret = RemoteCapacityToPages(manager->remoteNumaInfo.privateSize[local][remote], pageSize,
                                        &privatePages[local][remote]);
            if (ret) {
                return ret;
            }
            if (privatePages[local][remote] > UINT64_MAX - totalPages[remote]) {
                return -EOVERFLOW;
            }
            totalPages[remote] += privatePages[local][remote];
        }
    }
    return 0;
}

static void BuildPairRequestContext(const struct ProcessManager *manager,
                                    const uint64_t privatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                    const uint64_t sharedPages[REMOTE_NUMA_NUM], const ProcessAttr *attr,
                                    PairRequestContext *context)
{
    context->nrLocalNuma = manager->nrLocalNuma;
    context->pageSizeKB = manager->tracking.pageSize / KIB;
    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        for (int local = 0; local < manager->nrLocalNuma; local++) {
            if (privatePages[local][remote] > 0) {
                AddL1(&context->capacityLocalMask[remote], local);
            }
        }
        if (sharedPages[remote] > 0) {
            context->capacityLocalMask[remote] |= attr->managedLocalState.managedLocalMask;
        }
        if (attr->ignoreRemoteCapacity) {
            /* Sync migration can assign its target from every managed local NUMA. */
            context->capacityLocalMask[remote] |= attr->managedLocalState.managedLocalMask;
        }
    }
}

static int CollectAllPairRequests(const struct ProcessManager *manager,
                                  const uint64_t privatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                  const uint64_t sharedPages[REMOTE_NUMA_NUM], PairArbitrationEntry entries[],
                                  size_t entryCap, size_t *entryCount, bool migrateOnly)
{
    size_t count = 0;
    for (const ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (attr->scanType != NORMAL_SCAN || attr->groupPolicy.enabled ||
            (migrateOnly && attr->state != PROC_MIGRATE)) {
            continue;
        }

        PairRequestContext context = { 0 };
        BuildPairRequestContext(manager, privatePages, sharedPages, attr, &context);
        PairTarget processTargets[LOCAL_NUMA_NUM * REMOTE_NUMA_NUM] = { 0 };
        PairRequestSummary summary = { 0 };
        size_t processTargetCount = 0;
        int ret = BuildPairRequestedTargets(attr, &context, processTargets, LOCAL_NUMA_NUM * REMOTE_NUMA_NUM,
                                            &processTargetCount, &summary);
        if (ret) {
            SMAP_LOGGER_ERROR("Build pid %d Pair requests failed: %d.", attr->pid, ret);
            return ret;
        }
        if (processTargetCount > entryCap - count) {
            return -ENOSPC;
        }

        for (size_t i = 0; i < processTargetCount; i++) {
            int remoteIndex = processTargets[i].remoteNid - manager->nrLocalNuma;
            entries[count].target = processTargets[i];
            entries[count].process = attr;
            entries[count].actualPages =
                attr->strategyAttr.remoteNrPagesAfterMigrate[processTargets[i].localNid][remoteIndex];
            count++;
        }
    }

    if (count > 1) {
        qsort(entries, count, sizeof(entries[0]), ComparePairArbitrationEntry);
    }
    *entryCount = count;
    return 0;
}

static void ArbitrateAllPairCapacity(PairArbitrationEntry entries[], size_t entryCount, int nrLocalNuma,
                                     const uint64_t privateCapacity[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                     const uint64_t sharedCapacity[REMOTE_NUMA_NUM])
{
    for (size_t i = 0; i < entryCount; i++) {
        if (entries[i].process->ignoreRemoteCapacity) {
            /* Preserve migrate_out_sync's legacy target semantics. */
            entries[i].privatePages = entries[i].target.requestedPages;
        }
    }

    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        int remoteNid = nrLocalNuma + remote;
        for (int local = 0; local < nrLocalNuma; local++) {
            uint64_t remaining = privateCapacity[local][remote];
            uint64_t used =
                AllocatePairCapacityPhase(entries, entryCount, local, remoteNid, remaining, PAIR_PRIVATE_BASELINE);
            remaining -= used;
            (void)AllocatePairCapacityPhase(entries, entryCount, local, remoteNid, remaining, PAIR_PRIVATE_REMAINDER);
        }

        uint64_t remaining = sharedCapacity[remote];
        uint64_t used = AllocatePairCapacityPhase(entries, entryCount, -1, remoteNid, remaining, PAIR_SHARED_BASELINE);
        remaining -= used;
        (void)AllocatePairCapacityPhase(entries, entryCount, -1, remoteNid, remaining, PAIR_SHARED_REMAINDER);
    }
}

static void PublishPairCapacityResult(struct ProcessManager *manager, PairArbitrationEntry entries[], size_t entryCount,
                                      const uint64_t privateCapacity[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                      const uint64_t totalCapacity[REMOTE_NUMA_NUM])
{
    uint64_t privateUsed[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    uint64_t totalUsed[REMOTE_NUMA_NUM] = { 0 };

    for (size_t i = 0; i < entryCount; i++) {
        int remote = entries[i].target.remoteNid - manager->nrLocalNuma;
        int local = entries[i].target.localNid;
        entries[i].target.targetPages = entries[i].privatePages + entries[i].sharedPages;
        /*
         * RemoteNumaUsedInfo is consumed by migrate-back readiness checks, so
         * it must describe current residency rather than the clipped target.
         */
        privateUsed[local][remote] += entries[i].actualPages;
        totalUsed[remote] += entries[i].actualPages;
    }

    for (int remote = 0; remote < REMOTE_NUMA_NUM; remote++) {
        struct RemoteNumaUsedInfo *usedInfo = &manager->remoteNumaInfo.usedInfo[remote];
        usedInfo->size = totalCapacity[remote];
        usedInfo->used = totalUsed[remote];
        usedInfo->ifUsedFreshed = true;
        for (int local = 0; local < LOCAL_NUMA_NUM; local++) {
            struct RemoteNumaUsedInfo *privateInfo = &manager->remoteNumaInfo.privateUsedInfo[local][remote];
            privateInfo->size = privateCapacity[local][remote];
            privateInfo->used = privateUsed[local][remote];
            privateInfo->ifUsedFreshed = true;
        }
    }
}

static int BuildPairArbitrationSnapshotLocked(struct ProcessManager *manager, PairArbitrationEntry entries[],
                                              size_t entryCap, size_t *entryCount,
                                              uint64_t privateCapacity[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                              uint64_t totalCapacity[REMOTE_NUMA_NUM], bool migrateOnly)
{
    uint64_t sharedCapacity[REMOTE_NUMA_NUM] = { 0 };
    int ret = BuildPairCapacitySnapshot(manager, privateCapacity, sharedCapacity, totalCapacity);
    if (ret) {
        return ret;
    }
    ret = CollectAllPairRequests(manager, privateCapacity, sharedCapacity, entries, entryCap, entryCount, migrateOnly);
    if (ret) {
        return ret;
    }
    ArbitrateAllPairCapacity(entries, *entryCount, manager->nrLocalNuma, privateCapacity, sharedCapacity);
    return 0;
}

/*
 * Build and arbitrate all normal-process Pair targets from one manager
 * snapshot. The function owns the manager -> remote capacity lock order.
 */
int BuildAllPairTargets(struct ProcessManager *manager, PairTarget targets[], size_t targetCap, size_t *targetCnt)
{
    if (!manager || !targets || !targetCnt || manager->nrLocalNuma == 0 || manager->nrLocalNuma > LOCAL_NUMA_NUM ||
        targetCap > MAX_PAIR_TARGET_COUNT || targetCap > SIZE_MAX / sizeof(PairArbitrationEntry)) {
        return -EINVAL;
    }
    *targetCnt = 0;

    PairArbitrationEntry *entries = targetCap == 0 ? NULL : calloc(targetCap, sizeof(PairArbitrationEntry));
    if (targetCap > 0 && !entries) {
        return -ENOMEM;
    }

    int ret;
    size_t entryCount = 0;
    uint64_t privateCapacity[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    uint64_t totalCapacity[REMOTE_NUMA_NUM] = { 0 };

    EnvMutexLock(&manager->lock);
    EnvMutexLock(&manager->remoteNumaInfo.lock);
    ret = BuildPairArbitrationSnapshotLocked(manager, entries, targetCap, &entryCount, privateCapacity, totalCapacity,
                                             false);
    if (!ret) {
        PublishPairCapacityResult(manager, entries, entryCount, privateCapacity, totalCapacity);
        for (size_t i = 0; i < entryCount; i++) {
            targets[i] = entries[i].target;
        }
        *targetCnt = entryCount;
    }
    EnvMutexUnlock(&manager->remoteNumaInfo.lock);
    EnvMutexUnlock(&manager->lock);
    free(entries);
    return ret;
}

/*
 * Build target/actual plan inputs and per-process limits from one locked
 * manager snapshot. File I/O and per-cycle free-page budgeting stay in the
 * strategy layer.
 */
int BuildAllPairPlanInputsForState(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                                   PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt,
                                   bool migrateOnly)
{
    if (!manager || !plans || !planCnt || !pidBudgets || !pidBudgetCnt || manager->nrLocalNuma == 0 ||
        manager->nrLocalNuma > LOCAL_NUMA_NUM || planCap > MAX_PAIR_TARGET_COUNT ||
        planCap > SIZE_MAX / sizeof(PairArbitrationEntry)) {
        return -EINVAL;
    }
    *planCnt = 0;
    *pidBudgetCnt = 0;

    PairArbitrationEntry *entries = planCap == 0 ? NULL : calloc(planCap, sizeof(PairArbitrationEntry));
    if (planCap > 0 && !entries) {
        return -ENOMEM;
    }

    int ret;
    size_t entryCount = 0;
    size_t budgetCount = 0;
    uint64_t privateCapacity[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    uint64_t totalCapacity[REMOTE_NUMA_NUM] = { 0 };

    EnvMutexLock(&manager->lock);
    EnvMutexLock(&manager->remoteNumaInfo.lock);
    ret = BuildPairArbitrationSnapshotLocked(manager, entries, planCap, &entryCount, privateCapacity, totalCapacity,
                                             migrateOnly);
    if (!ret) {
        for (size_t i = 0; i < entryCount; i++) {
            if (i == 0 || entries[i].target.pid != entries[i - 1].target.pid) {
                budgetCount++;
            }
        }
        if (budgetCount > pidBudgetCap) {
            ret = -ENOSPC;
        }
    }
    if (!ret) {
        PublishPairCapacityResult(manager, entries, entryCount, privateCapacity, totalCapacity);
        size_t budgetIndex = 0;
        for (size_t i = 0; i < entryCount; i++) {
            int remoteIndex = entries[i].target.remoteNid - manager->nrLocalNuma;
            plans[i] = (PairPlan){
                .pid = entries[i].target.pid,
                .localNid = entries[i].target.localNid,
                .remoteNid = entries[i].target.remoteNid,
                .remoteIndex = remoteIndex,
                .targetPages = entries[i].target.targetPages,
                .actualPages = entries[i].actualPages,
            };
            if (i == 0 || entries[i].target.pid != entries[i - 1].target.pid) {
                pidBudgets[budgetIndex++] = (PairPidBudget){
                    .pid = entries[i].target.pid,
                    /*
                     * Keep the current separate-strategy limit semantics:
                     * CalcMaxMigrate(1, walkPage.nrPage).
                     */
                    .maxMigratePages = entries[i].process->walkPage.nrPage,
                };
            }
        }
        *planCnt = entryCount;
        *pidBudgetCnt = budgetCount;
    }
    EnvMutexUnlock(&manager->remoteNumaInfo.lock);
    EnvMutexUnlock(&manager->lock);
    free(entries);
    return ret;
}

int BuildAllPairPlanInputs(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                           PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt)
{
    return BuildAllPairPlanInputsForState(manager, plans, planCap, planCnt, pidBudgets, pidBudgetCap, pidBudgetCnt,
                                          false);
}

/**
 * DistributeActcData - 将读取的数据分配到各node的actcData
 * @attr: ProcessAttr结构体指针
 * @pmb: ProcessMemBitmap结构体指针
 * @buf: 读取的数据缓冲区
 *
 * 返回: 成功返回0，失败返回负错误码
 */
static int DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf)
{
    size_t actc_offset = 0;

    for (int nid = 0; nid < MAX_NODES; nid++) {
        if (attr->scanAttr.actcData[nid]) {
            free(attr->scanAttr.actcData[nid]);
            attr->scanAttr.actcData[nid] = NULL;
        }
        attr->scanAttr.actcLen[nid] = pmb->nrPages[nid];
        if (pmb->nrPages[nid] == 0) {
            attr->scanAttr.actcData[nid] = NULL;
            continue;
        }
        attr->scanAttr.actcData[nid] = malloc(pmb->nrPages[nid] * sizeof(ActcData));
        if (!attr->scanAttr.actcData[nid]) {
            SMAP_LOGGER_ERROR("malloc actcData[%d] failed for pid %d", nid, attr->pid);
            for (int i = 0; i < nid; i++) {
                free(attr->scanAttr.actcData[i]);
                attr->scanAttr.actcData[i] = NULL;
            }
            return -ENOMEM;
        }
        size_t actcDataSize = pmb->nrPages[nid] * sizeof(ActcData);
        int ret = memcpy_s(attr->scanAttr.actcData[nid], actcDataSize, buf + actc_offset, actcDataSize);
        if (ret != EOK) {
            SMAP_LOGGER_ERROR("copy actcData[%d] failed for pid %d, ret %d", nid, attr->pid, ret);
            for (int i = 0; i <= nid; i++) {
                free(attr->scanAttr.actcData[i]);
                attr->scanAttr.actcData[i] = NULL;
            }
            return -ret;
        }
        actc_offset += pmb->nrPages[nid];
    }
    return 0;
}

/**
 * ReadPidActcData - 从内核态read完整的actc_data数组
 * @attr: ProcessAttr结构体指针
 * @pmb: ProcessMemBitmap结构体指针（包含nrPages信息）
 *
 * read连续的actc_data数组，按nrPages分段分配到actcData[nid]。
 */
static int ReadPidActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb)
{
    char path[FREQ_FILE_PATH_LEN];
    int fd, ret;
    size_t total_actc = 0;
    size_t shm_size;
    ActcData *buf;
    ssize_t read_len;

    snprintf(path, sizeof(path), "/proc/smap/%d/mem_freq", attr->pid);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("open mem_freq file failed for pid %d: %d", attr->pid, errno);
        return -ENODEV;
    }

    for (int nid = 0; nid < MAX_NODES; nid++) {
        total_actc += pmb->nrPages[nid];
    }

    if (total_actc == 0) {
        SMAP_LOGGER_INFO("pid %d has no pages, skip read", attr->pid);
        close(fd);
        return 0;
    }

    shm_size = total_actc * sizeof(ActcData);
    buf = malloc(shm_size);
    if (!buf) {
        SMAP_LOGGER_ERROR("malloc failed for pid %d, size %zu", attr->pid, shm_size);
        close(fd);
        return -ENOMEM;
    }

    read_len = read(fd, buf, shm_size);
    close(fd);

    if (read_len < 0 || read_len != shm_size) {
        SMAP_LOGGER_ERROR("read failed for pid %d, expected %zu, got %zd", attr->pid, shm_size, read_len);
        free(buf);
        return -EIO;
    }

    ret = DistributeActcData(attr, pmb, buf);
    free(buf);

    if (ret) {
        return ret;
    }

    SMAP_LOGGER_INFO("read pid %d success, total_actc %zu", attr->pid, total_actc);
    return 0;
}

static int FillPidData(ProcessAttr *attr, struct ProcessMemBitmap *pmb)
{
    int ret;

    ret = ReadPidActcData(attr, pmb);
    if (ret) {
        SMAP_LOGGER_ERROR("Read pid %d actc data failed: %d", attr->pid, ret);
        return ret;
    }

    CalcActcStats(attr);

    return 0;
}

static int BuildBitmapBuf(size_t *len, char **buf)
{
    char *tmpBuf;
    size_t tmpLen;
    int ret = AccessIoctlWalkPagemap(&tmpLen);
    if (ret) {
        SMAP_LOGGER_ERROR("access ioctl walk pagemap error: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("AccessIoctlWalkPagemap bufLen %zu.", tmpLen);
    if (tmpLen == 0) {
        SMAP_LOGGER_ERROR("Access ioctl walk pagemap len invalid: %zu.", tmpLen);
        return -EINVAL;
    }

    tmpBuf = malloc(tmpLen);
    if (!tmpBuf) {
        tmpLen = 0;
        return -ENOMEM;
    }

    *len = tmpLen;
    *buf = tmpBuf;

    return 0;
}

static int ParseBitmapPid(struct ProcessMemBitmap *pmb, char *buf, size_t *offset)
{
    int ret;
    size_t pidSize = sizeof(pmb->pid);

    ret = memcpy_s(&pmb->pid, pidSize, buf, pidSize);
    if (ret) {
        return -ret;
    }
    *offset += pidSize;
    return 0;
}

static int ParseBitmapNrPages(struct ProcessMemBitmap *pmb, char *buf, size_t *offset)
{
    int ret;
    size_t pageNumSize = sizeof(pmb->nrPages[0]);
    size_t tmpOffset = 0;

    for (int nid = 0; nid < MAX_NODES; nid++) {
        ret = memcpy_s(&pmb->nrPages[nid], pageNumSize, buf + tmpOffset, pageNumSize);
        if (ret) {
            return -ret;
        }
        tmpOffset += pageNumSize;
    }
    *offset += tmpOffset;
    return 0;
}

static int ParseBitmap(size_t bufLen, char *buf, size_t *offset, struct ProcessMemBitmap *pmb)
{
    size_t newOffset = *offset;

    int ret = ParseBitmapPid(pmb, buf + newOffset, &newOffset);
    if (ret) {
        SMAP_LOGGER_ERROR("ParseBitmapPid err: %d.", ret);
        return ret;
    }

    ret = ParseBitmapNrPages(pmb, buf + newOffset, &newOffset);
    if (ret) {
        SMAP_LOGGER_ERROR("ParseBitmapNrPages err: %d.", ret);
        return ret;
    }

    SMAP_LOGGER_INFO("read continue %zu %zu.", newOffset, bufLen);

    *offset = newOffset;
    return 0;
}

static void NoAccountAlloc(int remoteNid, ProcessAttr *attr)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    int l1Nid[nrLocalNuma];
    int l1Len = 0;

    for (i = 0; i < nrLocalNuma; i++) {
        if (InAttrL1(attr, i)) {
            l1Nid[l1Len++] = i;
        }
    }
    if (l1Len == 0) {
        SMAP_LOGGER_WARNING("Aborted alloc account rebuild for pid %d due to missing L1 node", attr->pid);
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrLeft = attr->walkPage.nrPages[remoteNid];
    uint32_t nrChunk = nrLeft / l1Len;

    SMAP_LOGGER_INFO("Rebuilding alloc account for pid %d", attr->pid);
    for (i = 0; i < l1Len; i++) {
        int l1Index = l1Nid[i];
        int l2Index = remoteNid - nrLocalNuma;

        if (i == l1Len - 1) {
            sa->allocRemoteNrPages[l1Index][l2Index] = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        } else {
            sa->allocRemoteNrPages[l1Index][l2Index] = nrChunk;
            nrLeft -= nrChunk;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        }
    }
    SMAP_LOGGER_INFO("Rebuild alloc account complete for pid %d", attr->pid);
}

static void ClearNormalPidAccount(ProcessAttr *attr, int remoteNode, int nrLocalNuma)
{
    if (attr->state != PROC_MOVE) {
        for (int i = 0; i < nrLocalNuma; i++) {
            attr->strategyAttr.remoteNrPagesAfterMigrate[i][remoteNode] = 0;
        }
    }
}

// returnFlag为true表示该NUMA处理完成，无需后续处理
static void CheckAccountAndNrPage(ProcessAttr *attr, bool returnFlag[REMOTE_NUMA_NUM])
{
    int i, j;
    int nrLocalNuma = GetNrLocalNuma();
    /**
     * 每个远端numa的账本，和nrPage进行对比
     * 1、远端numa有账本，但是nrPage没有，对账本进行清零
     * 2、nrPage有但是远端numa没有账本，
     *   1）检查L1的分布情况，平均分
     *   2）L1没有分布情况，按照CPU绑定情况，平均分
     */
    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        int remoteNid = nrLocalNuma + j;
        uint32_t tmpTotal = 0;
        double ratio;
        if (NotInAttrL2(attr, remoteNid)) {
            continue;
        }
        if (attr->walkPage.nrPages[remoteNid] == 0) {
            ClearNormalPidAccount(attr, j, nrLocalNuma);
            continue;
        }
        for (i = 0; i < nrLocalNuma; i++) {
            tmpTotal += attr->strategyAttr.remoteNrPagesAfterMigrate[i][j];
        }
        // nrPage有但是远端numa没有账本
        if (tmpTotal == 0) {
            NoAccountAlloc(remoteNid, attr);
            continue;
        }
        returnFlag[j] = false;
    }
}

static void CalNrPagesPerLocalNuma(ProcessAttr *attr)
{
    StrategyAttribute *sa = &attr->strategyAttr;

    for (int i = 0; i < GetNrLocalNuma(); i++) {
        uint64_t whiteNum = attr->scanAttr.actCount[i].whiteNum;
        /* 饱和减法：扫描失败或总页数为0时旧 whiteNum 可能残留，防止下溢成数十亿可迁页 */
        uint32_t nrLocal = attr->walkPage.nrPages[i] > whiteNum ? (uint32_t)(attr->walkPage.nrPages[i] - whiteNum) : 0;
        uint32_t nrRemote = 0;

        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            nrRemote += sa->allocRemoteNrPages[i][j];
        }
        sa->nrPagesPerLocalNuma[i] = nrLocal + nrRemote;

        SMAP_LOGGER_DEBUG(
            "[cal_local_total] pid=%d, local_node=%d total_pages=%u local_pages=%u white=%llu remote_pages=%u",
            attr->pid, i, sa->nrPagesPerLocalNuma[i], nrLocal, whiteNum, nrRemote);
    }
}

/**
 * CalRemoteAllocRatio - 根据历史贡献度计算各本地NUMA迁往某远端NUMA的比例
 *
 * @param attr:    进程结构体指针
 * @param l2Index: L2索引
 * @param ratio:   比例数组
 * @param len:     比例数组长度
 */
static void CalRemoteAllocRatio(ProcessAttr *attr, int l2Index, double *ratio, int *len)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    int l2Nid = l2Index + nrLocalNuma;
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t acctTotal = 0;

    for (i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        acctTotal += sa->remoteNrPagesAfterMigrate[i][l2Index];
    }

    if (acctTotal == 0) {
        *len = 0;
    } else {
        for (i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
            ratio[i] = (double)sa->remoteNrPagesAfterMigrate[i][l2Index] / acctTotal;
            SMAP_LOGGER_DEBUG("[alloc_remote_ratio] pid=%d local=%d remote=%d pages=%u/%u remote_ratio=%.2lf",
                              attr->pid, i, l2Nid, sa->remoteNrPagesAfterMigrate[i][l2Index], acctTotal, ratio[i]);
        }
        *len = i;
    }
}

/**
 * CalRemoteAllocPages - 根据比例计算各本地NUMA的迁移页数
 *
 * @param attr:    进程结构体指针
 * @param l2Index: L2索引
 * @param ratio:   比例数组
 * @param len:     比例数组长度
 */
static void CalRemoteAllocPages(ProcessAttr *attr, int l2Index, double *ratio, int ratioLen)
{
    int l2Nid = l2Index + GetNrLocalNuma();
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTotal = attr->walkPage.nrPages[l2Nid];
    uint32_t nrLeft = nrTotal;

    for (int i = 0; i < ratioLen; i++) {
        if (ratio[i] == 0) {
            continue;
        }

        if (i == ratioLen - 1) {
            sa->allocRemoteNrPages[i][l2Index] = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrLeft);
        } else {
            uint32_t nrTmp = nrTotal * ratio[i];
            sa->allocRemoteNrPages[i][l2Index] = nrTmp;
            nrLeft -= nrTmp;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTmp);
        }
    }
}

static void CalRemotePerLocalWithAccount(int l2Index, ProcessAttr *attr)
{
    double ratioArr[LOCAL_NUMA_NUM];
    int ratioLen = 0;

    // 1. 根据 remoteNrPagesAfterMigrate 账本计算分配比例
    CalRemoteAllocRatio(attr, l2Index, ratioArr, &ratioLen);
    if (ratioLen == 0) {
        return;
    }
    // 2. 根据分配比例计算分配的页面数量
    CalRemoteAllocPages(attr, l2Index, ratioArr, ratioLen);
}

static void CalRemotePerLocal(ProcessAttr *attr)
{
    int i, j;
    bool returnFlag[REMOTE_NUMA_NUM];

    for (i = 0; i < REMOTE_NUMA_NUM; i++) {
        returnFlag[i] = true;
    }

    // 检查账本和当前内存页分布情况，处理有远端内存，但是没有账本的情况
    CheckAccountAndNrPage(attr, returnFlag);
    int nrLocalNuma = GetNrLocalNuma();

    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        // 1、Pid本地单一numa占比：pid->本地numa/sum(所有本地numa总迁出量)
        if (NotInAttrL2(attr, nrLocalNuma + j)) {
            continue;
        }
        if (returnFlag[j]) {
            continue;
        }
        // 有账本，并且nrPage[remoteNid] != 0，计算allocRemoteNrPages
        CalRemotePerLocalWithAccount(j, attr);
    }
}

static void CalNrPagesLocalTotalPerPid(ProcessAttr *attr)
{
    // 计算每个本地numa，对应可迁出到远端每个numa的内存量
    CalRemotePerLocal(attr);

    // pid本地numa总使用量：Pid本地numa数量+Pid远端使用量
    CalNrPagesPerLocalNuma(attr);
}

static void CalNrPagesLocalTotal(void)
{
    ProcessAttr *attr = g_processManager.processes;
    int ret;

    while (attr) {
        if (IsMultiNumaVm(attr) && GetRunMode() == MEM_POOL_MODE) {
            attr = attr->next;
            continue;
        }
        SMAP_LOGGER_DEBUG("CalNrPagesLocalTotal pid: %d.", attr->pid);
        CalNrPagesLocalTotalPerPid(attr);
        attr = attr->next;
    }
}

// 计算远端内存分配tmpNrPagesToUse下，不同pid应该迁出多少内存，结果叠加在l2RemoteMemRatio中
static void CalRemoteNumaAllocPerPid(int i, int j, uint32_t tmpNrPagesToUse,
                                     uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM])
{
    ProcessAttr *attr = g_processManager.processes;

    PidType type = GetPidType(&g_processManager);
    if (g_processManager.nr[type] == 0) {
        return;
    }
    double tmpRatioPerPid;

    // 再按比例分配tmpNrPagesToUse
    if (tmpMaxAllocNrPages[i][j] == 0) {
        return;
    }
    // 根据比例计算每个PID的迁出比例，更新迁出的比例到l2RemoteMemRatio
    while (attr) {
        // 1）每个PID最大迁出量/总最大迁出量 = 最大迁出量比例
        tmpRatioPerPid =
            (double)attr->strategyAttr.nrPagesPerLocalNuma[i] *
            ((attr->strategyAttr.initRemoteMemRatio[i][j] - attr->strategyAttr.l2RemoteMemRatio[i][j]) / HUNDRED) /
            tmpMaxAllocNrPages[i][j];
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 1: %u [%d][%d]: %.2lf %u.", tmpNrPagesToUse, i, j, tmpRatioPerPid,
                          attr->strategyAttr.nrPagesPerLocalNuma[i]);

        // 2）最大迁出量比例 * numa可用量 = 每个PID可用的量（即numa迁出的ratio ）
        attr->strategyAttr.l2RemoteMemRatio[i][j] +=
            ((double)tmpNrPagesToUse * tmpRatioPerPid / attr->strategyAttr.nrPagesPerLocalNuma[i]) * HUNDRED;
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 2: %.2lf.", attr->strategyAttr.l2RemoteMemRatio[i][j]);

        attr = attr->next;
    }
}

static void CalAvailBorrowPage(uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                               uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    struct RemoteNumaInfo *rmi = &g_processManager.remoteNumaInfo;

    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        if (rmi->sharedSize[j] > 0) {
            availSharedPages[j] = MBToPage(rmi->sharedSize[j]);
            SMAP_LOGGER_DEBUG("availSharedPages[%d] %llu", j, availSharedPages[j]);
        }
        for (int i = 0; i < GetNrLocalNuma() && i < LOCAL_NUMA_NUM; i++) {
            if (rmi->privateSize[i][j] > 0) {
                availPrivatePages[i][j] = MBToPage(rmi->privateSize[i][j]);
                SMAP_LOGGER_DEBUG("availPrivatePages[%d][%d] %llu", i, j, availPrivatePages[i][j]);
            }
        }
    }
}

static void AllocPrivatePage(uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                             uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM])
{
    for (int i = 0; i < GetNrLocalNuma(); i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            if (tmpMaxAllocNrPages[i][j] == 0) {
                continue;
            }
            SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u.", i, j, tmpMaxAllocNrPages[i][j]);
            SMAP_LOGGER_DEBUG("availPrivatePages 2 %llu.", availPrivatePages[i][j]);
            if (availPrivatePages[i][j] == 0) {
                continue;
            }

            uint32_t tmpNrPagesToUse;
            // If 每个numa最大迁出量 > 专属numa：
            if (tmpMaxAllocNrPages[i][j] > availPrivatePages[i][j]) {
                tmpNrPagesToUse = availPrivatePages[i][j];
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                tmpMaxAllocNrPages[i][j] -= availPrivatePages[i][j];
            } else {
                // If 专属numa  > 每个numa最大迁出量：直接迁（迁出的ratio + remote_numa ID）
                tmpNrPagesToUse = tmpMaxAllocNrPages[i][j];
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                tmpMaxAllocNrPages[i][j] = 0;
            }
        }
    }
}

static void AllocBorrowPage(uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                            uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                            uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    int i, j;
    // 优先使用专属的远端内存
    AllocPrivatePage(tmpMaxAllocNrPages, availPrivatePages);
    double tmpRatioPerLocalNuma[LOCAL_NUMA_NUM];
    // 再使用共享远端内存
    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        uint32_t tmpNrPagesCanMigOut = 0;
        if (availSharedPages[j] == 0) {
            continue;
        }
        for (i = 0; i < GetNrLocalNuma(); i++) {
            tmpRatioPerLocalNuma[i] = 0;
            tmpNrPagesCanMigOut += tmpMaxAllocNrPages[i][j];
        }
        if (tmpNrPagesCanMigOut == 0) {
            continue;
        }
        for (i = 0; i < GetNrLocalNuma(); i++) {
            tmpRatioPerLocalNuma[i] = (double)tmpMaxAllocNrPages[i][j] / tmpNrPagesCanMigOut;
            // 将共享远端内存，分给每个本地numa去迁出，按照各本地numa可迁出的比例分配
            uint32_t canUsePage = tmpRatioPerLocalNuma[i] * availSharedPages[j];
            SMAP_LOGGER_INFO("tmpRatioPerLocalNuma[%d] %.2lf, tmpNrPagesCanMigOut: %u, SharedBorrow[%d]: %u.", i,
                             tmpRatioPerLocalNuma[i], tmpNrPagesCanMigOut, j, availSharedPages[j]);
            if (canUsePage > tmpMaxAllocNrPages[i][j]) {
                CalRemoteNumaAllocPerPid(i, j, tmpMaxAllocNrPages[i][j], tmpMaxAllocNrPages);
            } else {
                CalRemoteNumaAllocPerPid(i, j, canUsePage, tmpMaxAllocNrPages);
            }
        }
    }
}

static void AllocBorrowPagesForMemsize(ProcessAttr *attr, uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                       uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    int l2Nid = attr->migrateParam[0].nid;
    int l2Index = l2Nid - GetNrLocalNuma();
    if (l2Index < 0 || l2Index >= REMOTE_NUMA_NUM) {
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTarget = KBToPage(attr->migrateParam[0].memSize);

    for (int i = 0; i < GetNrLocalNuma() && i < LOCAL_NUMA_NUM; i++) {
        if (!InAttrL1(attr, i)) {
            sa->memSize[i][l2Index] = 0;
            SMAP_LOGGER_INFO("[memsize_clear] pid=%d local=%d remote=%d pages=0", attr->pid, i, l2Nid);
            continue;
        }

        // 先使用私有借用内存
        uint32_t nrTotal = sa->nrPagesPerLocalNuma[i];
        uint32_t nrAvail = MIN(nrTotal, availPrivatePages[i][l2Index]);
        SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         l2Nid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            nrTotal -= nrAvail;
            availPrivatePages[i][l2Index] -= nrAvail;
            sa->memSize[i][l2Index] = PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrAvail);
        } else {
            availPrivatePages[i][l2Index] -= nrTarget;
            sa->memSize[i][l2Index] = PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_private*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTarget);
            break;
        }

        if (nrTotal == 0) {
            continue;
        }

        // 再使用共享借用内存
        nrAvail = MIN(nrTotal, availSharedPages[l2Index]);
        SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         l2Nid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            availSharedPages[l2Index] -= nrAvail;
            sa->memSize[i][l2Index] += PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrAvail);
        } else {
            availSharedPages[l2Index] -= nrTarget;
            sa->memSize[i][l2Index] += PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_shared*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTarget);
            break;
        }
    }
}

static void CalRemoteNumaSizeAllocPerNuma(void)
{
    ProcessAttr *attr;
    struct RemoteNumaInfo remoteNumaInfo = g_processManager.remoteNumaInfo;
    uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    int i, j;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = { 0 };
    CalAvailBorrowPage(availPrivatePages, availSharedPages);

    // 先满足迁移模式为 MIG_MEMSIZE_MODE 的进程
    for (attr = g_processManager.processes; attr; attr = attr->next) {
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            AllocBorrowPagesForMemsize(attr, availPrivatePages, availSharedPages);
        }
    }

    // 计算所有进程**想要**从各本地NUMA迁移到各远端NUMA的总页面数量
    for (attr = g_processManager.processes; attr; attr = attr->next) {
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            continue;
        }

        StrategyAttribute *sa = &attr->strategyAttr;
        for (i = 0; i < GetNrLocalNuma(); i++) {
            for (j = 0; j < REMOTE_NUMA_NUM; j++) {
                tmpMaxAllocNrPages[i][j] += sa->nrPagesPerLocalNuma[i] * sa->initRemoteMemRatio[i][j] / HUNDRED;
                SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u, initRemoteMemRatio=%.2lf.", i, j,
                                  tmpMaxAllocNrPages[i][j], sa->initRemoteMemRatio[i][j]);

                sa->l2RemoteMemRatio[i][j] = 0;
            }
        }
    }

    // 用远端借用的内存计算每个pid，每个numa可迁出的比例
    AllocBorrowPage(tmpMaxAllocNrPages, availPrivatePages, availSharedPages);
}

static void CalcMigrateNrPagesPerPIDMuiltNuma(void)
{
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;
    PidType type = GetPidType(&g_processManager);
    if (g_processManager.nr[type] == 0) {
        return;
    }
    // 根据账本信息，计算每个PID各本地numa可支配的内存；
    CalNrPagesLocalTotal();

    if (g_runMode == MEM_POOL_MODE) {
        return;
    }
    EnvMutexLock(&numaInfo->lock);
    ClearRemoteMemUsed();
    CalRemoteMemUsed();
    // 按照远端numa的粒度，和上一步计算的迁移量，计算每个本地numa相对于1个远端numa的比例
    CalRemoteNumaSizeAllocPerNuma();
    EnvMutexUnlock(&numaInfo->lock);
}

static int BuildAndFillBitmapBuf(size_t *len, char **buf)
{
    int ret;
    ret = BuildBitmapBuf(len, buf);
    if (ret) {
        SMAP_LOGGER_ERROR("Access ioctl walk pagemap error: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("Build bitmap buffer done.");
    ret = AccessRead(*len, *buf);
    if (ret) {
        SMAP_LOGGER_ERROR("Access read pagemap error: %d.", ret);
        free(*buf);
        return ret;
    }
    return 0;
}

static int RefreshManagedLocalTrackingScope(ProcessAttr *attr)
{
    ProcessAttr candidate = *attr;
    int ret = RefreshManagedLocalState(&candidate, false);
    if (ret) {
        return ret;
    }

    candidate.numaAttr.numaNodes = BuildManagedTrackingNodes(&candidate);
    if (candidate.numaAttr.numaNodes != attr->numaAttr.numaNodes) {
        struct AccessAddPidPayload payload = {
            .type = attr->scanType,
            .pid = attr->pid,
            .scanTime = attr->scanTime,
            .duration = attr->duration,
            .numaNodes = candidate.numaAttr.numaNodes,
        };
        ret = AccessIoctlAddPid(1, &payload);
        if (ret) {
            SMAP_LOGGER_ERROR("Refresh pid %d managed tracking failed: %d.", attr->pid, ret);
            return ret;
        }
    }

    attr->managedLocalState = candidate.managedLocalState;
    attr->numaAttr.numaNodes = candidate.numaAttr.numaNodes;
    return 0;
}

int BuildAllPidData(void)
{
    int ret, failedCount = 0;
    char *buf;
    size_t bufLen;
    EnvMutexLock(&g_processManager.lock);
    ret = BuildAndFillBitmapBuf(&bufLen, &buf);
    if (ret) {
        SMAP_LOGGER_ERROR("BuildAllPidData: build and fill BitmapBuf error: %d.", ret);
        EnvMutexUnlock(&g_processManager.lock);
        return ret;
    }
    for (size_t offset = 0; offset < bufLen;) {
        struct ProcessMemBitmap pmb = { 0 };
        SMAP_LOGGER_INFO("Parse bitmap from %zu.", offset);
        ret = ParseBitmap(bufLen, buf, &offset, &pmb);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("parse bitmap failed.");
            failedCount++;
            break;
        }
        ProcessAttr *current = GetProcessAttrLocked(pmb.pid);
        if (current && current->scanType == NORMAL_SCAN) {
            SMAP_LOGGER_INFO("Pid %d, numaNodes %#x, nrLocalNuma %u.", current->pid, current->numaAttr.numaNodes,
                             g_processManager.nrLocalNuma);
            SetPidNrPages(current, pmb.nrPages, MAX_NODES);
            ret = FillPidData(current, &pmb);
            if (ret) {
                SMAP_LOGGER_ERROR("Fill pid %d actc data failed.", current->pid);
                failedCount++;
                continue;
            }
            if (!current->groupPolicy.enabled) {
                ret = RefreshManagedLocalTrackingScope(current);
                if (ret) {
                    SMAP_LOGGER_ERROR("Refresh pid %d managed local state failed: %d.", current->pid, ret);
                    failedCount++;
                }
                CalibratePairAccount(current);
            }
        }
    }
    CalcMigrateNrPagesPerPIDMuiltNuma();
    free(buf);
    EnvMutexUnlock(&g_processManager.lock);
    return failedCount;
}

struct ProcessManager *GetProcessManager(void)
{
    return &g_processManager;
}

int SetRemoteNumaInfo(int srcNid, int destNid, uint64_t size)
{
    int ret;
    int column = destNid - g_processManager.nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;
    EnvMutexLock(&numaInfo->lock);
    ret = SyncOneNumaConfig(srcNid, destNid, size);
    if (ret) {
        SMAP_LOGGER_ERROR("SyncOneNumaConfig %d-%d to %llu failed: %d.", srcNid, destNid, size, ret);
        EnvMutexUnlock(&numaInfo->lock);
        return -EBADF;
    }
    SMAP_LOGGER_INFO("SetRemoteNumaInfo %d-%d to %llu.", srcNid, destNid, size);
    if (srcNid == NUMA_NO_NODE) {
        numaInfo->sharedSize[column] = size;
    } else {
        numaInfo->privateSize[srcNid][column] = size;
    }
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        int l2Nid = j + g_processManager.nrLocalNuma;
        numaInfo->usedInfo[j].ifUsedFreshed = false;
        numaInfo->usedInfo[j].size = MBToPage(numaInfo->sharedSize[j]);
        if (numaInfo->usedInfo[j].size) {
            SMAP_LOGGER_INFO("Node%d shared pages: %llu.", l2Nid, numaInfo->usedInfo[j].size);
        }
        for (int i = 0; i < g_processManager.nrLocalNuma; i++) {
            numaInfo->usedInfo[j].size += MBToPage(numaInfo->privateSize[i][j]);
            numaInfo->privateUsedInfo[i][j].ifUsedFreshed = false;
            numaInfo->privateUsedInfo[i][j].size = MBToPage(numaInfo->privateSize[i][j]);
            if (numaInfo->privateUsedInfo[i][j].size) {
                SMAP_LOGGER_INFO("local %d borrow remote %d private pages: %llu.", i, l2Nid,
                                 numaInfo->privateUsedInfo[i][j].size);
            }
        }
        if (numaInfo->usedInfo[j].size) {
            SMAP_LOGGER_INFO("Node%d total borrow pages: %llu.", l2Nid, numaInfo->usedInfo[j].size);
        }
    }
    EnvMutexUnlock(&numaInfo->lock);
    return 0;
}

static bool CheckPrivateBorrowUsed(int destNid)
{
    int nrLocalNuma = GetNrLocalNuma();
    int column = destNid - nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;

    for (int count = 0; count < MAX_FRESH_USED_TIME; count++) {
        EnvMutexLock(&numaInfo->lock);
        for (int i = 0; i < nrLocalNuma; i++) {
            struct RemoteNumaUsedInfo *usedInfo = &numaInfo->privateUsedInfo[i][column];
            SMAP_LOGGER_INFO("[private_borrow] local=%d remote=%d used_pages=%llu total_pages=%llu fresh=%d", i,
                             destNid, usedInfo->used, usedInfo->size, usedInfo->ifUsedFreshed);

            if (!usedInfo->ifUsedFreshed) {
                EnvMutexUnlock(&numaInfo->lock);
                EnvMsleep(WAIT_FRESH_USED_PERIOD);
                break;
            }

            if (usedInfo->used > usedInfo->size) {
                EnvMutexUnlock(&numaInfo->lock);
                return false;
            }

            // 走到这里表明所有本地NUMA的远端内存用量都少于总量
            if (i == nrLocalNuma - 1) {
                EnvMutexUnlock(&numaInfo->lock);
                return true;
            }
        }
    }
    return false;
}

static bool CheckBorrowUsed(int destNid)
{
    int column = destNid - g_processManager.nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;

    for (int count = 0; count < MAX_FRESH_USED_TIME; count++) {
        EnvMutexLock(&numaInfo->lock);
        struct RemoteNumaUsedInfo *usedInfo = &numaInfo->usedInfo[column];
        SMAP_LOGGER_INFO("[total_borrow] remote=%d used_pages=%llu total_pages=%llu freshed=%d", destNid,
                         usedInfo->used, usedInfo->size, usedInfo->ifUsedFreshed);

        if (!usedInfo->ifUsedFreshed) {
            EnvMutexUnlock(&numaInfo->lock);
            EnvMsleep(WAIT_FRESH_USED_PERIOD);
            continue;
        }
        if (usedInfo->used > usedInfo->size) {
            EnvMutexUnlock(&numaInfo->lock);
            return false;
        }
        EnvMutexUnlock(&numaInfo->lock);
        return true;
    }
    return false;
}

bool CheckReadyMigrateBack(int destNid)
{
    // 如果已经没有管理中的虚机，则默认可以执行迁回
    EnvMutexLock(&g_processManager.lock);
    if (!g_processManager.processes) {
        SMAP_LOGGER_INFO("CheckReadyMigrateBack no process, destNid %d.", destNid);
        EnvMutexUnlock(&g_processManager.lock);
        return true;
    }
    EnvMutexUnlock(&g_processManager.lock);
    struct RemoteNumaInfo *numaInfo = &g_processManager.remoteNumaInfo;
    int column = destNid - g_processManager.nrLocalNuma;

    int nrWait = 0;
    while (nrWait < MAX_MIGRATE_BACK_WAIT_TIME) {
        SMAP_LOGGER_INFO("Wait until ready to migrate back, destNid: %d, nrWait: %d.", destNid, nrWait);
        EnvMutexLock(&numaInfo->lock);
        bool flag = numaInfo->sharedSize[column] > 0;
        EnvMutexUnlock(&numaInfo->lock);
        if (flag) {
            if (CheckBorrowUsed(destNid)) {
                return true;
            }
        } else {
            if (CheckPrivateBorrowUsed(destNid)) {
                return true;
            }
        }
        EnvMsleep(MIGRATE_BACK_CHECK_PERIOD);
        nrWait++;
    }
    SMAP_LOGGER_WARNING("destNid %d not ready to migrate back after %d times.", destNid, MAX_MIGRATE_BACK_WAIT_TIME);
    return false;
}

/*
 * 检查pidArr是否都符合状态切换的要求，会跳过未纳管的pid，不会返回错误
 *
 * 返回值：0-否，1-是，其它-异常
 */
int IsPidArrayStateChangeReady(pid_t *pidArr, int len, int enable)
{
    if (!pidArr) {
        SMAP_LOGGER_ERROR("IsPidArrReadyForChangeStat pidArr is null.");
        return -EINVAL;
    }
    for (int i = 0; i < len; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(pidArr[i]);
        if (!attr) {
            SMAP_LOGGER_INFO("pid %d is not in smap list.", pidArr[i]);
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d actual state %d.", pidArr[i], attr->state);
        if (enable == DISABLE_PROCESS_MIGRATE && (attr->state != PROC_IDLE && attr->state != PROC_MOVE)) {
            return 0;
        }
        if (enable == ENABLE_PROCESS_MIGRATE && attr->state == PROC_BACK) {
            return 0;
        }
    }
    return 1;
}

/*
 * 检查pidArr是否都处于state，会跳过未纳管的pid，不会返回错误
 *
 * 返回值：0-否，1-是，其它-异常
 */
int IsPidArrInState(pid_t *pidArr, int len, enum ProcessState state)
{
    if (!pidArr) {
        SMAP_LOGGER_ERROR("IsPidArrInState pidArr is null.");
        return -EINVAL;
    }
    for (int i = 0; i < len; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(pidArr[i]);
        if (!attr) {
            SMAP_LOGGER_INFO("pid %d is not in smap list.", pidArr[i]);
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d actual state %d, expected state %d.", pidArr[i], attr->state, state);
        if (attr->state != state) {
            return 0;
        }
    }
    return 1;
}

static void SetPidArrState(pid_t *pidArr, int len, enum ProcessState state, int enable)
{
    for (int i = 0; i < len; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(pidArr[i]);
        if (!attr) {
            continue;
        }
        /* enable == 1时，迁移状态的pid也视为合理状态，不需要设置为空闲态 */
        if (enable == ENABLE_PROCESS_MIGRATE && attr->state == PROC_MIGRATE) {
            SMAP_LOGGER_DEBUG("pid %d is in PROC_MIGRATE state.", attr->pid);
            continue;
        }
        attr->state = state;
    }
}

/*
 * 检查使用指定l2Node的所有pid是否都处于state态
 *
 * 返回值：false-否，true-是
 */
bool IsAllL2NodePidInState(enum ProcessState state, int l2Node)
{
    EnvMutexLock(&g_processManager.lock);
    for (ProcessAttr *attr = g_processManager.processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, l2Node)) {
            continue;
        }
        if (attr->state != state) {
            EnvMutexUnlock(&g_processManager.lock);
            return false;
        }
    }
    EnvMutexUnlock(&g_processManager.lock);
    return true;
}

static void SetChangePidRemoteMsgPayload(int srcNid, int destNid, int *i, int maxProcessCnt,
                                         struct AccessAddPidPayload *payload)
{
    for (ProcessAttr *attr = g_processManager.processes; attr && *i < maxProcessCnt; attr = attr->next) {
        if (NotEqualToAttrL2(attr, srcNid)) {
            continue;
        }
        SMAP_LOGGER_INFO("ready to change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        payload[*i].pid = attr->pid;
        payload[*i].numaNodes = attr->numaAttr.numaNodes;
        SetL2ByNid(&payload[*i].numaNodes, destNid);
        payload[*i].scanTime = attr->scanTime;
        payload[*i].duration = attr->duration;
        payload[*i].type = attr->scanType;
        (*i)++;
    }
}

static void ChangePidRemoteMemory(ProcessAttr *attr, int srcNodeIndex, int destNodeIndex, uint64_t memSize, int ratio)
{
    int nrLocalNuma = GetNrLocalNuma();
    int l1node;
    if (GetRunMode() == WATERLINE_MODE) {
        l1node = GetAttrL1(attr);
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            ClearNodeBit(&attr->numaAttr.numaNodes, srcNodeIndex + LOCAL_NUMA_BITS);
            attr->migrateParam[0].nid = destNodeIndex + nrLocalNuma;
        } else {
            if (ratio >= attr->strategyAttr.initRemoteMemRatio[l1node][srcNodeIndex]) {
                ClearNodeBit(&attr->numaAttr.numaNodes, srcNodeIndex + LOCAL_NUMA_BITS);
            }
        }
        for (int i = 0; i < g_processManager.nrLocalNuma; i++) {
            attr->strategyAttr.initRemoteMemRatio[i][destNodeIndex] += ratio;
            attr->strategyAttr.initRemoteMemRatio[i][srcNodeIndex] -= ratio;
            attr->strategyAttr.memSize[i][destNodeIndex] = attr->strategyAttr.memSize[i][srcNodeIndex];
            attr->strategyAttr.memSize[i][srcNodeIndex] = 0;

            SMAP_LOGGER_INFO("[change_remote] pid=%d local=%d old_remote=%d new_remote=%d old_sz=%llu new_sz=%llu",
                             attr->pid, i, srcNodeIndex, destNodeIndex, attr->strategyAttr.memSize[i][srcNodeIndex],
                             attr->strategyAttr.memSize[i][destNodeIndex]);
        }
    } else if (GetRunMode() == MEM_POOL_MODE) {
        uint64_t srcMemSize = 0;
        int remoteNidIndex;
        for (int i = 0; i < attr->remoteNumaCnt; i++) {
            int srcNid = srcNodeIndex + nrLocalNuma;
            if (srcNid == attr->migrateParam[i].nid) {
                srcMemSize = attr->migrateParam[i].memSize;
                remoteNidIndex = i;
                break;
            }
        }
        if (memSize >= srcMemSize) {
            ClearNodeBit(&attr->numaAttr.numaNodes, srcNodeIndex + LOCAL_NUMA_BITS);
            attr->migrateParam[remoteNidIndex].nid = 0;
            attr->migrateParam[remoteNidIndex].memSize = 0;
        } else {
            attr->migrateParam[remoteNidIndex].memSize -= memSize;
        }

        for (int i = 0; i < g_processManager.nrLocalNuma; i++) {
            attr->strategyAttr.memSize[i][destNodeIndex] += memSize;
            attr->strategyAttr.memSize[i][srcNodeIndex] -= memSize;
        }
    }

    AddAttrL2(attr, destNodeIndex + nrLocalNuma);

    if (GetRunMode() == WATERLINE_MODE && attr->migrateMode == MIG_MEMSIZE_MODE) {
        return;
    }

    attr->remoteNumaCnt = GetL2Count(attr->numaAttr.numaNodes);
    SMAP_LOGGER_INFO("========= remoteNumaCnt %d", attr->remoteNumaCnt);
    int targetIdx = -1;
    int zeroIdx = -1;

    for (int i = 0; i < attr->remoteNumaCnt; i++) {
        if (attr->migrateParam[i].nid == (destNodeIndex + nrLocalNuma)) {
            targetIdx = i;
            break;
        }
        if (zeroIdx == -1 && attr->migrateParam[i].nid == 0) {
            zeroIdx = i;
        }
    }

    if (targetIdx != -1) {
        attr->migrateParam[targetIdx].memSize += memSize;
    } else if (zeroIdx != -1) {
        attr->migrateParam[zeroIdx].nid = destNodeIndex + nrLocalNuma;
        attr->migrateParam[zeroIdx].memSize = memSize;
    }
}

static void ChangePidRemoteMemoryByNuma(ProcessAttr *attr, int srcNode, int destNode)
{
    if (GetRunMode() == WATERLINE_MODE) {
        for (int i = 0; i < g_processManager.nrLocalNuma; i++) {
            attr->strategyAttr.initRemoteMemRatio[i][destNode] = attr->strategyAttr.initRemoteMemRatio[i][srcNode];
            attr->strategyAttr.initRemoteMemRatio[i][srcNode] = 0;
        }
    } else if (GetRunMode() == MEM_POOL_MODE) {
        for (int i = 0; i < g_processManager.nrLocalNuma; i++) {
            attr->strategyAttr.memSize[i][destNode] = attr->strategyAttr.memSize[i][srcNode];
            attr->strategyAttr.memSize[i][srcNode] = 0;
        }
    }
}

int ChangePidRemoteByNuma(int srcNid, int destNid)
{
    int i = 0;
    int maxProcessCnt = GetCurrentMaxNrPid();
    int srcNode = srcNid - g_processManager.nrLocalNuma;
    int destNode = destNid - g_processManager.nrLocalNuma;
    ProcessAttr *attr;
    struct AccessAddPidPayload *payload = malloc(sizeof(struct AccessAddPidPayload) * maxProcessCnt);
    if (!payload) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma malloc payload failed.");
        return -ENOMEM;
    }

    EnvMutexLock(&g_processManager.lock);
    SetChangePidRemoteMsgPayload(srcNid, destNid, &i, maxProcessCnt, payload);
    if (i == 0) {
        SMAP_LOGGER_INFO("ChangePidRemoteByNuma len: %d, no need to change.", i);
        EnvMutexUnlock(&g_processManager.lock);
        free(payload);
        return 0;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl begin, len: %d.", i);
    int ret = AccessIoctlAddPid(i, payload);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        EnvMutexUnlock(&g_processManager.lock);
        return ret;
    }
    for (attr = g_processManager.processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, srcNid)) {
            continue;
        }
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        for (int j = 0; j < g_processManager.nrLocalNuma; j++) {
            attr->strategyAttr.remoteNrPagesAfterMigrate[j][destNode] +=
                attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode];
            attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode] = 0;
        }
        ChangePidRemoteMemoryByNuma(attr, srcNode, destNode);
        SetAttrL2(attr, destNid);
    }
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid after change remote maybe failed: %d.", ret);
    }
    EnvMutexUnlock(&g_processManager.lock);
    return 0;
}

int EnableProcessMigrate(pid_t *pidArr, int len, int enable)
{
    int retry = WAIT_PROC_STATE_MAX_RETRY;
    enum ProcessState newState;
    newState = enable == ENABLE_PROCESS_MIGRATE ? PROC_IDLE : PROC_MOVE;

    SMAP_LOGGER_DEBUG("enter EnableProcessMigrate.");
    while (true) {
        EnvMutexLock(&g_processManager.lock);
        int ret = IsPidArrayStateChangeReady(pidArr, len, enable);
        if (ret == 1) {
            if (enable == ENABLE_PROCESS_MIGRATE) {
                SMAP_LOGGER_INFO("set pids state to migrate state: %d or %d succeed.", PROC_IDLE, PROC_MIGRATE);
            } else {
                SMAP_LOGGER_INFO("set pids state from %d to %d succeed.", PROC_IDLE, PROC_MOVE);
            }
            SetPidArrState(pidArr, len, newState, enable);
            ret = SyncAllProcessConfig();
            if (ret) {
                SMAP_LOGGER_WARNING("Synchronize pid state maybe failed: %d.", ret);
            }
            EnvMutexUnlock(&g_processManager.lock);
            return 0;
        }
        EnvMutexUnlock(&g_processManager.lock);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("check pid state err: %d.", ret);
            return ret;
        }
        if (--retry < 0) {
            SMAP_LOGGER_INFO("wait for pid state to change timed out, enable: %d.", enable);
            return -ETIMEDOUT;
        }
        SMAP_LOGGER_INFO("wait for pid state to change, %d more times left.", retry);
        EnvMsleep(WAIT_PROC_STATE_PERIOD);
    }
}

/*
 * 检查远端NUMA上的内存是否可被迁回
 *
 * 传入的nid必须是远端NUMA，如果有使用该NUMA的进程是PROC_MOVE状态，则不可执行迁回
 * 返回值：0-否，1-是，其它-异常
 */
int IsRemoteNumaMigrateBackAllowed(int nid)
{
    if (nid < g_processManager.nrLocalNuma) {
        return -EINVAL;
    }
    EnvMutexLock(&g_processManager.lock);
    for (ProcessAttr *attr = g_processManager.processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state == PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d == PROC_MOVE.", attr->pid, attr->state);
            EnvMutexUnlock(&g_processManager.lock);
            return 0;
        }
    }
    EnvMutexUnlock(&g_processManager.lock);
    return 1;
}

/*
 * 检查远端NUMA上的内存是否可被搬移
 *
 * 和IsNumaMigrateBackAllowed相反，如果有使用该NUMA的进程不是PROC_MOVE状态，则不可执行搬移
 * 返回值：0-否，1-是，其它-异常
 */
int IsRemoteNumaMoveAllowed(int nid)
{
    if (nid < g_processManager.nrLocalNuma) {
        return -EINVAL;
    }
    EnvMutexLock(&g_processManager.lock);
    for (ProcessAttr *attr = g_processManager.processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state != PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d != PROC_MOVE.", attr->pid, attr->state);
            EnvMutexUnlock(&g_processManager.lock);
            return 0;
        }
    }
    EnvMutexUnlock(&g_processManager.lock);
    return 1;
}

static bool IsRemoteTargetMigOutDone(ProcessAttr *attr, int remoteNid, uint64_t targetPages)
{
    if (remoteNid < 0 || remoteNid >= MAX_NODES) {
        SMAP_LOGGER_ERROR("Invalid remote node %d of pid %d.", remoteNid, attr->pid);
        return false;
    }

    int remoteIdx = remoteNid - GetNrLocalNuma();
    uint64_t accountedPages = 0;
    if (remoteIdx >= 0 && remoteIdx < REMOTE_NUMA_NUM) {
        for (int local = 0; local < LOCAL_NUMA_NUM; local++) {
            accountedPages += attr->strategyAttr.remoteNrPagesAfterMigrate[local][remoteIdx];
        }
    }
    uint64_t remotePages = attr->walkPage.nrPages[remoteNid];
    SMAP_LOGGER_INFO("Pid: %d, remote node: %d, target pages: %llu, accounted pages: %llu, current remote pages: %llu.",
                     attr->pid, remoteNid, targetPages, accountedPages, remotePages);

    if (targetPages > 0 && accountedPages == targetPages) {
        return true;
    }
    return remotePages == targetPages;
}

static bool GetRemoteTargetPages(ProcessAttr *attr, int remoteNid, uint64_t *targetPages)
{
    for (int i = 0; i < attr->remoteNumaCnt; i++) {
        if (attr->migrateParam[i].nid != remoteNid) {
            continue;
        }
        *targetPages = KBToHugePage(attr->migrateParam[i].memSize);
        return true;
    }

    return false;
}

bool MigOutIsDone(ProcessAttr *attr, bool *isMultiNumaPid)
{
    bool ret = false;
    uint64_t remoteNum;
    pid_t pid = attr->pid;

    attr->enableSwap = false;
    if (IsMultiNumaVm(attr)) {
        *isMultiNumaPid = true;
        for (int i = 0; i < attr->remoteNumaCnt; i++) {
            int l2node = attr->migrateParam[i].nid;
            remoteNum = KBToHugePage(attr->migrateParam[i].memSize);
            if (!IsRemoteTargetMigOutDone(attr, l2node, remoteNum)) {
                return false;
            }
        }
        attr->enableSwap = true;
        ret = true;
    } else {
        int l2Node = GetAttrL2(attr);
        if (l2Node < g_processManager.nrLocalNuma || l2Node >= MAX_NODES) {
            SMAP_LOGGER_ERROR("Invalid l2Node %d of pid %d.", l2Node, pid);
            return false;
        }
        if (!GetRemoteTargetPages(attr, l2Node, &remoteNum)) {
            SMAP_LOGGER_ERROR("Pid %d has no migrate target for remote node %d.", pid, l2Node);
            return false;
        }
        if (remoteNum > attr->walkPage.nrPage) {
            SMAP_LOGGER_WARNING("Pid %d mig memSize is larger than nrPage.", attr->pid);
        }
        if (attr->walkPage.nrPage && IsRemoteTargetMigOutDone(attr, l2Node, remoteNum)) {
            attr->enableSwap = true;
            ret = true;
        }
    }

    return ret;
}

static void SetPayloadValue(struct AccessAddPidPayload *payload, struct MigPidRemoteNumaIoctlMsg *msg, int len)
{
    int runMode = GetRunMode();
    uint64_t srcMemSize;
    int l1node;
    int l2node;
    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < len; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(msg->payloads[i].pid);
        if (!attr) {
            SMAP_LOGGER_ERROR("GetProcessAttrLocked pid %d null.", msg->payloads[i].pid);
            continue;
        }
        payload[i].pid = attr->pid;
        payload[i].numaNodes = attr->numaAttr.numaNodes;
        l1node = GetAttrL1(attr);
        l2node = msg->payloads[i].srcNid;
        // 远端单numa->远端多numa，使用AddL2ByNid
        if (runMode == WATERLINE_MODE) {
            if (msg->payloads[i].ratio >= attr->strategyAttr.initRemoteMemRatio[l1node][l2node - nrLocalNuma]) {
                ClearNodeBit(&payload[i].numaNodes, l2node + (LOCAL_NUMA_BITS - nrLocalNuma));
            }
        } else { // MEM_POOL_MODE
            if (msg->payloads[i].memSize >= attr->strategyAttr.memSize[l1node][l2node - nrLocalNuma]) {
                ClearNodeBit(&payload[i].numaNodes, l2node + (LOCAL_NUMA_BITS - nrLocalNuma));
            }
        }

        AddL2ByNid(&payload[i].numaNodes, msg->payloads[i].destNid);
        payload[i].scanTime = attr->scanTime;
        payload[i].duration = attr->duration;
        payload[i].type = attr->scanType;
    }
}

int ChangePidRemoteByPid(struct MigPidRemoteNumaIoctlMsg *msg)
{
    int maxProcessCnt = GetCurrentMaxNrPid();
    if (!msg || !msg->payloads || !msg->migResArray || msg->pidCnt <= 0 || msg->pidCnt > maxProcessCnt) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByPid msg invalid.");
        return -EINVAL;
    }

    struct AccessAddPidPayload *payload = malloc(sizeof(struct AccessAddPidPayload) * maxProcessCnt);
    if (!payload) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByPid malloc payload failed.");
        return -ENOMEM;
    }

    EnvMutexLock(&g_processManager.lock);
    SetPayloadValue(payload, msg, msg->pidCnt);
    SMAP_LOGGER_INFO("ChangePidRemoteByPid ioctl begin, len: %d.", msg->pidCnt);
    int ret = AccessIoctlAddPid(msg->pidCnt, payload);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        EnvMutexUnlock(&g_processManager.lock);
        return ret;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl done.");
    for (int i = 0; i < msg->pidCnt; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(msg->payloads[i].pid);
        if (!attr) {
            continue;
        }
        int srcNode = msg->payloads[i].srcNid - g_processManager.nrLocalNuma;
        int destNode = msg->payloads[i].destNid - g_processManager.nrLocalNuma;
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, msg->payloads[i].srcNid,
                         msg->payloads[i].destNid);
        if (GetL1Count(attr->numaAttr.numaNodes) > 1) { // 容器本地多numa
            for (int j = 0; j < g_processManager.nrLocalNuma; j++) {
                attr->strategyAttr.remoteNrPagesAfterMigrate[j][destNode] +=
                    attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode];
                attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode] = 0;
            }
        } else {
            int l1node = GetAttrL1(attr);
            attr->strategyAttr.remoteNrPagesAfterMigrate[l1node][destNode] += msg->payloads[i].successCnt;
            attr->strategyAttr.remoteNrPagesAfterMigrate[l1node][srcNode] -= msg->payloads[i].successCnt;
        }

        ChangePidRemoteMemory(attr, srcNode, destNode, msg->payloads[i].memSize, msg->payloads[i].ratio);
        /* Pair planning and V1 persistence both use targetConfig as their source of truth. */
        ret = MoveProcessRemoteTarget(&attr->targetConfig, msg->payloads[i].srcNid, msg->payloads[i].destNid,
                                      msg->payloads[i].memSize, msg->payloads[i].ratio);
        if (ret) {
            SMAP_LOGGER_WARNING("Pid %d move Pair target %d to %d failed: %d.", attr->pid, msg->payloads[i].srcNid,
                                msg->payloads[i].destNid, ret);
        } else {
            attr->remoteNumaCnt = attr->targetConfig.count;
        }
    }
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid after change remote maybe failed: %d.", ret);
    }
    EnvMutexUnlock(&g_processManager.lock);
    return 0;
}

bool IsMemoryLow(pid_t pid)
{
    bool isLow = false;
    EnvMutexLock(&g_processManager.lock);
    ProcessAttr *process = GetProcessAttrLocked(pid);
    if (process && process->isLowMem) {
        SMAP_LOGGER_INFO("Pid %d dest nid memory is low.", pid);
        isLow = true;
    }
    EnvMutexUnlock(&g_processManager.lock);
    return isLow;
}
