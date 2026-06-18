/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "securec.h"
#include "smap_user_log.h"
#include "strategy.h"
#include "grouped_strategy.h"
#include "strategy_config.h"

#define GROUP_SWAP_READY_ROUNDS 2
#define GROUP_SWAP_PERCENT_BASE 100
#define GROUP_SWAP_DEFAULT_FREQ_WT 1
#define GROUP_SWAP_DEFAULT_SLOW_THRED 2
#define GROUP_SWAP_SLOW_THRED_FACTOR 2
#define MAX_GROUP_TARGET_ENTRY (MAX_MIGRATION_GROUP_NUM * MAX_GROUP_REMOTE_NUMA)

typedef struct {
    uint64_t addr;
    actc_t freq;
    uint8_t prior;
    bool isWhiteListPage;
    int nid;
} GroupPageData;

typedef struct {
    int groupIdx;
    uint64_t remoteUsed;
    uint64_t deficit;
} PromoteGroup;

typedef struct {
    GroupPageData local;
    GroupPageData remote;
} SwapPair;

typedef struct {
    GroupPageData page;
    int toNid;
} RebalancePair;

typedef struct {
    int groupIdx;
    int targetIdx;
    uint64_t oldUsedPages;
} GroupTargetEntry;

static int GroupPageAscFunc(const void *data1, const void *data2)
{
    const GroupPageData *p1 = (const GroupPageData *)data1;
    const GroupPageData *p2 = (const GroupPageData *)data2;
    if (p1->isWhiteListPage != p2->isWhiteListPage) {
        return p1->isWhiteListPage ? 1 : -1;
    }
    if (p1->freq < p2->freq) {
        return -1;
    }
    if (p1->freq > p2->freq) {
        return 1;
    }
    return (int)p2->prior - (int)p1->prior;
}

static int GroupPageDescFunc(const void *data1, const void *data2)
{
    const GroupPageData *p1 = (const GroupPageData *)data1;
    const GroupPageData *p2 = (const GroupPageData *)data2;
    if (p1->isWhiteListPage != p2->isWhiteListPage) {
        return p1->isWhiteListPage ? 1 : -1;
    }
    if (p1->freq < p2->freq) {
        return 1;
    }
    if (p1->freq > p2->freq) {
        return -1;
    }
    return (int)p1->prior - (int)p2->prior;
}

static int ActcDataDescFunc(const void *data1, const void *data2)
{
    const ActcData *p1 = (const ActcData *)data1;
    const ActcData *p2 = (const ActcData *)data2;
    if (p1->isWhiteListPage != p2->isWhiteListPage) {
        return p1->isWhiteListPage ? 1 : -1;
    }
    if (p1->freq < p2->freq) {
        return 1;
    }
    if (p1->freq > p2->freq) {
        return -1;
    }
    return (int)p1->prior - (int)p2->prior;
}

static int PromoteGroupCmp(const void *data1, const void *data2)
{
    const PromoteGroup *g1 = (const PromoteGroup *)data1;
    const PromoteGroup *g2 = (const PromoteGroup *)data2;
    if (g1->deficit < g2->deficit) {
        return 1;
    }
    if (g1->deficit > g2->deficit) {
        return -1;
    }
    return g1->groupIdx - g2->groupIdx;
}

static bool GroupHasLocal(const MigrationGroupAttr *group, int nid)
{
    for (int i = 0; i < group->localCount; i++) {
        if (group->locals[i].nid == nid) {
            return true;
        }
    }
    return false;
}

static int GroupTargetIndex(const MigrationGroupAttr *group, int nid)
{
    for (int i = 0; i < group->targetCount; i++) {
        if (group->targets[i].nid == nid) {
            return i;
        }
    }
    return -1;
}

static bool PolicyHasNode(const GroupMigrationPolicy *policy, int nid)
{
    for (int i = 0; i < policy->groupCount; i++) {
        if (GroupHasLocal(&policy->groups[i], nid) || GroupTargetIndex(&policy->groups[i], nid) >= 0) {
            return true;
        }
    }
    return false;
}

static void WarnUngroupedPages(ProcessAttr *process)
{
    for (int nid = 0; nid < MAX_NODES; nid++) {
        if (process->scanAttr.actcLen[nid] == 0) {
            continue;
        }
        if (!PolicyHasNode(&process->groupPolicy, nid)) {
            SMAP_LOGGER_WARNING("pid %d has %llu pages on ungrouped numa %d, skipped by grouped strategy.",
                                process->pid, process->scanAttr.actcLen[nid], nid);
        }
    }
}

static uint64_t CalcLocalUsed(ProcessAttr *process, const MigrationGroupAttr *group)
{
    uint64_t used = 0;
    for (int i = 0; i < group->localCount; i++) {
        used += process->scanAttr.actcLen[group->locals[i].nid];
    }
    return used;
}

static uint64_t CalcLocalDeficit(ProcessAttr *process, const MigrationGroupAttr *group, int localIdx)
{
    int nid = group->locals[localIdx].nid;
    uint64_t used = process->scanAttr.actcLen[nid];
    uint64_t reserve = group->locals[localIdx].localReservePages;
    return used < reserve ? reserve - used : 0;
}

static uint64_t CalcLocalExcess(ProcessAttr *process, const MigrationGroupAttr *group, int localIdx)
{
    int nid = group->locals[localIdx].nid;
    uint64_t used = process->scanAttr.actcLen[nid];
    uint64_t reserve = group->locals[localIdx].localReservePages;
    return used > reserve ? used - reserve : 0;
}

