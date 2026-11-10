/*
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
#include "node_matrix.h"
#include "manage_internal.h"

void InitProcessTargetConfig(ProcessTargetConfig *config)
{
    if (!config) {
        return;
    }

    *config = (ProcessTargetConfig){
        .sourceLocalNid = NUMA_NO_NODE,
    };
}

void ClearProcessTargetConfig(ProcessTargetConfig *config)
{
    InitProcessTargetConfig(config);
}

int CopyProcessTargetConfig(ProcessTargetConfig *dest, const ProcessTargetConfig *src)
{
    if (!dest || !src || src->count > SMAP_MAX_NUMNODES ||
        (src->migrateMode != MIG_RATIO_MODE && src->migrateMode != MIG_MEMSIZE_MODE)) {
        return -EINVAL;
    }

    *dest = *src;
    return 0;
}

bool RemoveProcessRemoteTarget(ProcessTargetConfig *config, int remoteNid)
{
    if (!config || config->count > SMAP_MAX_NUMNODES) {
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
        if (config->count == 0) {
            config->sourceLocalNid = NUMA_NO_NODE;
        }
        return true;
    }
    return false;
}

int MoveProcessRemoteTarget(ProcessTargetConfig *config, int srcNid, int destNid, uint64_t memSizeKB, int ratio)
{
    if (!config || config->count > SMAP_MAX_NUMNODES || srcNid == destNid ||
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
        if (config->count == SMAP_MAX_NUMNODES) {
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
    int nrLocalNuma = GetNrLocalNuma();
    if (copy.sourceLocalNid < NUMA_NO_NODE || copy.sourceLocalNid >= nrLocalNuma) {
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
    if (!config || config->count > SMAP_MAX_NUMNODES) {
        return NULL;
    }

    for (uint32_t i = 0; i < config->count; i++) {
        if (config->targets[i].remoteNid == remoteNid) {
            return &config->targets[i];
        }
    }
    return NULL;
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
    attr->pendingSeq = 0;
    attr->syncWaitRemoteEmpty = false;
    attr->syncWaitRemoteEmptySnapshotValid = false;
    attr->managedLocalState = (ManagedLocalState){ 0 };
}

int BuildProcessTargetConfigFromParam(const ProcessParam *param, ProcessTargetConfig *config)
{
    if (!param || !config || param->count < 0 || param->count > SMAP_MAX_NUMNODES) {
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
void MigratePagesToRemote(ProcessAttr *attr, int remoteNid, const uint64_t pagesPerNuma[SMAP_MAX_NUMNODES],
                          uint64_t pages)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t pagesToMigrate = pages;

    for (int i = 0; i < nrLocalNuma && pagesToMigrate > 0; i++) {
        if (InAttrL1(attr, i)) {
            uint64_t allocPages = MIN(pagesPerNuma[i], pagesToMigrate);
            NM_AT(attr->strategyAttr.memSize, i, remoteNid) += allocPages * (pageSize / KIB);
            pagesToMigrate -= allocPages;
        }
    }
}

static void ClearSingleRemoteTarget(ProcessAttr *attr, int remoteNid, int nrLocalNuma)
{
    for (int i = 0; i < nrLocalNuma; i++) {
        NM_AT(attr->strategyAttr.memSize, i, remoteNid) = 0;
        NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid) = 0;
    }
}

static void AccountExistingRemotePagesByOldAccount(ProcessAttr *attr, int remoteNid, uint64_t remotePages,
                                                   uint64_t oldTotal, int nrLocalNuma)
{
    uint64_t nrLeft = remotePages;

    for (int i = 0; i < nrLocalNuma; i++) {
        if (!InAttrL1(attr, i)) {
            continue;
        }

        uint64_t oldPages = NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid);
        uint64_t nrPages = oldTotal == 0 ? 0 : remotePages * oldPages / oldTotal;
        if (nrPages > nrLeft) {
            nrPages = nrLeft;
        }
        NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid) = nrPages;
        nrLeft -= nrPages;
    }

    for (int i = nrLocalNuma - 1; i >= 0 && nrLeft > 0; i--) {
        if (!InAttrL1(attr, i)) {
            continue;
        }
        NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid) += nrLeft;
        break;
    }
}

static void AccountExistingRemotePagesByLocalPages(ProcessAttr *attr, int remoteNid,
                                                   const uint64_t pagesPerNuma[SMAP_MAX_NUMNODES], uint64_t remotePages,
                                                   int nrLocalNuma)
{
    uint64_t localTotal = 0;
    uint64_t nrLeft = remotePages;

    for (int i = 0; i < nrLocalNuma; i++) {
        if (InAttrL1(attr, i)) {
            localTotal += pagesPerNuma[i];
        }
    }

    for (int i = 0; i < nrLocalNuma; i++) {
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
        NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid) = nrPages;
        nrLeft -= nrPages;
    }

    for (int i = nrLocalNuma - 1; i >= 0 && nrLeft > 0; i--) {
        if (!InAttrL1(attr, i)) {
            continue;
        }
        NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid) += nrLeft;
        break;
    }
}

static void AccountExistingRemotePages(ProcessAttr *attr, int remoteNid, const uint64_t pagesPerNuma[SMAP_MAX_NUMNODES],
                                       uint64_t remotePages, int nrLocalNuma)
{
    uint64_t oldTotal = 0;

    if (remotePages == 0) {
        return;
    }

    for (int i = 0; i < nrLocalNuma; i++) {
        if (InAttrL1(attr, i)) {
            oldTotal += NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid);
        }
    }

    if (oldTotal > 0) {
        AccountExistingRemotePagesByOldAccount(attr, remoteNid, remotePages, oldTotal, nrLocalNuma);
    } else {
        AccountExistingRemotePagesByLocalPages(attr, remoteNid, pagesPerNuma, remotePages, nrLocalNuma);
    }
}

static void SetSingleRemoteTargetPages(ProcessAttr *attr, int remoteNid, const uint64_t pagesPerNuma[SMAP_MAX_NUMNODES],
                                       uint64_t targetPages, int nrLocalNuma)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    uint64_t nrLeft = targetPages;
    int lastLocal = NUMA_NO_NODE;

    for (int i = 0; i < nrLocalNuma && nrLeft > 0; i++) {
        if (!InAttrL1(attr, i)) {
            continue;
        }

        uint64_t accounted = NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, remoteNid);
        uint64_t capacity = accounted + pagesPerNuma[i];
        uint64_t nrPages = MIN(capacity, nrLeft);
        NM_AT(attr->strategyAttr.memSize, i, remoteNid) = nrPages * (pageSize / KIB);
        nrLeft -= nrPages;
        lastLocal = i;
    }

    if (nrLeft > 0 && lastLocal != NUMA_NO_NODE) {
        NM_AT(attr->strategyAttr.memSize, lastLocal, remoteNid) += nrLeft * (pageSize / KIB);
    }
}

/* Recall pages from remote NUMA in reverse order (NUMA(n-1) -> ... -> NUMA1 -> NUMA0) */
void RecallPagesFromRemote(ProcessAttr *attr, int remoteNid, uint64_t pages)
{
    uint32_t pageSize = IsHugeMode() ? GetHugePageSize() : GetNormalPageSize();
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t pagesToRecall = pages;

    for (int i = nrLocalNuma - 1; i >= 0 && pagesToRecall > 0; i--) {
        if (InAttrL1(attr, i)) {
            uint64_t existingMemSizePages = NM_AT(attr->strategyAttr.memSize, i, remoteNid) / (pageSize / KIB);
            uint64_t recallPages = MIN(existingMemSizePages, pagesToRecall);
            NM_AT(attr->strategyAttr.memSize, i, remoteNid) -= recallPages * (pageSize / KIB);
            pagesToRecall -= recallPages;
        }
    }
}

