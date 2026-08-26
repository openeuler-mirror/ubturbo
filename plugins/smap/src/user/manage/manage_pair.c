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
#include "manage_internal.h"

static uint32_t BuildPairCapacityLocalMask(const ProcessAttr *attr, int remoteIndex)
{
    uint32_t capacityLocalMask = 0;
    uint32_t managedLocalMask = attr->managedLocalState.managedLocalMask;
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
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
 * The caller must hold GetProcessManager()->lock.
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
                                  size_t entryCap, size_t *entryCount, bool migrateOnly, struct PidSlot *all[],
                                  size_t *allCnt)
{
    size_t count = 0;
    size_t n = PidSlotCollectRefs((struct ProcessManager *)manager, all, MAX_PID_SLOTS);
    *allCnt = n;
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
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
                                              uint64_t totalCapacity[REMOTE_NUMA_NUM], bool migrateOnly,
                                              struct PidSlot *all[], size_t *allCnt)
{
    uint64_t sharedCapacity[REMOTE_NUMA_NUM] = { 0 };
    int ret = BuildPairCapacitySnapshot(manager, privateCapacity, sharedCapacity, totalCapacity);
    if (ret) {
        return ret;
    }
    ret = CollectAllPairRequests(manager, privateCapacity, sharedCapacity, entries, entryCap, entryCount, migrateOnly,
                                 all, allCnt);
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

    struct PidSlot *all[MAX_PID_SLOTS];
    size_t allCnt = 0;
    EnvMutexLock(&manager->remoteNumaInfo.lock);
    ret = BuildPairArbitrationSnapshotLocked(manager, entries, targetCap, &entryCount, privateCapacity, totalCapacity,
                                             false, all, &allCnt);
    if (!ret) {
        PublishPairCapacityResult(manager, entries, entryCount, privateCapacity, totalCapacity);
        for (size_t i = 0; i < entryCount; i++) {
            targets[i] = entries[i].target;
        }
        *targetCnt = entryCount;
    }
    EnvMutexUnlock(&manager->remoteNumaInfo.lock);
    PidSlotReleaseRefs(all, allCnt);
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

    struct PidSlot *all[MAX_PID_SLOTS];
    size_t allCnt = 0;
    EnvMutexLock(&manager->remoteNumaInfo.lock);
    ret = BuildPairArbitrationSnapshotLocked(manager, entries, planCap, &entryCount, privateCapacity, totalCapacity,
                                             migrateOnly, all, &allCnt);
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
    PidSlotReleaseRefs(all, allCnt);
    free(entries);
    return ret;
}

int BuildAllPairPlanInputs(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                           PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt)
{
    return BuildAllPairPlanInputsForState(manager, plans, planCap, planCnt, pidBudgets, pidBudgetCap, pidBudgetCnt,
                                          false);
}