static uint64_t CalcGroupDeficit(ProcessAttr *process, const MigrationGroupAttr *group)
{
    uint64_t deficit = 0;
    for (int i = 0; i < group->localCount; i++) {
        deficit += CalcLocalDeficit(process, group, i);
    }
    return deficit;
}

static uint64_t CalcGroupExcess(ProcessAttr *process, const MigrationGroupAttr *group)
{
    uint64_t excess = 0;
    for (int i = 0; i < group->localCount; i++) {
        excess += CalcLocalExcess(process, group, i);
    }
    return excess;
}

static bool IsGroupLocalSteady(ProcessAttr *process, const MigrationGroupAttr *group)
{
    uint32_t ratio = GetGroupSwapLocalWatermarkRatioConfig();
    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        uint64_t reserve = group->locals[i].localReservePages;
        uint64_t threshold = ((__uint128_t)reserve * ratio + GROUP_SWAP_PERCENT_BASE - 1) / GROUP_SWAP_PERCENT_BASE;
        if (process->scanAttr.actcLen[nid] < threshold) {
            return false;
        }
    }
    return true;
}

static uint64_t CalcRemoteUsed(const MigrationGroupAttr *group)
{
    uint64_t used = 0;
    for (int i = 0; i < group->targetCount; i++) {
        used += group->targets[i].usedPages;
    }
    return used;
}

static uint64_t CalcProcessTotalPages(const ProcessAttr *process)
{
    uint64_t total = 0;

    for (int nid = 0; nid < MAX_NODES; nid++) {
        total += process->scanAttr.actcLen[nid];
    }
    return total;
}

static bool UpdateGroupSwapTotalPagesStable(ProcessAttr *process)
{
    uint64_t totalPages = CalcProcessTotalPages(process);

    if (!process->groupSwapTotalPagesValid || process->groupSwapLastTotalPages != totalPages) {
        process->groupSwapLastTotalPages = totalPages;
        process->groupSwapStableTotalRounds = 1;
        process->groupSwapTotalPagesValid = true;
    } else if (process->groupSwapStableTotalRounds < GROUP_SWAP_READY_ROUNDS) {
        process->groupSwapStableTotalRounds++;
    }

    return process->groupSwapStableTotalRounds >= GROUP_SWAP_READY_ROUNDS;
}

static void ResetGroupSwapCandidateRounds(GroupMigrationPolicy *policy)
{
    for (int i = 0; i < policy->groupCount; i++) {
        policy->groups[i].swapCandidateRounds = 0;
    }
}

static uint64_t CalcRemoteRemaining(const MigrationGroupAttr *group, int targetIdx)
{
    const GroupTargetAttr *target = &group->targets[targetIdx];
    if (target->usedPages >= target->quotaPages) {
        return 0;
    }
    return target->quotaPages - target->usedPages;
}

/*
 * Collect all group target entries that point to the same remote NUMA.
 * A single entry means the target is private to one group; multiple entries
 * mean this remote NUMA is shared and must be split by quota.
 */
static int CollectTargetEntriesByNid(GroupMigrationPolicy *policy, int nid, GroupTargetEntry entries[], int maxEntries)
{
    int count = 0;

    for (int i = 0; i < policy->groupCount; i++) {
        MigrationGroupAttr *group = &policy->groups[i];
        for (int j = 0; j < group->targetCount; j++) {
            if (group->targets[j].nid != nid) {
                continue;
            }
            if (count >= maxEntries) {
                SMAP_LOGGER_WARNING("grouped target entry count exceeds limit on numa %d.", nid);
                return count;
            }
            entries[count].groupIdx = i;
            entries[count].targetIdx = j;
            entries[count].oldUsedPages = group->targets[j].usedPages;
            count++;
        }
    }
    return count;
}

/* Calculate the total quota for entries sharing one remote NUMA. */
static uint64_t CalcTargetQuotaSum(GroupMigrationPolicy *policy, const GroupTargetEntry entries[], int entryCount)
{
    uint64_t quotaSum = 0;

    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[entries[i].groupIdx].targets[entries[i].targetIdx];
        if (UINT64_MAX - quotaSum < target->quotaPages) {
            return UINT64_MAX;
        }
        quotaSum += target->quotaPages;
    }
    return quotaSum;
}

/*
 * For a non-shared target, ACTC resident pages can be mapped directly to the
 * only owning group target.
 */
static void SyncSingleTargetUsedPages(ProcessAttr *process, GroupTargetEntry *entry, uint64_t residentPages)
{
    MigrationGroupAttr *group = &process->groupPolicy.groups[entry->groupIdx];
    GroupTargetAttr *target = &group->targets[entry->targetIdx];

    target->usedPages = residentPages;
    if (entry->oldUsedPages != residentPages) {
        SMAP_LOGGER_INFO("grouped pid %d group %d target %d sync used pages %llu from actc, old %llu.", process->pid,
                         entry->groupIdx, target->nid, residentPages, entry->oldUsedPages);
    }
    if (residentPages > target->quotaPages) {
        SMAP_LOGGER_WARNING("grouped pid %d group %d target %d resident pages %llu exceed quota %llu.", process->pid,
                            entry->groupIdx, target->nid, residentPages, target->quotaPages);
    }
}

/*
 * For a shared target, split ACTC resident pages by configured quota. This is
 * capacity accounting only; it does not imply page-level ownership.
 */