/* Handle single remote NUMA scenario: single local+single remote, or multi-local+single remote */
int SetSingleRemoteNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma,
                              const uint64_t pagesPerNuma[SMAP_MAX_NUMNODES])
{
    if (!pagesPerNuma) {
        return -EINVAL;
    }

    int remoteNid = param->numaParam[0].nid;

    /* Validate remote NUMA node */
    if (!IsRemoteNidValid(remoteNid)) {
        SMAP_LOGGER_WARNING("Invalid remote numa %d for pid %d, nrLocalNuma: %d.", remoteNid, attr->pid, nrLocalNuma);
        return -EINVAL;
    }

    /* Calculate target pages and pages already on remote NUMA */
    uint64_t targetPages = IsHugeMode() ? KBToHugePage(param->numaParam[0].memSize) :
                                          KBToNormalPage(param->numaParam[0].memSize);
    uint64_t remoteExistingPages = pagesPerNuma[remoteNid];

    /* Set ratio for all local NUMAs */
    for (int i = 0; i < nrLocalNuma; i++) {
        NM_AT(attr->strategyAttr.initRemoteMemRatio, i, remoteNid) = param->numaParam[0].ratio;
    }

    ClearSingleRemoteTarget(attr, remoteNid, nrLocalNuma);
    AccountExistingRemotePages(attr, remoteNid, pagesPerNuma, remoteExistingPages, nrLocalNuma);
    SetSingleRemoteTargetPages(attr, remoteNid, pagesPerNuma, targetPages, nrLocalNuma);

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
    int nrLocalNuma = GetNrLocalNuma();
    int cap = (int)GetNumaCap();
    for (int i = 0; i < cap; i++) {
        attr->migrateParam[i].nid = DEFAULT_L2_NODE;
        attr->migrateParam[i].memSize = 0;
        /* initRemoteMemRatio 等 5 个数组的第一维仅分配 nrLocalNuma 行 */
        for (int j = 0; j < nrLocalNuma; j++) {
            NM_AT(attr->strategyAttr.initRemoteMemRatio, j, i) = 0;
            NM_AT(attr->strategyAttr.memSize, j, i) = 0;
            NM_AT(attr->strategyAttr.allocRemoteNrPages, j, i) = 0;
            NM_AT(attr->strategyAttr.l2RemoteMemRatio, j, i) = 0;
            NM_AT(attr->strategyAttr.l3RemoteMemRatio, j, i) = 0;
        }
    }
    /* 迁移量方向矩阵第一维按 nrLocalNuma 分配，按 [local][remote] 清零 */
    for (int j = 0; j < nrLocalNuma; j++) {
        for (int i = 0; i < cap; i++) {
            NM_AT(attr->strategyAttr.nrMigratePages, j, i) = 0;
            NM_AT(attr->strategyAttr.nrMigrateBackPages, j, i) = 0;
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

/* UpdateProcessMigrateConfig 的实现体：param 由调用方堆分配，避免 ProcessParam 栈实例 */
static int UpdateProcessMigrateConfigInner(ProcessAttr *attr, const ProcessTargetConfig *config,
                                           const ManagedLocalObservation *observation, ProcessParam *param,
                                           int nrLocalNuma, int localNumaCnt)
{
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

    TargetConfigToProcessParam(attr, config, param);
    if (config->count > 1 || localNumaCnt > 1) {
        SetMultiNumaConfig(attr, param, nrLocalNuma);
        return 0;
    }
    if (!observation || !observation->residentValid) {
        SMAP_LOGGER_ERROR("Pid %d page residency is unavailable.", attr->pid);
        return -EINVAL;
    }
    return SetSingleRemoteNumaConfig(attr, param, nrLocalNuma, observation->numaPages);
}

/* Generate compatibility runtime fields from the requested target. */
static int UpdateProcessMigrateConfig(ProcessAttr *attr, const ProcessTargetConfig *config,
                                      const ManagedLocalObservation *observation)
{
    int nrLocalNuma = GetNrLocalNuma();
    int localNumaCnt = NumaGetL1Count(&attr->numaAttr.numaNodes, GetNrLocalNuma());
    /* ProcessParam 含节点定维数组（约数十KB），超过栈使用约束，改堆分配 */
    ProcessParam *param = calloc(1, sizeof(*param));
    int ret;

    if (param == NULL) {
        return -ENOMEM;
    }
    ret = UpdateProcessMigrateConfigInner(attr, config, observation, param, nrLocalNuma, localNumaCnt);
    free(param);
    return ret;
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

/* 将 src 的置位并入 dst（两者均由运行期 cap 分配，长度一致） */
static void OrNumaBitmap(NumaBitmap *dst, const NumaBitmap *src)
{
    if (dst == NULL || src == NULL || dst->bits == NULL || src->bits == NULL) {
        return;
    }
    uint32_t nWords = dst->nWords < src->nWords ? dst->nWords : src->nWords;
    for (uint32_t w = 0; w < nWords; w++) {
        dst->bits[w] |= src->bits[w];
    }
}

static void BuildCandidateRemoteDrainNodes(ProcessAttr *candidate, const ProcessTargetConfig *config,
                                           const ManagedLocalObservation *observation, NumaBitmap *drainNodes)
{
    if (!candidate || !config || !observation || !observation->residentValid || drainNodes == NULL) {
        return;
    }

    int nrLocalNuma = GetNrLocalNuma();
    int cap = (int)GetNumaCap();
    for (int remoteNid = nrLocalNuma; remoteNid < cap; remoteNid++) {
        if (observation->numaPages[remoteNid] == 0 || FindProcessRemoteTarget(config, remoteNid)) {
            continue;
        }
        NumaBitmapSet(drainNodes, remoteNid);
        if (!InAttrL2(candidate, remoteNid)) {
            SMAP_LOGGER_INFO("Pid %d remote node %d has unmanaged resident pages; add it to drain tracking.",
                             candidate->pid, remoteNid);
        }
    }
}

static int PrepareProcessTargetCandidate(ProcessAttr *candidate, const ProcessTargetConfig *config,
                                         const ManagedLocalObservation *observation, bool skipRemoteResidencyCheck)
{
    ProcessTargetConfig targetConfig;
    NumaBitmap drainNodes = { 0 };
    bool drainNodesAllocated = false;
    int ret = CopyProcessTargetConfig(&targetConfig, config);
    if (ret) {
        return ret;
    }
    if (!skipRemoteResidencyCheck) {
        if (!NumaBitmapAlloc(&drainNodes)) {
            return -ENOMEM;
        }
        drainNodesAllocated = true;
        BuildCandidateRemoteDrainNodes(candidate, &targetConfig, observation, &drainNodes);
    }

    candidate->targetConfig = targetConfig;
    ret = ApplyManagedLocalObservation(candidate, observation, true);
    if (ret != 0) {
        goto OUT;
    }
    ret = UpdateProcessMigrateConfig(candidate, &targetConfig, observation);
    if (ret != 0) {
        goto OUT;
    }
    /* 内核不维护 numa_nodes 位图；用户态仍需记住待迁回的远端驻留节点。 */
    OrNumaBitmap(&candidate->numaAttr.numaNodes, &drainNodes);
    UpdateAutoRemoveRemoteEmptyFlag(candidate, &targetConfig);
OUT:
    if (drainNodesAllocated) {
        NumaBitmapFree(&drainNodes);
    }
    return ret;
}

void PublishProcessTargetCandidate(ProcessAttr *attr, const ProcessAttr *candidate)
{
    attr->targetConfig = candidate->targetConfig;
    attr->migrateMode = candidate->migrateMode;
    attr->remoteNumaCnt = candidate->remoteNumaCnt;
    attr->initLocalMemRatio = candidate->initLocalMemRatio;
    attr->autoRemoveWhenRemoteEmpty = candidate->autoRemoveWhenRemoteEmpty;
    attr->syncWaitRemoteEmpty = candidate->syncWaitRemoteEmpty;
    attr->syncWaitRemoteEmptySnapshotValid = candidate->syncWaitRemoteEmptySnapshotValid;
    attr->ignoreRemoteCapacity = candidate->ignoreRemoteCapacity;
    /* numaAttr 含变长位图堆缓冲区：禁止整体赋值（会把候选的 bits 指针搬给 active，
     * 候选销毁时成悬垂并二次释放）；cpuMask 逐字节拷贝，numaNodes 由下方
     * CopyProcessAttrArrays 按内容深拷贝（指针相同即栈候选共享时跳过） */
    (void)memcpy_s(attr->numaAttr.cpuMask, sizeof(attr->numaAttr.cpuMask), candidate->numaAttr.cpuMask,
                   sizeof(attr->numaAttr.cpuMask));
    attr->managedLocalState = candidate->managedLocalState;
    /* strategyAttr 含堆指针成员，禁止整体赋值（会把候选的指针搬给 active，
     * 候选销毁时成悬垂）；标量与内嵌数组逐项拷贝，指针数组按内容拷贝 */
    attr->strategyAttr.separateParam = candidate->strategyAttr.separateParam;
    (void)memcpy_s(attr->strategyAttr.nrPagesPerLocalNuma, sizeof(attr->strategyAttr.nrPagesPerLocalNuma),
                   candidate->strategyAttr.nrPagesPerLocalNuma, sizeof(attr->strategyAttr.nrPagesPerLocalNuma));
    (void)memcpy_s(attr->strategyAttr.dir, sizeof(attr->strategyAttr.dir), candidate->strategyAttr.dir,
                   sizeof(attr->strategyAttr.dir));
    (void)memcpy_s(attr->strategyAttr.ubBwRestrict, sizeof(attr->strategyAttr.ubBwRestrict),
                   candidate->strategyAttr.ubBwRestrict, sizeof(attr->strategyAttr.ubBwRestrict));
    CopyProcessAttrArrays(attr, candidate);
    attr->isFirstScan = candidate->isFirstScan;
    attr->scanTime = candidate->scanTime;
    int cap = (int)GetNumaCap();
    for (int i = 0; i < cap; i++) {
        attr->migrateParam[i] = candidate->migrateParam[i];
    }
}

int ApplyProcessTargetConfig(ProcessAttr *attr, const ProcessTargetConfig *config, bool skipRemoteResidencyCheck)
{
    ManagedLocalObservation observation;
    int ret = CollectProcessCandidateObservation(attr->pid, attr->type == VM_TYPE, &observation);
    if (ret) {
        return ret;
    }

    ProcessAttr candidate = *attr;
    ret = PrepareProcessTargetCandidate(&candidate, config, &observation, skipRemoteResidencyCheck);
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

    /*
     * The kernel tracks pages per node without a numa_nodes bitmap; staging a
     * target config must not touch tracking scope. Nothing to seed here.
     */
    attr->pendingTargetConfig = targetConfig;
    attr->pendingTargetConfigValid = true;
    attr->pendingIgnoreRemoteCapacity = ignoreRemoteCapacity;
    /* 注册槽位的调用方持有 attrLock。序号用于识别应用期间的新写入。 */
    attr->pendingSeq++;
    SMAP_LOGGER_INFO("Save pending migration target for pid %d.", attr->pid);
    return 0;
}

int ConfigureMigrationTargetsWithCapacityPolicy(ProcessAttr *attr, const ProcessTargetConfig *config,
                                                bool ignoreRemoteCapacity, bool skipRemoteResidencyCheck)
{
    ProcessTargetConfig targetConfig;
    if (!attr || ValidateProcessTargetConfig(config) || CopyProcessTargetConfig(&targetConfig, config)) {
        return -EINVAL;
    }

    if (attr->state == PROC_MIGRATE) {
        return StagePendingMigrationTargets(attr, &targetConfig, ignoreRemoteCapacity);
    }

    int ret = ApplyProcessTargetConfig(attr, &targetConfig, skipRemoteResidencyCheck);
    if (!ret) {
        attr->ignoreRemoteCapacity = ignoreRemoteCapacity;
    }
    return ret;
}

int ConfigureMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config)
{
    return ConfigureMigrationTargetsWithCapacityPolicy(attr, config, false, false);
}

int ApplyPendingMigrationTargets(ProcessAttr *attr)
{
    if (!attr) {
        return 0;
    }

    struct PidSlot *slot = PidSlotGetRefByAttr(attr);
    if (slot) {
        EnvMutexLock(&slot->attrLock);
    }
    if (!attr->pendingTargetConfigValid) {
        if (slot) {
            EnvMutexUnlock(&slot->attrLock);
            PidSlotReleaseRefs(&slot, 1);
        }
        return 0;
    }
    ProcessTargetConfig config = attr->pendingTargetConfig;
    bool ignoreRemoteCapacity = attr->pendingIgnoreRemoteCapacity;
    uint32_t pendingSeq = attr->pendingSeq;
    if (slot) {
        EnvMutexUnlock(&slot->attrLock);
    }
    ManagedLocalObservation observation;

    int ret = CollectProcessCandidateObservation(attr->pid, attr->type == VM_TYPE, &observation);
    if (ret) {
        if (slot) {
            PidSlotReleaseRefs(&slot, 1);
        }
        return ret;
    }

    ProcessAttr candidate = *attr;
    candidate.ignoreRemoteCapacity = ignoreRemoteCapacity;
    ret = PrepareProcessTargetCandidate(&candidate, &config, &observation, false);
    if (ret) {
        if (slot) {
            PidSlotReleaseRefs(&slot, 1);
        }
        return ret;
    }

    /*
     * No tracking-scope ioctl needed here: the kernel tracks pages per node
     * without a numa_nodes bitmap and the pid is already tracked, so ADD_PID
     * would be a no-op re-seed.
     */
    if (slot) {
        EnvMutexLock(&slot->attrLock);
    }
    bool stageUnchanged = attr->pendingSeq == pendingSeq;
    PublishProcessTargetCandidate(attr, &candidate);
    if (attr->syncWaitRemoteEmpty) {
        attr->syncWaitRemoteEmptySnapshotValid = false;
    }
    if (stageUnchanged) {
        ClearProcessTargetConfig(&attr->pendingTargetConfig);
        attr->pendingTargetConfigValid = false;
        attr->pendingIgnoreRemoteCapacity = false;
    } else {
        SMAP_LOGGER_INFO("Pending migration target of pid %d restaged during apply, defer to next cycle.", attr->pid);
    }
    if (slot) {
        EnvMutexUnlock(&slot->attrLock);
        PidSlotReleaseRefs(&slot, 1);
    }
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

int SetProcessConfig(ProcessAttr *attr, ProcessParam *param, bool skipRemoteResidencyCheck)
{
    ProcessTargetConfig config;
    int ret = BuildProcessTargetConfigFromParam(param, &config);
    if (ret) {
        return ret;
    }

    SetBasicProcessConfig(attr, param);
    return ConfigureMigrationTargetsWithCapacityPolicy(attr, &config, param->ignoreRemoteCapacity,
                                                       skipRemoteResidencyCheck);
}

int SetRemoteNumaInfo(int srcNid, int destNid, uint64_t size)
{
    int ret;
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
        numaInfo->sharedSize[destNid] = size;
    } else {
        NM_AT(numaInfo->privateSize, srcNid, destNid) = size;
    }
    int cap = (int)GetNumaCap();
    for (int nid = 0; nid < cap; nid++) {
        numaInfo->usedInfo[nid].ifUsedFreshed = false;
        numaInfo->usedInfo[nid].size = MBToPage(numaInfo->sharedSize[nid]);
        if (numaInfo->usedInfo[nid].size) {
            SMAP_LOGGER_INFO("Node%d shared pages: %llu.", nid, numaInfo->usedInfo[nid].size);
        }
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
            numaInfo->usedInfo[nid].size += MBToPage(NM_AT(numaInfo->privateSize, i, nid));
            NM_AT(numaInfo->privateUsedInfo, i, nid).ifUsedFreshed = false;
            NM_AT(numaInfo->privateUsedInfo, i, nid).size = MBToPage(NM_AT(numaInfo->privateSize, i, nid));
            if (NM_AT(numaInfo->privateUsedInfo, i, nid).size) {
                SMAP_LOGGER_INFO("local %d borrow remote %d private pages: %llu.", i, nid,
                                 NM_AT(numaInfo->privateUsedInfo, i, nid).size);
            }
        }
        if (numaInfo->usedInfo[nid].size) {
            SMAP_LOGGER_INFO("Node%d total borrow pages: %llu.", nid, numaInfo->usedInfo[nid].size);
        }
    }
    /* 配额变更后同步刷新 used/fresh，供紧随其后的迁回就绪检查使用。 */
    ClearRemoteMemUsed();
    CalRemoteMemUsed();
    EnvMutexUnlock(&numaInfo->lock);
    return 0;
}

bool CheckPrivateBorrowUsed(int destNid)
{
    int nrLocalNuma = GetNrLocalNuma();
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;

    for (int count = 0; count < MAX_FRESH_USED_TIME; count++) {
        EnvMutexLock(&numaInfo->lock);
        for (int i = 0; i < nrLocalNuma; i++) {
            struct RemoteNumaUsedInfo *usedInfo = &NM_AT(numaInfo->privateUsedInfo, i, destNid);
            SMAP_LOGGER_INFO("[private_borrow] local=%d remote=%d used_pages=%llu total_pages=%llu fresh=%d", i,
                             destNid, usedInfo->used, usedInfo->size, usedInfo->ifUsedFreshed);

            if (!usedInfo->ifUsedFreshed) {
                EnvMutexUnlock(&numaInfo->lock);
                EnvMsleep(WAIT_FRESH_USED_PERIOD);
                break;
            }

            // 远端配额已取消（size=0）：残留 used 须由 migrate_back 清理，
            // 不应再用 used>size 卡住 ready，否则 size 被提前清零后永远 not ready。
            if (usedInfo->size == 0) {
                if (i == nrLocalNuma - 1) {
                    EnvMutexUnlock(&numaInfo->lock);
                    return true;
                }
                continue;
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
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;

    for (int count = 0; count < MAX_FRESH_USED_TIME; count++) {
        EnvMutexLock(&numaInfo->lock);
        struct RemoteNumaUsedInfo *usedInfo = &numaInfo->usedInfo[destNid];
        SMAP_LOGGER_INFO("[total_borrow] remote=%d used_pages=%llu total_pages=%llu freshed=%d", destNid,
                         usedInfo->used, usedInfo->size, usedInfo->ifUsedFreshed);

        if (!usedInfo->ifUsedFreshed) {
            EnvMutexUnlock(&numaInfo->lock);
            EnvMsleep(WAIT_FRESH_USED_PERIOD);
            continue;
        }
        if (usedInfo->size == 0) {
            EnvMutexUnlock(&numaInfo->lock);
            return true;
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

/* 普通扫描 PID 的远端账本、快照和在途迁移均为空时返回 true。 */
static bool IsRemoteNodeDrained(int destNid)
{
    int nrLocalNuma = GetNrLocalNuma();

    uint64_t accountSum = 0;
    bool inFlight = false;
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    EnvMutexLock(&numaInfo->lock);
    for (int i = 0; i < nrLocalNuma && !inFlight; i++) {
        inFlight = NM_AT(numaInfo->privateInFlight, i, destNid) != 0;
    }
    EnvMutexUnlock(&numaInfo->lock);

    bool migrating = false;
    uint64_t snapshotSum = 0;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (!InAttrL2(attr, destNid)) {
            continue;
        }
        if (attr->state == PROC_MIGRATE || attr->state == PROC_MOVE) {
            SMAP_LOGGER_INFO("Remote node %d still has pid %d in state %d.", destNid, attr->pid, attr->state);
            migrating = true;
            break;
        }
        if (attr->groupPolicy.enabled || attr->scanType != NORMAL_SCAN) {
            continue;
        }
        for (int l = 0; l < nrLocalNuma; l++) {
            accountSum += NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, l, destNid);
        }
        snapshotSum += attr->walkPage.nrPages[destNid];
    }
    PidSlotReleaseRefs(all, n);

    if (inFlight) {
        SMAP_LOGGER_INFO("Remote node %d still has in-flight migrate outbound pages.", destNid);
        return false;
    }
    if (migrating) {
        return false;
    }
    if (accountSum != 0 || snapshotSum != 0) {
        SMAP_LOGGER_INFO("Remote node %d not drained: account pages %llu, snapshot pages %llu.", destNid, accountSum,
                         snapshotSum);
        return false;
    }
    return true;
}

/* 检查远端节点的 shared/private 配额是否均为零。 */
static bool IsRemoteQuotaCleared(int destNid)
{
    int nrLocalNuma = GetNrLocalNuma();
    if (!IsRemoteNidValid(destNid)) {
        return false;
    }
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    EnvMutexLock(&numaInfo->lock);
    bool cleared = numaInfo->sharedSize[destNid] == 0;
    for (int i = 0; i < nrLocalNuma && cleared; i++) {
        cleared = NM_AT(numaInfo->privateSize, i, destNid) == 0;
    }
    EnvMutexUnlock(&numaInfo->lock);
    return cleared;
}

bool CheckReadyMigrateBack(int destNid)
{
    if (!IsRemoteNidValid(destNid)) {
        SMAP_LOGGER_ERROR("Invalid remote node %d for migrate back readiness check.", destNid);
        return false;
    }

    // 如果已经没有管理中的虚机，则默认可以执行迁回
    if (PidSlotEmpty(GetProcessManager())) {
        SMAP_LOGGER_INFO("CheckReadyMigrateBack no process, destNid %d.", destNid);
        return true;
    }
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    // 配额清零后，还需检查普通扫描 PID 的远端账本和快照。
    bool quotaCleared = IsRemoteQuotaCleared(destNid);

    int nrWait = 0;
    while (nrWait < MAX_MIGRATE_BACK_WAIT_TIME) {
        SMAP_LOGGER_INFO("Wait until ready to migrate back, destNid: %d, nrWait: %d.", destNid, nrWait);
        EnvMutexLock(&numaInfo->lock);
        bool flag = numaInfo->sharedSize[destNid] > 0;
        EnvMutexUnlock(&numaInfo->lock);
        bool ready;
        if (flag) {
            ready = CheckBorrowUsed(destNid);
        } else {
            ready = CheckPrivateBorrowUsed(destNid);
        }
        if (ready && quotaCleared) {
            // 设0后必须等 per-pid 流水把远端清空（账本结算+在途归零+无活跃流水），
            // 否则显式迁回 ioctl 会与在途迁出竞争内核页面，导致概率性迁不回
            if (!IsRemoteNodeDrained(destNid)) {
                ready = false;
            }
        }
        if (ready) {
            return true;
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
        struct PidSlot *slot = PidSlotGetRef(pidArr[i]);
        if (!slot) {
            SMAP_LOGGER_INFO("pid %d is not in smap list.", pidArr[i]);
            continue;
        }
        ProcessAttr *attr = slot->attr;
        EnvMutexLock(&slot->attrLock);
        enum ProcessState st = attr->state;
        EnvMutexUnlock(&slot->attrLock);
        PutProcessAttr(attr);
        SMAP_LOGGER_DEBUG("pid %d actual state %d.", pidArr[i], st);
        if (enable == DISABLE_PROCESS_MIGRATE && (st != PROC_IDLE && st != PROC_MOVE)) {
            return 0;
        }
        if (enable == ENABLE_PROCESS_MIGRATE && st == PROC_BACK) {
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
        ProcessAttr *attr = GetProcessAttr(pidArr[i]);
        if (!attr) {
            SMAP_LOGGER_INFO("pid %d is not in smap list.", pidArr[i]);
            PutProcessAttr(attr);
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d actual state %d, expected state %d.", pidArr[i], attr->state, state);
        if (attr->state != state) {
            PutProcessAttr(attr);
            return 0;
        }
        PutProcessAttr(attr);
    }
    return 1;
}

void SetPidArrState(pid_t *pidArr, int len, enum ProcessState state, int enable)
{
    for (int i = 0; i < len; i++) {
        struct PidSlot *slot = PidSlotGetRef(pidArr[i]);
        if (!slot) {
            continue;
        }
        ProcessAttr *attr = slot->attr;
        EnvMutexLock(&slot->attrLock);
        /* enable == 1时，迁移状态的pid也视为合理状态，不需要设置为空闲态 */
        if (enable == ENABLE_PROCESS_MIGRATE && attr->state == PROC_MIGRATE) {
            SMAP_LOGGER_DEBUG("pid %d is in PROC_MIGRATE state.", attr->pid);
            EnvMutexUnlock(&slot->attrLock);
            PutProcessAttr(attr);
            continue;
        }
        attr->state = state;
        EnvMutexUnlock(&slot->attrLock);
        PutProcessAttr(attr);
    }
}

/*
 * 检查使用指定l2Node的所有pid是否都处于state态
 *
 * 返回值：false-否，true-是
 */
bool IsAllL2NodePidInState(enum ProcessState state, int l2Node)
{
    bool result = true;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (NotEqualToAttrL2(attr, l2Node)) {
            continue;
        }
        if (attr->state != state) {
            result = false;
            break;
        }
    }
    PidSlotReleaseRefs(all, n);
    return result;
}

/* 回收本文件 payload 数组内深拷贝出的变长位图（幂等；与 smap_interface 同名会重复定义，故独立命名） */
static void FreeRemotePayloadBitmaps(struct AccessAddPidPayload *payload, int count)
{
    for (int i = 0; i < count; i++) {
        NumaBitmapFree(&payload[i].numaNodes);
    }
}

static void SetChangePidRemoteMsgPayload(int srcNid, int destNid, int *i, int maxProcessCnt,
                                         struct AccessAddPidPayload *payload)
{
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n && *i < maxProcessCnt; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (NotEqualToAttrL2(attr, srcNid)) {
            continue;
        }
        SMAP_LOGGER_INFO("ready to change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        payload[*i].pid = attr->pid;
        /* 需在副本上改位图，必须深拷贝，避免污染 attr 自身位图 */
        (void)NumaBitmapCopy(&payload[*i].numaNodes, &attr->numaAttr.numaNodes);
        SetL2ByNid(&payload[*i].numaNodes, destNid);
        payload[*i].scanTime = attr->scanTime;
        payload[*i].duration = attr->duration;
        payload[*i].type = attr->scanType;
        payload[*i].pidType = attr->type;
        (*i)++;
    }
    PidSlotReleaseRefs(all, n);
}

static void ChangePidRemoteMemory(ProcessAttr *attr, int srcNid, int destNid, uint64_t memSize, int ratio)
{
    int nrLocalNuma = GetNrLocalNuma();
    int l1node;
    if (GetRunMode() == WATERLINE_MODE) {
        l1node = GetAttrL1(attr);
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            attr->migrateParam[0].nid = destNid;
        } else {
            /* l1node 未命中 L1 时返回 NUMA_NO_NODE，不可作 initRemoteMemRatio 下标 */
            if (l1node >= 0 && l1node < nrLocalNuma &&
                ratio >= NM_AT(attr->strategyAttr.initRemoteMemRatio, l1node, srcNid)) {
                NumaBitmapClear(&attr->numaAttr.numaNodes, srcNid);
            }
        }
        bool migOutAll = true;
        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
            if (!InAttrL1(attr, i)) {
                continue;
            }
            if (attr->migrateMode == MIG_MEMSIZE_MODE && NM_AT(attr->strategyAttr.memSize, i, srcNid) == 0) {
                continue;
            }
            NM_AT(attr->strategyAttr.initRemoteMemRatio, i, destNid) += ratio;
            NM_AT(attr->strategyAttr.initRemoteMemRatio, i, srcNid) -= ratio;
            NM_AT(attr->strategyAttr.memSize, i, destNid) += memSize;
            if (NM_AT(attr->strategyAttr.memSize, i, srcNid) >= memSize) {
                NM_AT(attr->strategyAttr.memSize, i, srcNid) -= memSize;
            } else {
                NM_AT(attr->strategyAttr.memSize, i, srcNid) = 0;
            }
            if (attr->migrateMode == MIG_MEMSIZE_MODE && NM_AT(attr->strategyAttr.memSize, i, srcNid) != 0) {
                migOutAll = false;
            }

            SMAP_LOGGER_INFO("[change_remote] pid=%d local=%d old_remote=%d new_remote=%d old_sz=%llu new_sz=%llu",
                             attr->pid, i, srcNid, destNid, NM_AT(attr->strategyAttr.memSize, i, srcNid),
                             NM_AT(attr->strategyAttr.memSize, i, destNid));
        }
        /* 部分迁移时保留原节点L2位，否则同源节点后续迁移请求可能会被拒绝 */
        if (attr->migrateMode == MIG_MEMSIZE_MODE && migOutAll) {
            NumaBitmapClear(&attr->numaAttr.numaNodes, srcNid);
        }
    } else if (GetRunMode() == MEM_POOL_MODE) {
        uint64_t srcMemSize = 0;
        /* srcNid 可能不在 migrateParam 中（remoteNumaCnt 为 0 或 stale 失配），
         * 未命中时保持 -1，跳过对 migrateParam 的写入 */
        int remoteNidIndex = -1;
        for (int i = 0; i < attr->remoteNumaCnt; i++) {
            if (srcNid == attr->migrateParam[i].nid) {
                srcMemSize = attr->migrateParam[i].memSize;
                remoteNidIndex = i;
                break;
            }
        }
        if (memSize >= srcMemSize) {
            NumaBitmapClear(&attr->numaAttr.numaNodes, srcNid);
            if (remoteNidIndex >= 0) {
                attr->migrateParam[remoteNidIndex].nid = 0;
                attr->migrateParam[remoteNidIndex].memSize = 0;
            }
        } else if (remoteNidIndex >= 0) {
            attr->migrateParam[remoteNidIndex].memSize -= memSize;
        }

        for (int i = 0; i < GetProcessManager()->nrLocalNuma; i++) {
            if (!InAttrL1(attr, i)) {
                continue;
            }
            NM_AT(attr->strategyAttr.memSize, i, destNid) += memSize;
            if (NM_AT(attr->strategyAttr.memSize, i, srcNid) >= memSize) {
                NM_AT(attr->strategyAttr.memSize, i, srcNid) -= memSize;
            } else {
                NM_AT(attr->strategyAttr.memSize, i, srcNid) = 0;
            }
        }
    }

    AddAttrL2(attr, destNid);

    if (GetRunMode() == WATERLINE_MODE && attr->migrateMode == MIG_MEMSIZE_MODE) {
        return;
    }

    attr->remoteNumaCnt = NumaGetL2Count(&attr->numaAttr.numaNodes, GetNrLocalNuma());
    SMAP_LOGGER_INFO("========= remoteNumaCnt %d", attr->remoteNumaCnt);
    int targetIdx = -1;
    int zeroIdx = -1;

    for (int i = 0; i < attr->remoteNumaCnt; i++) {
        if (attr->migrateParam[i].nid == destNid) {
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
        attr->migrateParam[zeroIdx].nid = destNid;
        attr->migrateParam[zeroIdx].memSize = memSize;
    }
}

static void ChangePidRemoteMemoryByNuma(ProcessAttr *attr, int srcNid, int destNid)
{
    int nrLocalNuma = GetProcessManager()->nrLocalNuma;
    for (int i = 0; i < nrLocalNuma; i++) {
        NM_AT(attr->strategyAttr.initRemoteMemRatio, i, destNid) =
            NM_AT(attr->strategyAttr.initRemoteMemRatio, i, srcNid);
        NM_AT(attr->strategyAttr.initRemoteMemRatio, i, srcNid) = 0;
        NM_AT(attr->strategyAttr.memSize, i, destNid) = NM_AT(attr->strategyAttr.memSize, i, srcNid);
        NM_AT(attr->strategyAttr.memSize, i, srcNid) = 0;
    }
    for (int i = 0; i < attr->remoteNumaCnt; i++) {
        if (attr->migrateParam[i].nid == srcNid)
            attr->migrateParam[i].nid = destNid;
    }
}

/*
 * Move the targetConfig entry from srcNid to destNid so that the stale srcNid
 * is no longer a migration target; kernel tracking of srcNid drains naturally
 * via idle reclaim once its pages are gone.
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
    ProcessAttr *attr;
    struct AccessAddPidPayload *payload = calloc(maxProcessCnt, sizeof(struct AccessAddPidPayload));
    if (!payload) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma malloc payload failed.");
        return -ENOMEM;
    }

    SetChangePidRemoteMsgPayload(srcNid, destNid, &i, maxProcessCnt, payload);
    if (i == 0) {
        SMAP_LOGGER_INFO("ChangePidRemoteByNuma len: %d, no need to change.", i);
        FreeRemotePayloadBitmaps(payload, maxProcessCnt);
        free(payload);
        return 0;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl begin, len: %d.", i);
    int ret = AccessIoctlAddPid(i, payload);
    FreeRemotePayloadBitmaps(payload, maxProcessCnt);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        return ret;
    }
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        attr = all[k]->attr;
        if (NotEqualToAttrL2(attr, srcNid)) {
            continue;
        }
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        for (int j = 0; j < GetProcessManager()->nrLocalNuma; j++) {
            NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, destNid) +=
                NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, srcNid);
            NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, srcNid) = 0;
        }
        ChangePidRemoteMemoryByNuma(attr, srcNid, destNid);
        MoveProcessTargetConfig(attr, srcNid, destNid);
        SetAttrL2(attr, destNid);
    }
    PidSlotReleaseRefs(all, n);
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid after change remote maybe failed: %d.", ret);
    }
    return 0;
}

int EnableProcessMigrate(pid_t *pidArr, int len, int enable)
{
    int retry = WAIT_PROC_STATE_MAX_RETRY;
    enum ProcessState newState;
    newState = enable == ENABLE_PROCESS_MIGRATE ? PROC_IDLE : PROC_MOVE;

    SMAP_LOGGER_DEBUG("enter EnableProcessMigrate.");
    while (true) {
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
            return 0;
        }
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
    int result = 1;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state == PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d == PROC_MOVE.", attr->pid, attr->state);
            result = 0;
            break;
        }
    }
    PidSlotReleaseRefs(all, n);
    return result;
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
    int result = 1;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (NotEqualToAttrL2(attr, nid)) {
            continue;
        }
        SMAP_LOGGER_DEBUG("pid %d state: %d.", attr->pid, attr->state);
        if (attr->state != PROC_MOVE) {
            SMAP_LOGGER_INFO("pid %d state %d != PROC_MOVE.", attr->pid, attr->state);
            result = 0;
            break;
        }
    }
    PidSlotReleaseRefs(all, n);
    return result;
}

static bool IsRemoteTargetMigOutDone(ProcessAttr *attr, int remoteNid, uint64_t targetPages)
{
    if (!IsRemoteNidValid(remoteNid)) {
        SMAP_LOGGER_ERROR("Invalid remote node %d of pid %d.", remoteNid, attr->pid);
        return false;
    }

    uint64_t accountedPages = 0;
    /* remoteNrPagesAfterMigrate 第一维仅分配 nrLocalNuma 行 */
    int nrLocalNuma = GetNrLocalNuma();
    for (int local = 0; local < nrLocalNuma; local++) {
        accountedPages += NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, local, remoteNid);
    }
    uint64_t remotePages = attr->walkPage.nrPages[remoteNid];
    SMAP_LOGGER_INFO("Pid: %d, remote node: %d, target pages: %llu, accounted pages: %llu, current remote pages: %llu.",
                     attr->pid, remoteNid, targetPages, accountedPages, remotePages);

    if (targetPages == 0) {
        return accountedPages == 0 && remotePages == 0;
    }
    if (accountedPages == targetPages) {
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
    *isMultiNumaPid = IsMultiNumaVm(attr);
    /* 新迁移目标已暂存未生效：migrateParam 仍是上一轮配置，用它判定完成会在上一次
     * sync 刚结束时误判成功（旧账本/旧远端页数恰好等于旧目标），导致上层提前
     * remove。等待迁移周期 ApplyPendingMigrationTargets 生效后再判定。 */
    if (attr->pendingTargetConfigValid) {
        SMAP_LOGGER_INFO("Pid %d has a pending migration target, mig out is not done yet.", pid);
        return false;
    }
    if (attr->syncWaitRemoteEmpty && !attr->syncWaitRemoteEmptySnapshotValid) {
        SMAP_LOGGER_INFO("Pid %d is waiting for a fresh remote-empty page snapshot.", pid);
        return false;
    }
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
        if (!IsRemoteNidValid(l2Node)) {
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

static int SetPayloadValue(struct AccessAddPidPayload *payload, struct MigPidRemoteNumaIoctlMsg *msg, int len)
{
    int runMode = GetRunMode();
    int l1node;
    int l2node;
    int nrLocalNuma = GetNrLocalNuma();
    int outCnt = 0;

    for (int i = 0; i < len; i++) {
        ProcessAttr *attr = GetProcessAttr(msg->payloads[i].pid);
        if (!attr) {
            SMAP_LOGGER_ERROR("GetProcessAttr pid %d null.", msg->payloads[i].pid);
            PutProcessAttr(attr);
            continue;
        }
        int found = -1;
        for (int j = 0; j < outCnt; j++) {
            if (payload[j].pid == attr->pid) {
                found = j;
                break;
            }
        }
        if (found < 0) {
            found = outCnt++;
            payload[found].pid = attr->pid;
            /* 需在副本上改位图，必须深拷贝，避免污染 attr 自身位图 */
            (void)NumaBitmapCopy(&payload[found].numaNodes, &attr->numaAttr.numaNodes);
            payload[found].scanTime = attr->scanTime;
            payload[found].duration = attr->duration;
            payload[found].type = attr->scanType;
            payload[found].pidType = attr->type;
        }
        l1node = GetAttrL1(attr);
        l2node = msg->payloads[i].srcNid;
        /* l1node 未命中返回 NUMA_NO_NODE；l2node 为外部输入的绝对 nid，
         * 均须校验后才能作为 initRemoteMemRatio/memSize 的下标 */
        if (l1node < 0 || l1node >= nrLocalNuma || !IsRemoteNidValid(l2node)) {
            SMAP_LOGGER_ERROR("Pid %d invalid l1node %d or srcNid %d.", attr->pid, l1node, l2node);
            PutProcessAttr(attr);
            continue;
        }
        // 远端单numa->远端多numa，使用AddL2ByNid
        if (runMode == WATERLINE_MODE) {
            bool clearSrc;
            if (attr->migrateMode == MIG_MEMSIZE_MODE) {
                clearSrc = msg->payloads[i].memSize >= NM_AT(attr->strategyAttr.memSize, l1node, l2node);
            } else {
                int initRatio = NM_AT(attr->strategyAttr.initRemoteMemRatio, l1node, l2node);
                clearSrc = msg->payloads[i].ratio >= initRatio;
            }
            if (clearSrc) {
                NumaBitmapClear(&payload[found].numaNodes, l2node);
            }
        } else {
            if (msg->payloads[i].memSize >= NM_AT(attr->strategyAttr.memSize, l1node, l2node)) {
                NumaBitmapClear(&payload[found].numaNodes, l2node);
            }
        }
        AddL2ByNid(&payload[found].numaNodes, msg->payloads[i].destNid);
        PutProcessAttr(attr);
    }
    return outCnt;
}

int ChangePidRemoteByPid(struct MigPidRemoteNumaIoctlMsg *msg)
{
    int maxProcessCnt = GetCurrentMaxNrPid();
    if (!msg || !msg->payloads || !msg->migResArray || msg->pidCnt <= 0 || msg->pidCnt > maxProcessCnt) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByPid msg invalid.");
        return -EINVAL;
    }

    /* calloc 清零：SetPayloadValue 存在 continue 跳过路径，被跳过条目的其余字段不得携带堆残留垃圾下发内核 */
    struct AccessAddPidPayload *payload = calloc(maxProcessCnt, sizeof(struct AccessAddPidPayload));
    if (!payload) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByPid malloc payload failed.");
        return -ENOMEM;
    }

    int payloadCnt = SetPayloadValue(payload, msg, msg->pidCnt);
    if (payloadCnt == 0) {
        FreeRemotePayloadBitmaps(payload, maxProcessCnt);
        free(payload);
        SMAP_LOGGER_INFO("ChangePidRemoteByPid no valid payload.");
        return 0;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByPid ioctl begin, len: %d.", payloadCnt);
    int ret = AccessIoctlAddPid(payloadCnt, payload);
    FreeRemotePayloadBitmaps(payload, maxProcessCnt);
    free(payload);
    if (ret) {
        SMAP_LOGGER_ERROR("ChangePidRemoteByNuma ioctl failed: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("ChangePidRemoteByNuma ioctl done.");
    for (int i = 0; i < msg->pidCnt; i++) {
        ProcessAttr *attr = GetProcessAttr(msg->payloads[i].pid);
        if (!attr) {
            PutProcessAttr(attr);
            continue;
        }
        int srcNid = msg->payloads[i].srcNid;
        int destNid = msg->payloads[i].destNid;
        SMAP_LOGGER_INFO("change pid %d L2 from %d to %d.", attr->pid, srcNid, destNid);
        if (NumaGetL1Count(&attr->numaAttr.numaNodes, GetNrLocalNuma()) > 1) { // 容器本地多numa
            for (int j = 0; j < GetProcessManager()->nrLocalNuma; j++) {
                NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, destNid) +=
                    NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, srcNid);
                NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, j, srcNid) = 0;
            }
        } else {
            int l1node = GetAttrL1(attr);
            /* l1node 未命中 L1 时返回 NUMA_NO_NODE，不可作 remoteNrPagesAfterMigrate 行下标 */
            if (l1node >= 0 && l1node < GetNrLocalNuma()) {
                NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, l1node, destNid) += msg->payloads[i].successCnt;
                NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, l1node, srcNid) -= msg->payloads[i].successCnt;
            }
        }

        ChangePidRemoteMemory(attr, srcNid, destNid, msg->payloads[i].memSize, msg->payloads[i].ratio);
        /* Pair planning and V1 persistence both use targetConfig as their source of truth. */
        ret = MoveProcessRemoteTarget(&attr->targetConfig, srcNid, destNid, msg->payloads[i].memSize,
                                      msg->payloads[i].ratio);
        if (ret) {
            SMAP_LOGGER_WARNING("Pid %d move Pair target %d to %d failed: %d.", attr->pid, srcNid, destNid, ret);
        } else {
            attr->remoteNumaCnt = attr->targetConfig.count;
            /* Sync migrateParam from targetConfig so the strategy layer reads
             * the correct memSize after MoveProcessRemoteTarget split/move.
             * Without this, migrateParam[0].memSize stays at the stale original
             * value and the strategy layer overwrites strategyAttr.memSize with
             * the wrong number, causing accumulation in ChangePidRemoteMemory. */
            for (int j = 0; j < attr->targetConfig.count; j++) {
                attr->migrateParam[j].nid = attr->targetConfig.targets[j].remoteNid;
                attr->migrateParam[j].memSize = attr->targetConfig.targets[j].memSizeKB;
            }
        }
        PutProcessAttr(attr);
    }
    ret = SyncAllProcessConfig();
    if (ret) {
        SMAP_LOGGER_WARNING("Synchronize pid after change remote maybe failed: %d.", ret);
    }
    return 0;
}

bool IsMemoryLow(pid_t pid)
{
    bool isLow = false;
    ProcessAttr *process = GetProcessAttr(pid);
    if (process && process->isLowMem) {
        SMAP_LOGGER_INFO("Pid %d dest nid memory is low.", pid);
        isLow = true;
    }
    PutProcessAttr(process);
    return isLow;
}
