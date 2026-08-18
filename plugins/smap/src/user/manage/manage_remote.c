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

int ValidateProcessTargetConfig(const ProcessTargetConfig *config)
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

int BuildProcessTargetConfigFromParam(const ProcessParam *param, ProcessTargetConfig *config)
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

/* Migrate additional pages to remote NUMA in forward order (NUMA0 -> NUMA1 -> ...) */
void MigratePagesToRemote(ProcessAttr *attr, int l2Index, const uint64_t pagesPerNuma[MAX_NODES], uint64_t pages)
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
void RecallPagesFromRemote(ProcessAttr *attr, int l2Index, uint64_t pages)
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
int SetSingleRemoteNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma,
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
    if (config->count > 1 || localNumaCnt > 1) {
        SetMultiNumaConfig(attr, &param, nrLocalNuma);
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

void PublishProcessTargetCandidate(ProcessAttr *attr, const ProcessAttr *candidate)
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
    attr->isFirstScan = candidate->isFirstScan;
    attr->scanTime = candidate->scanTime;
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        attr->migrateParam[i] = candidate->migrateParam[i];
    }
}

int ApplyProcessTargetConfig(ProcessAttr *attr, const ProcessTargetConfig *config)
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

int StagePendingMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config, bool ignoreRemoteCapacity)
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

int ConfigureMigrationTargetsWithCapacityPolicy(ProcessAttr *attr, const ProcessTargetConfig *config,
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
        .pidType = attr->type,
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

bool IsZeroRemoteTargetConfig(ProcessParam *param)
{
    ProcessTargetConfig config;
    if (BuildProcessTargetConfigFromParam(param, &config)) {
        return false;
    }
    return IsZeroProcessTargetConfig(&config);
}

int SetProcessConfig(ProcessAttr *attr, ProcessParam *param)
{
    ProcessTargetConfig config;
    int ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        return ret;
    }

    SetBasicProcessConfig(attr, param);
    return ConfigureMigrationTargetsWithCapacityPolicy(attr, &config, param->ignoreRemoteCapacity);
}

int SetRemoteNumaInfo(int srcNid, int destNid, uint64_t size)
{
    int ret;
    int column = destNid - GetProcessManager()->nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
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
        int l2Nid = j + GetProcessManager()->nrLocalNuma;
        numaInfo->usedInfo[j].ifUsedFreshed = false;
        numaInfo->usedInfo[j].size = MBToPage(numaInfo->sharedSize[j]);
        if (numaInfo->usedInfo[j].size) {
            SMAP_LOGGER_INFO("Node%d shared pages: %llu.", l2Nid, numaInfo->usedInfo[j].size);
        }
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
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

bool CheckPrivateBorrowUsed(int destNid)
{
    int nrLocalNuma = GetNrLocalNuma();
    int column = destNid - nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;

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

bool CheckBorrowUsed(int destNid)
{
    int column = destNid - GetProcessManager()->nrLocalNuma;
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;

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
    EnvMutexLock(&GetProcessManager()->lock);
    if (!GetProcessManager()->processes) {
        SMAP_LOGGER_INFO("CheckReadyMigrateBack no process, destNid %d.", destNid);
        EnvMutexUnlock(&GetProcessManager()->lock);
        return true;
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    int column = destNid - GetProcessManager()->nrLocalNuma;

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

void SetPidArrState(pid_t *pidArr, int len, enum ProcessState state, int enable)
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
    EnvMutexLock(&GetProcessManager()->lock);
    for (ProcessAttr *attr = GetProcessManager()->processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, l2Node)) {
            continue;
        }
        if (attr->state != state) {
            EnvMutexUnlock(&GetProcessManager()->lock);
            return false;
        }
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
    return true;
}

static void SetChangePidRemoteMsgPayload(int srcNid, int destNid, int *i, int maxProcessCnt,
                                         struct AccessAddPidPayload *payload)
{
    for (ProcessAttr *attr = GetProcessManager()->processes; attr && *i < maxProcessCnt; attr = attr->next) {
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
        payload[*i].pidType = attr->type;
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
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
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

        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
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
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
            attr->strategyAttr.initRemoteMemRatio[i][destNode] = attr->strategyAttr.initRemoteMemRatio[i][srcNode];
            attr->strategyAttr.initRemoteMemRatio[i][srcNode] = 0;
        }
    } else if (GetRunMode() == MEM_POOL_MODE) {
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
            attr->strategyAttr.memSize[i][destNode] = attr->strategyAttr.memSize[i][srcNode];
            attr->strategyAttr.memSize[i][srcNode] = 0;
        }
    }
}

/*
 * Move the targetConfig entry from srcNid to destNid so that
 * BuildManagedTrackingNodes does not re-add the stale srcNid.
 */
static void MoveProcessTargetConfig(ProcessAttr *attr, int srcNid, int destNid)
{
    const ProcessRemoteTarget *srcTarget = FindProcessRemoteTarget(&attr->targetConfig, srcNid);
    if (srcTarget == NULL) {
        return;
    }
    uint64_t memSizeKB = srcTarget->memSizeKB;
    int ratio = srcTarget->ratio;
    int ret = MoveProcessRemoteTarget(&attr->targetConfig, srcNid, destNid, memSizeKB, ratio);
    /* MoveProcessRemoteTarget is a no-op when moved == 0; remove explicitly. */
    if (ret == 0 && FindProcessRemoteTarget(&attr->targetConfig, srcNid) != NULL) {
        (void)RemoveProcessRemoteTarget(&attr->targetConfig, srcNid);
    }
    if (ret != 0) {
        SMAP_LOGGER_WARNING("Pid %d move Pair target %d to %d failed: %d.", attr->pid, srcNid, destNid, ret);
    }
    attr->remoteNumaCnt = attr->targetConfig.count;
}