static void AssignSharedTargetUsedPages(ProcessAttr *process, const GroupTargetEntry entries[], int entryCount,
                                        uint64_t residentPages, uint64_t quotaSum)
{
    uint64_t assignedPages = 0;
    GroupMigrationPolicy *policy = &process->groupPolicy;

    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[entries[i].groupIdx].targets[entries[i].targetIdx];
        /* Use 128-bit multiplication to avoid overflow before division. */
        target->usedPages = (__uint128_t)residentPages * target->quotaPages / quotaSum;
        assignedPages += target->usedPages;
    }

    uint64_t remainingPages = residentPages - assignedPages;
    /* Distribute truncation remainder in configuration order. */
    for (int i = 0; i < entryCount && remainingPages > 0; i++) {
        GroupTargetAttr *target = &policy->groups[entries[i].groupIdx].targets[entries[i].targetIdx];
        target->usedPages++;
        remainingPages--;
    }
}

/* Validate shared-target quota accounting and publish the split usedPages. */
static void SyncSharedTargetUsedPages(ProcessAttr *process, const GroupTargetEntry entries[], int entryCount,
                                      uint64_t residentPages, uint64_t quotaSum)
{
    GroupMigrationPolicy *policy = &process->groupPolicy;
    int nid = policy->groups[entries[0].groupIdx].targets[entries[0].targetIdx].nid;

    if (quotaSum == 0 || quotaSum == UINT64_MAX) {
        SMAP_LOGGER_WARNING("grouped pid %d target %d invalid quota sum %llu, skip used pages sync.", process->pid, nid,
                            quotaSum);
        return;
    }
    if (residentPages > quotaSum) {
        SMAP_LOGGER_WARNING("grouped pid %d shared target %d resident pages %llu exceed quota sum %llu.", process->pid,
                            nid, residentPages, quotaSum);
    }

    AssignSharedTargetUsedPages(process, entries, entryCount, residentPages, quotaSum);
    for (int i = 0; i < entryCount; i++) {
        GroupTargetAttr *target = &policy->groups[entries[i].groupIdx].targets[entries[i].targetIdx];
        if (entries[i].oldUsedPages != target->usedPages) {
            SMAP_LOGGER_INFO("grouped pid %d group %d shared target %d sync used pages %llu from actc, old %llu.",
                             process->pid, entries[i].groupIdx, nid, target->usedPages, entries[i].oldUsedPages);
        }
    }
}

/*
 * Refresh grouped target usedPages from the current ACTC scan before planning.
 * This captures remote pages that were allocated by VM growth instead of SMAP
 * demote, so promote can migrate them back when local reserves are below
 * waterline.
 */
static void SyncGroupedTargetUsedPages(ProcessAttr *process)
{
    GroupMigrationPolicy *policy = &process->groupPolicy;

    for (int nid = 0; nid < MAX_NODES; nid++) {
        GroupTargetEntry entries[MAX_GROUP_TARGET_ENTRY] = { 0 };
        int entryCount = CollectTargetEntriesByNid(policy, nid, entries, MAX_GROUP_TARGET_ENTRY);
        if (entryCount <= 0) {
            continue;
        }

        uint64_t residentPages = process->scanAttr.actcLen[nid];
        if (entryCount == 1) {
            SyncSingleTargetUsedPages(process, &entries[0], residentPages);
            continue;
        }

        uint64_t quotaSum = CalcTargetQuotaSum(policy, entries, entryCount);
        SyncSharedTargetUsedPages(process, entries, entryCount, residentPages, quotaSum);
    }
}

static bool IsSharedTarget(const GroupMigrationPolicy *policy, int groupIdx)
{
    const MigrationGroupAttr *group = &policy->groups[groupIdx];
    for (int i = 0; i < group->targetCount; i++) {
        int nid = group->targets[i].nid;
        for (int j = 0; j < policy->groupCount; j++) {
            if (j == groupIdx) {
                continue;
            }
            if (GroupTargetIndex(&policy->groups[j], nid) >= 0) {
                return true;
            }
        }
    }
    return false;
}

static uint64_t CalcGroupSwapBaseCap(uint64_t remoteUsed)
{
    uint32_t ratio = GetGroupSwapRatioConfig();
    if (ratio == 0) {
        return 0;
    }
    return (remoteUsed * ratio + GROUP_SWAP_PERCENT_BASE - 1) / GROUP_SWAP_PERCENT_BASE;
}

static void EnsureGroupedSwapParam(ProcessAttr *process, uint64_t localUsed, uint64_t remoteUsed)
{
    if (process->separateParam.freqWt == 0) {
        process->separateParam.freqWt = GROUP_SWAP_DEFAULT_FREQ_WT;
    }
    if (process->separateParam.slowThred == 0) {
        process->separateParam.slowThred = GROUP_SWAP_DEFAULT_SLOW_THRED;
    }
    if (process->separateParam.maxMigrate == 0) {
        process->separateParam.maxMigrate = localUsed + remoteUsed;
    }
}

static bool IsSwapBeneficial(const ProcessAttr *process, const GroupPageData *local, const GroupPageData *remote)
{
    uint64_t freqWt = MAX((uint64_t)process->separateParam.freqWt, (uint64_t)GROUP_SWAP_DEFAULT_FREQ_WT);
    uint64_t slowThred = (uint64_t)process->separateParam.slowThred * freqWt * GROUP_SWAP_SLOW_THRED_FACTOR;
    uint64_t localFreq = freqWt * local->freq;
    uint64_t remoteFreq = remote->freq;

    if (remote->freq < GetGroupSwapMinRemoteFreqConfig()) {
        return false;
    }
    if (remoteFreq <= localFreq + GetGroupSwapMinFreqGainConfig()) {
        return false;
    }
    return ((localFreq == 0 && remoteFreq > 0) || (localFreq + slowThred < remoteFreq));
}

static int AppendMigPages(struct MigList *list, const GroupPageData *pages, uint64_t nr)
{
    if (nr == 0) {
        return 0;
    }
    uint64_t oldNr = list->nr;
    uint64_t newNr = oldNr + nr;
    uint64_t *addr = malloc(sizeof(uint64_t) * newNr);
    if (!addr) {
        return -ENOMEM;
    }
    if (oldNr > 0) {
        int ret = memcpy_s(addr, sizeof(uint64_t) * newNr, list->addr, sizeof(uint64_t) * oldNr);
        if (ret) {
            free(addr);
            return -ret;
        }
    }
    for (uint64_t i = 0; i < nr; i++) {
        addr[oldNr + i] = pages[i].addr;
    }
    free(list->addr);
    list->addr = addr;
    list->nr = newNr;
    return 0;
}

static int CollectLocalPages(ProcessAttr *process, const MigrationGroupAttr *group, GroupPageData **pages,
                             uint64_t *nrPages)
{
    uint64_t total = CalcLocalUsed(process, group);
    *pages = NULL;
    *nrPages = 0;
    if (total == 0) {
        return 0;
    }
    GroupPageData *data = calloc(total, sizeof(GroupPageData));
    if (!data) {
        return -ENOMEM;
    }
    uint64_t pos = 0;
    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        for (uint64_t j = 0; j < process->scanAttr.actcLen[nid]; j++) {
            data[pos].addr = process->scanAttr.actcData[nid][j].addr;
            data[pos].freq = process->scanAttr.actcData[nid][j].freq;
            data[pos].prior = process->scanAttr.actcData[nid][j].prior;
            data[pos].isWhiteListPage = process->scanAttr.actcData[nid][j].isWhiteListPage;
            data[pos].nid = nid;
            pos++;
        }
    }
    qsort(data, total, sizeof(GroupPageData), GroupPageAscFunc);
    *pages = data;
    *nrPages = total;
    return 0;
}

static int CollectRemotePages(ProcessAttr *process, const MigrationGroupAttr *group, const uint64_t remoteOffsets[],
                              GroupPageData **pages, uint64_t *nrPages)
{
    uint64_t total = 0;
    for (int i = 0; i < group->targetCount; i++) {
        int nid = group->targets[i].nid;
        if (remoteOffsets[nid] >= process->scanAttr.actcLen[nid]) {
            continue;
        }
        total += MIN(group->targets[i].usedPages, process->scanAttr.actcLen[nid] - remoteOffsets[nid]);
    }
    *pages = NULL;
    *nrPages = 0;
    if (total == 0) {
        return 0;
    }
    GroupPageData *data = calloc(total, sizeof(GroupPageData));
    if (!data) {
        return -ENOMEM;
    }
    uint64_t pos = 0;
    for (int i = 0; i < group->targetCount; i++) {
        int nid = group->targets[i].nid;
        if (remoteOffsets[nid] >= process->scanAttr.actcLen[nid]) {
            continue;
        }
        qsort(process->scanAttr.actcData[nid], process->scanAttr.actcLen[nid], sizeof(ActcData), ActcDataDescFunc);
        uint64_t limit = MIN(group->targets[i].usedPages, process->scanAttr.actcLen[nid] - remoteOffsets[nid]);
        for (uint64_t j = 0; j < limit; j++) {
            uint64_t actcIdx = remoteOffsets[nid] + j;
            data[pos].addr = process->scanAttr.actcData[nid][actcIdx].addr;
            data[pos].freq = process->scanAttr.actcData[nid][actcIdx].freq;
            data[pos].prior = process->scanAttr.actcData[nid][actcIdx].prior;
            data[pos].isWhiteListPage = process->scanAttr.actcData[nid][actcIdx].isWhiteListPage;
            data[pos].nid = nid;
            pos++;
        }
    }
    qsort(data, total, sizeof(GroupPageData), GroupPageDescFunc);
    *pages = data;
    *nrPages = total;
    return 0;
}

static int BuildDemoteMigList(ProcessAttr *process, MigrationGroupAttr *group,
                              struct MigList mlist[MAX_NODES][MAX_NODES], uint64_t demoteNeed)
{
    if (demoteNeed == 0) {
        return 0;
    }
    GroupPageData *pages = NULL;
    uint64_t nrPages = 0;
    int ret = CollectLocalPages(process, group, &pages, &nrPages);
    if (ret) {
        return ret;
    }
    if (nrPages == 0) {
        return 0;
    }
    GroupPageData *selectedPages = calloc(MIN(demoteNeed, nrPages), sizeof(GroupPageData));
    if (!selectedPages) {
        free(pages);
        return -ENOMEM;
    }
    uint64_t localSelected[MAX_NODES] = { 0 };
    uint64_t localExcess[MAX_NODES] = { 0 };
    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        localExcess[nid] = CalcLocalExcess(process, group, i);
    }
    uint64_t selected = 0;
    uint64_t selectLimit = MIN(demoteNeed, nrPages);
    for (uint64_t i = 0; i < nrPages && selected < selectLimit; i++) {
        int nid = pages[i].nid;
        if (localSelected[nid] >= localExcess[nid]) {
            continue;
        }
        selectedPages[selected++] = pages[i];
        localSelected[nid]++;
    }
    uint64_t pageOff = 0;
    uint64_t remaining = selected;
    for (int i = 0; i < group->targetCount && remaining > 0; i++) {
        int to = group->targets[i].nid;
        uint64_t targetRemaining = CalcRemoteRemaining(group, i);
        targetRemaining = MIN(targetRemaining, GetNrFreeHugePagesByNode(to));
        uint64_t targetMig = MIN(remaining, targetRemaining);
        uint64_t targetEnd = pageOff + targetMig;
        while (pageOff < targetEnd) {
            int from = selectedPages[pageOff].nid;
            uint64_t start = pageOff;
            while (pageOff < targetEnd && selectedPages[pageOff].nid == from) {
                pageOff++;
            }
            ret = AppendMigPages(&mlist[from][to], &selectedPages[start], pageOff - start);
            if (ret) {
                free(selectedPages);
                free(pages);
                return ret;
            }
        }
        remaining -= targetMig;
    }
    free(selectedPages);
    free(pages);
    return 0;
}