int ChangePidRemoteByNuma(int srcNid, int destNid)
{
    int i = 0;
    int maxProcessCnt = GetCurrentMaxNrPid();
    int srcNode = srcNid - GetProcessManager()->nrLocalNuma;
    int destNode = destNid - GetProcessManager()->nrLocalNuma;
    ProcessAttr *attr;
    struct AccessAddPidPayload *payload = malloc(sizeof(struct AccessAddPidPayload) * maxProcessCnt);
    if (!payload) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma malloc payload failed.");
        return -ENOMEM;
    }

    EnvMutexLock(&GetProcessManager()->lock);
    SetChangePidRemoteMsgPayload(srcNid, destNid, &i, maxProcessCnt, payload);
    if (i == 0) {
        SMAP_LOGGER_INFO("ChangePidRemoteByNuma len: %d, no need to change.", i);
        EnvMutexUnlock(&GetProcessManager()->lock);
        free(payload);
        return 0;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl begin, len: %d.", i);
    int ret = AccessIoctlAddPid(i, payload);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        EnvMutexUnlock(&GetProcessManager()->lock);
        return ret;
    }
    for (attr = GetProcessManager()->processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, srcNid)) {
            continue;
        }
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        for (int j = 0; j < GetProcessManager()->nrLocalNuma; j++) {
            attr->strategyAttr.remoteNrPagesAfterMigrate[j][destNode] +=
                attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode];
            attr->strategyAttr.remoteNrPagesAfterMigrate[j][srcNode] = 0;
        }
        ChangePidRemoteMemoryByNuma(attr, srcNode, destNode);
        MoveProcessTargetConfig(attr, srcNid, destNid);
        SetAttrL2(attr, destNid);
    }
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid after change remote maybe failed: %d.", ret);
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
    return 0;
}

int EnableProcessMigrate(pid_t *pidArr, int len, int enable)
{
    int retry = WAIT_PROC_STATE_MAX_RETRY;
    enum ProcessState newState;
    newState = enable == ENABLE_PROCESS_MIGRATE ? PROC_IDLE : PROC_MOVE;

    SMAP_LOGGER_DEBUG("enter EnableProcessMigrate.");
    while (true) {
        EnvMutexLock(&GetProcessManager()->lock);
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
            EnvMutexUnlock(&GetProcessManager()->lock);
            return 0;
        }
        EnvMutexUnlock(&GetProcessManager()->lock);
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
    if (nid < GetProcessManager()->nrLocalNuma) {
        return -EINVAL;
    }
    EnvMutexLock(&GetProcessManager()->lock);
    for (ProcessAttr *attr = GetProcessManager()->processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state == PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d == PROC_MOVE.", attr->pid, attr->state);
            EnvMutexUnlock(&GetProcessManager()->lock);
            return 0;
        }
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
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
    if (nid < GetProcessManager()->nrLocalNuma) {
        return -EINVAL;
    }
    EnvMutexLock(&GetProcessManager()->lock);
    for (ProcessAttr *attr = GetProcessManager()->processes; attr; attr = attr->next) {
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state != PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d != PROC_MOVE.", attr->pid, attr->state);
            EnvMutexUnlock(&GetProcessManager()->lock);
            return 0;
        }
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
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
        if (l2Node < GetProcessManager()->nrLocalNuma || l2Node >= MAX_NODES) {
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
        payload[i].pidType = attr->type;
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

    EnvMutexLock(&GetProcessManager()->lock);
    SetPayloadValue(payload, msg, msg->pidCnt);
    SMAP_LOGGER_INFO("ChangePidRemoteByPid ioctl begin, len: %d.", msg->pidCnt);
    int ret = AccessIoctlAddPid(msg->pidCnt, payload);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        EnvMutexUnlock(&GetProcessManager()->lock);
        return ret;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl done.");
    for (int i = 0; i < msg->pidCnt; i++) {
        ProcessAttr *attr = GetProcessAttrLocked(msg->payloads[i].pid);
        if (!attr) {
            continue;
        }
        int srcNode = msg->payloads[i].srcNid - GetProcessManager()->nrLocalNuma;
        int destNode = msg->payloads[i].destNid - GetProcessManager()->nrLocalNuma;
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, msg->payloads[i].srcNid,
                         msg->payloads[i].destNid);
        if (GetL1Count(attr->numaAttr.numaNodes) > 1) { // 容器本地多numa
            for (int j = 0; j < GetProcessManager()->nrLocalNuma; j++) {
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
    EnvMutexUnlock(&GetProcessManager()->lock);
    return 0;
}

bool IsMemoryLow(pid_t pid)
{
    bool isLow = false;
    EnvMutexLock(&GetProcessManager()->lock);
    ProcessAttr *process = GetProcessAttrLocked(pid);
    if (process && process->isLowMem) {
        SMAP_LOGGER_INFO("Pid %d dest nid memory is low.", pid);
        isLow = true;
    }
    EnvMutexUnlock(&GetProcessManager()->lock);
    return isLow;
}