static int SelectPromoteLocal(ProcessAttr *process, const MigrationGroupAttr *group, uint64_t selectedPerLocal[])
{
    int selectedIdx = -1;
    uint64_t selectedDeficit = 0;
    uint64_t selectedFree = 0;

    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        uint64_t deficit = CalcLocalDeficit(process, group, i);
        if (deficit <= selectedPerLocal[i]) {
            continue;
        }
        uint64_t freePages = GetNrFreeHugePagesByNode(nid);
        if (freePages <= selectedPerLocal[i]) {
            continue;
        }
        uint64_t remainingDeficit = deficit - selectedPerLocal[i];
        uint64_t remainingFree = freePages - selectedPerLocal[i];
        if (selectedIdx < 0 || remainingDeficit > selectedDeficit ||
            (remainingDeficit == selectedDeficit && remainingFree > selectedFree)) {
            selectedIdx = i;
            selectedDeficit = remainingDeficit;
            selectedFree = remainingFree;
        }
    }
    return selectedIdx;
}

static int BuildPromoteMigList(ProcessAttr *process, MigrationGroupAttr *group, uint64_t remoteOffsets[],
                               struct MigList mlist[MAX_NODES][MAX_NODES], uint64_t promoteNeed)
{
    if (promoteNeed == 0) {
        return 0;
    }
    GroupPageData *pages = NULL;
    uint64_t nrPages = 0;
    int ret = CollectRemotePages(process, group, remoteOffsets, &pages, &nrPages);
    if (ret) {
        return ret;
    }
    uint64_t nrMig = MIN(promoteNeed, nrPages);
    uint64_t selectedPerRemote[MAX_NODES] = { 0 };
    uint64_t selectedPerLocal[MAX_GROUP_LOCAL_NUMA] = { 0 };
    uint64_t pageOff = 0;
    while (pageOff < nrMig) {
        int localIdx = SelectPromoteLocal(process, group, selectedPerLocal);
        if (localIdx < 0) {
            break;
        }
        int to = group->locals[localIdx].nid;
        int from = pages[pageOff].nid;
        ret = AppendMigPages(&mlist[from][to], &pages[pageOff], 1);
        if (ret) {
            free(pages);
            return ret;
        }
        selectedPerRemote[from]++;
        selectedPerLocal[localIdx]++;
        pageOff++;
    }
    for (int nid = 0; nid < MAX_NODES; nid++) {
        remoteOffsets[nid] += selectedPerRemote[nid];
    }
    free(pages);
    return 0;
}

static void GetPlannedPromoteToLocal(const MigrationGroupAttr *group, struct MigList mlist[MAX_NODES][MAX_NODES],
                                     uint64_t plannedIn[MAX_NODES])
{
    for (int i = 0; i < group->targetCount; i++) {
        int from = group->targets[i].nid;
        for (int j = 0; j < group->localCount; j++) {
            int to = group->locals[j].nid;
            plannedIn[to] += mlist[from][to].nr;
        }
    }
}

static int SelectRebalanceTargetLocal(ProcessAttr *process, const MigrationGroupAttr *group,
                                      const uint64_t plannedIn[MAX_NODES], const uint64_t selectedPerLocal[])
{
    int selectedIdx = -1;
    uint64_t selectedDeficit = 0;
    uint64_t selectedFree = 0;

    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        uint64_t deficit = CalcLocalDeficit(process, group, i);
        uint64_t selected = plannedIn[nid] + selectedPerLocal[i];
        if (deficit <= selected) {
            continue;
        }
        uint64_t freePages = GetNrFreeHugePagesByNode(nid);
        if (freePages <= selected) {
            continue;
        }
        uint64_t remainingDeficit = deficit - selected;
        uint64_t remainingFree = freePages - selected;
        if (selectedIdx < 0 || remainingDeficit > selectedDeficit ||
            (remainingDeficit == selectedDeficit && remainingFree > selectedFree)) {
            selectedIdx = i;
            selectedDeficit = remainingDeficit;
            selectedFree = remainingFree;
        }
    }
    return selectedIdx;
}

static void FreeRebalanceBuckets(GroupPageData *pages[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            free(pages[i][j]);
        }
    }
}

static int BuildRebalanceMigListsFromPairs(const RebalancePair *pairs, uint64_t nrPairs,
                                           struct MigList mlist[MAX_NODES][MAX_NODES])
{
    uint64_t cnt[MAX_NODES][MAX_NODES] = { 0 };
    uint64_t pos[MAX_NODES][MAX_NODES] = { 0 };
    GroupPageData *pages[MAX_NODES][MAX_NODES] = { NULL };

    for (uint64_t i = 0; i < nrPairs; i++) {
        cnt[pairs[i].page.nid][pairs[i].toNid]++;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            if (cnt[i][j] == 0) {
                continue;
            }
            pages[i][j] = calloc(cnt[i][j], sizeof(GroupPageData));
            if (!pages[i][j]) {
                FreeRebalanceBuckets(pages);
                return -ENOMEM;
            }
        }
    }
    for (uint64_t i = 0; i < nrPairs; i++) {
        int from = pairs[i].page.nid;
        int to = pairs[i].toNid;
        pages[from][to][pos[from][to]++] = pairs[i].page;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            int ret = AppendMigPages(&mlist[i][j], pages[i][j], cnt[i][j]);
            if (ret) {
                FreeRebalanceBuckets(pages);
                return ret;
            }
        }
    }
    FreeRebalanceBuckets(pages);
    return 0;
}

static int BuildLocalRebalanceMigList(ProcessAttr *process, MigrationGroupAttr *group,
                                      struct MigList mlist[MAX_NODES][MAX_NODES])
{
    uint64_t deficit = CalcGroupDeficit(process, group);
    uint64_t excess = CalcGroupExcess(process, group);
    if (deficit == 0 || excess == 0) {
        return 0;
    }

    uint64_t plannedIn[MAX_NODES] = { 0 };
    GetPlannedPromoteToLocal(group, mlist, plannedIn);
    uint64_t plannedPromote = 0;
    for (int i = 0; i < group->localCount; i++) {
        plannedPromote += plannedIn[group->locals[i].nid];
    }
    if (plannedPromote >= deficit) {
        return 0;
    }

    GroupPageData *pages = NULL;
    uint64_t nrPages = 0;
    int ret = CollectLocalPages(process, group, &pages, &nrPages);
    if (ret) {
        return ret;
    }
    uint64_t rebalanceNeed = MIN(deficit - plannedPromote, excess);
    if (rebalanceNeed == 0 || nrPages == 0) {
        free(pages);
        return 0;
    }
    RebalancePair *pairs = calloc(rebalanceNeed, sizeof(RebalancePair));
    if (!pairs) {
        free(pages);
        return -ENOMEM;
    }

    uint64_t localSelectedFrom[MAX_NODES] = { 0 };
    uint64_t selectedPerTarget[MAX_GROUP_LOCAL_NUMA] = { 0 };
    uint64_t localExcess[MAX_NODES] = { 0 };
    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        localExcess[nid] = CalcLocalExcess(process, group, i);
    }

    uint64_t built = 0;
    for (uint64_t i = 0; i < nrPages && built < rebalanceNeed; i++) {
        int from = pages[i].nid;
        if (localSelectedFrom[from] >= localExcess[from]) {
            continue;
        }
        int targetIdx = SelectRebalanceTargetLocal(process, group, plannedIn, selectedPerTarget);
        if (targetIdx < 0) {
            break;
        }
        int to = group->locals[targetIdx].nid;
        pairs[built].page = pages[i];
        pairs[built].toNid = to;
        localSelectedFrom[from]++;
        selectedPerTarget[targetIdx]++;
        built++;
    }
    if (built > 0) {
        SMAP_LOGGER_INFO("grouped pid %d local rebalance %llu pages.", process->pid, built);
        ret = BuildRebalanceMigListsFromPairs(pairs, built, mlist);
    }
    free(pairs);
    free(pages);
    return ret;
}

static void FreeSwapBuckets(GroupPageData *promotePages[MAX_NODES][MAX_NODES],
                            GroupPageData *demotePages[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            free(promotePages[i][j]);
            free(demotePages[i][j]);
        }
    }
}

static int AllocSwapBuckets(uint64_t promoteCnt[MAX_NODES][MAX_NODES], uint64_t demoteCnt[MAX_NODES][MAX_NODES],
                            GroupPageData *promotePages[MAX_NODES][MAX_NODES],
                            GroupPageData *demotePages[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            if (promoteCnt[i][j] > 0) {
                promotePages[i][j] = calloc(promoteCnt[i][j], sizeof(GroupPageData));
                if (!promotePages[i][j]) {
                    return -ENOMEM;
                }
            }
            if (demoteCnt[i][j] == 0) {
                continue;
            }
            demotePages[i][j] = calloc(demoteCnt[i][j], sizeof(GroupPageData));
            if (!demotePages[i][j]) {
                return -ENOMEM;
            }
        }
    }
    return 0;
}

static int BuildSwapMigListsFromPairs(const SwapPair *pairs, uint64_t nrPairs,
                                      struct MigList mlist[MAX_NODES][MAX_NODES])
{
    uint64_t promoteCnt[MAX_NODES][MAX_NODES] = { 0 };
    uint64_t demoteCnt[MAX_NODES][MAX_NODES] = { 0 };
    uint64_t promotePos[MAX_NODES][MAX_NODES] = { 0 };
    uint64_t demotePos[MAX_NODES][MAX_NODES] = { 0 };
    GroupPageData *promotePages[MAX_NODES][MAX_NODES] = { NULL };
    GroupPageData *demotePages[MAX_NODES][MAX_NODES] = { NULL };

    for (uint64_t i = 0; i < nrPairs; i++) {
        int remoteNid = pairs[i].remote.nid;
        int localNid = pairs[i].local.nid;
        promoteCnt[remoteNid][localNid]++;
        demoteCnt[localNid][remoteNid]++;
    }
    int ret = AllocSwapBuckets(promoteCnt, demoteCnt, promotePages, demotePages);
    if (ret) {
        FreeSwapBuckets(promotePages, demotePages);
        return ret;
    }
    for (uint64_t i = 0; i < nrPairs; i++) {
        int remoteNid = pairs[i].remote.nid;
        int localNid = pairs[i].local.nid;
        promotePages[remoteNid][localNid][promotePos[remoteNid][localNid]++] = pairs[i].remote;
        demotePages[localNid][remoteNid][demotePos[localNid][remoteNid]++] = pairs[i].local;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            ret = AppendMigPages(&mlist[i][j], promotePages[i][j], promoteCnt[i][j]);
            if (ret) {
                FreeSwapBuckets(promotePages, demotePages);
                return ret;
            }
            ret = AppendMigPages(&mlist[i][j], demotePages[i][j], demoteCnt[i][j]);
            if (ret) {
                FreeSwapBuckets(promotePages, demotePages);
                return ret;
            }
        }
    }
    FreeSwapBuckets(promotePages, demotePages);
    return 0;
}

static uint64_t SelectSwapPairs(ProcessAttr *process, const MigrationGroupAttr *group, GroupPageData *localPages,
                                uint64_t nrLocalPages, GroupPageData *remotePages, uint64_t nrRemotePages,
                                uint64_t swapCap, SwapPair *pairs)
{
    uint64_t remoteFree[MAX_NODES] = { 0 };
    uint64_t localFree[MAX_NODES] = { 0 };
    uint64_t nrPairs = 0;
    uint64_t localIdx = 0;

    for (int i = 0; i < group->localCount; i++) {
        int nid = group->locals[i].nid;
        localFree[nid] = GetNrFreeHugePagesByNode(nid);
    }
    for (int i = 0; i < group->targetCount; i++) {
        int nid = group->targets[i].nid;
        remoteFree[nid] = GetNrFreeHugePagesByNode(nid);
    }
    for (uint64_t remoteIdx = 0; remoteIdx < nrRemotePages && localIdx < nrLocalPages && nrPairs < swapCap;
         remoteIdx++) {
        int remoteNid = remotePages[remoteIdx].nid;
        while (localIdx < nrLocalPages && localFree[localPages[localIdx].nid] == 0) {
            localIdx++;
        }
        if (localIdx >= nrLocalPages) {
            break;
        }
        if (!IsSwapBeneficial(process, &localPages[localIdx], &remotePages[remoteIdx])) {
            break;
        }
        int localNid = localPages[localIdx].nid;
        if (remoteFree[remoteNid] == 0) {
            continue;
        }
        pairs[nrPairs].local = localPages[localIdx];
        pairs[nrPairs].remote = remotePages[remoteIdx];
        localIdx++;
        nrPairs++;
        localFree[localNid]--;
        remoteFree[remoteNid]--;
    }
    return nrPairs;
}

static int BuildSwapMigList(ProcessAttr *process, MigrationGroupAttr *group, struct MigList mlist[MAX_NODES][MAX_NODES],
                            uint64_t localUsed, uint64_t remoteUsed, uint64_t *nrBuilt)
{
    uint64_t remoteOffsets[MAX_NODES] = { 0 };
    GroupPageData *localPages = NULL;
    GroupPageData *remotePages = NULL;
    SwapPair *pairs = NULL;
    uint64_t nrLocalPages = 0;
    uint64_t nrRemotePages = 0;
    *nrBuilt = 0;
    int ret = CollectLocalPages(process, group, &localPages, &nrLocalPages);
    if (ret) {
        return ret;
    }
    ret = CollectRemotePages(process, group, remoteOffsets, &remotePages, &nrRemotePages);
    if (ret) {
        free(localPages);
        return ret;
    }
    uint64_t swapCap = CalcGroupSwapBaseCap(remoteUsed);
    swapCap = MIN(swapCap, process->separateParam.maxMigrate);
    swapCap = MIN(swapCap, nrLocalPages);
    swapCap = MIN(swapCap, nrRemotePages);
    if (swapCap == 0) {
        free(localPages);
        free(remotePages);
        return 0;
    }
    pairs = calloc(swapCap, sizeof(SwapPair));
    if (!pairs) {
        free(localPages);
        free(remotePages);
        return -ENOMEM;
    }
    uint64_t nrPairs =
        SelectSwapPairs(process, group, localPages, nrLocalPages, remotePages, nrRemotePages, swapCap, pairs);
    if (nrPairs > 0) {
        SMAP_LOGGER_INFO("grouped pid %d swap %llu pages, localUsed %llu remoteUsed %llu.", process->pid, nrPairs,
                         localUsed, remoteUsed);
        ret = BuildSwapMigListsFromPairs(pairs, nrPairs, mlist);
        if (!ret) {
            *nrBuilt = nrPairs;
        }
    }
    free(pairs);
    free(localPages);
    free(remotePages);
    return ret;
}

static int BuildPromoteGroups(ProcessAttr *process, PromoteGroup *groups, int *groupCnt)
{
    *groupCnt = 0;
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        uint64_t remoteUsed = CalcRemoteUsed(group);
        uint64_t deficit = CalcGroupDeficit(process, group);
        if (deficit == 0 || remoteUsed == 0) {
            continue;
        }
        groups[*groupCnt].groupIdx = i;
        groups[*groupCnt].remoteUsed = remoteUsed;
        groups[*groupCnt].deficit = deficit;
        (*groupCnt)++;
    }
    qsort(groups, *groupCnt, sizeof(PromoteGroup), PromoteGroupCmp);
    return 0;
}

static int RunPromoteStage(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    PromoteGroup promoteGroups[MAX_MIGRATION_GROUP_NUM] = { 0 };
    uint64_t remoteOffsets[MAX_NODES] = { 0 };
    int groupCnt = 0;
    int ret = BuildPromoteGroups(process, promoteGroups, &groupCnt);
    if (ret) {
        return ret;
    }
    for (int i = 0; i < groupCnt; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[promoteGroups[i].groupIdx];
        uint64_t promoteNeed = MIN(promoteGroups[i].deficit, promoteGroups[i].remoteUsed);
        SMAP_LOGGER_INFO("grouped pid %d group %d warm-up promote %llu pages.", process->pid, promoteGroups[i].groupIdx,
                         promoteNeed);
        ret = BuildPromoteMigList(process, group, remoteOffsets, mlist, promoteNeed);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static int RunDemoteStage(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        uint64_t localUsed = CalcLocalUsed(process, group);
        uint64_t remoteUsed = CalcRemoteUsed(group);
        uint64_t deficit = CalcGroupDeficit(process, group);
        uint64_t excess = CalcGroupExcess(process, group);
        SMAP_LOGGER_INFO("grouped pid %d group %d localUsed %llu deficit %llu excess %llu remoteUsed %llu.",
                         process->pid, i, localUsed, deficit, excess, remoteUsed);
        if (deficit > 0 || excess == 0) {
            SMAP_LOGGER_INFO("grouped pid %d group %d skip demote.", process->pid, i);
            continue;
        }
        uint64_t demoteAllowed = 0;
        for (int j = 0; j < group->targetCount; j++) {
            demoteAllowed += CalcRemoteRemaining(group, j);
        }
        uint64_t actualDemote = MIN(excess, demoteAllowed);
        SMAP_LOGGER_INFO("grouped pid %d group %d cool-down demote %llu pages, allowed %llu.", process->pid, i,
                         actualDemote, demoteAllowed);
        int ret = BuildDemoteMigList(process, group, mlist, actualDemote);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static int RunLocalRebalanceStage(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        uint64_t deficit = CalcGroupDeficit(process, group);
        uint64_t excess = CalcGroupExcess(process, group);
        if (deficit == 0 || excess == 0) {
            continue;
        }
        SMAP_LOGGER_INFO("grouped pid %d group %d local rebalance candidate deficit %llu excess %llu.", process->pid, i,
                         deficit, excess);
        int ret = BuildLocalRebalanceMigList(process, group, mlist);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static int RunSwapStage(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    if (!process->enableSwap) {
        return 0;
    }
    if (!UpdateGroupSwapTotalPagesStable(process)) {
        SMAP_LOGGER_INFO("grouped pid %d swap waits for stable total pages, current %llu round %u.", process->pid,
                         process->groupSwapLastTotalPages, process->groupSwapStableTotalRounds);
        ResetGroupSwapCandidateRounds(&process->groupPolicy);
        return 0;
    }
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        uint64_t localUsed = CalcLocalUsed(process, group);
        uint64_t remoteUsed = CalcRemoteUsed(group);
        if (!IsGroupLocalSteady(process, group) || remoteUsed == 0 || IsSharedTarget(&process->groupPolicy, i)) {
            group->swapCandidateRounds = 0;
            continue;
        }
        if (group->swapCandidateRounds < GROUP_SWAP_READY_ROUNDS) {
            group->swapCandidateRounds++;
        }
        if (group->swapCandidateRounds < GROUP_SWAP_READY_ROUNDS) {
            SMAP_LOGGER_INFO("grouped pid %d group %d swap candidate round %u.", process->pid, i,
                             group->swapCandidateRounds);
            continue;
        }
        EnsureGroupedSwapParam(process, localUsed, remoteUsed);
        uint64_t nrBuilt = 0;
        int ret = BuildSwapMigList(process, group, mlist, localUsed, remoteUsed, &nrBuilt);
        if (ret) {
            return ret;
        }
        if (nrBuilt == 0) {
            group->swapCandidateRounds = 0;
        }
    }
    return 0;
}

int GroupedMigrationStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    if (!process || !process->groupPolicy.enabled) {
        return -EINVAL;
    }
    /* Swap depends on target usedPages accounting. */
    SyncGroupedTargetUsedPages(process);
    WarnUngroupedPages(process);
    int ret = RunSwapStage(process, mlist);
    if (ret) {
        SMAP_LOGGER_ERROR("grouped pid %d swap stage failed: %d.", process->pid, ret);
    }
    return ret;
}

void UpdateGroupedMigrationResult(ProcessAttr *process, int fromNid, int toNid, uint64_t successPages)
{
    if (!process || !process->groupPolicy.enabled || successPages == 0) {
        return;
    }
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        if (GroupHasLocal(group, fromNid)) {
            if (GroupHasLocal(group, toNid)) {
                SMAP_LOGGER_INFO("grouped pid %d group %d local rebalance from %d to %d success pages %llu.",
                                 process->pid, i, fromNid, toNid, successPages);
                return;
            }
            int targetIdx = GroupTargetIndex(group, toNid);
            if (targetIdx >= 0) {
                group->targets[targetIdx].usedPages += successPages;
                SMAP_LOGGER_INFO("grouped pid %d group %d target %d used pages %llu.", process->pid, i, toNid,
                                 group->targets[targetIdx].usedPages);
                return;
            }
        }
        if (GroupHasLocal(group, toNid)) {
            int targetIdx = GroupTargetIndex(group, fromNid);
            if (targetIdx >= 0) {
                if (successPages > group->targets[targetIdx].usedPages) {
                    SMAP_LOGGER_WARNING("grouped pid %d group %d target %d used pages underflow.", process->pid, i,
                                        fromNid);
                    group->targets[targetIdx].usedPages = 0;
                } else {
                    group->targets[targetIdx].usedPages -= successPages;
                }
                SMAP_LOGGER_INFO("grouped pid %d group %d target %d used pages %llu.", process->pid, i, fromNid,
                                 group->targets[targetIdx].usedPages);
                return;
            }
        }
    }
    SMAP_LOGGER_WARNING("grouped pid %d cannot match migration result from %d to %d.", process->pid, fromNid, toNid);
}
