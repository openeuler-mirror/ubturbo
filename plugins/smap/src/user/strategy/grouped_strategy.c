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

typedef struct {
    uint64_t addr;
    actc_t freq;
    uint8_t prior;
    bool isWhiteListPage;
    int nid;
} GroupPageData;

typedef struct {
    int groupIdx;
    uint64_t localUsed;
    uint64_t remoteUsed;
    uint64_t deficit;
    double deficitRatio;
} PromoteGroup;

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
    if (g1->deficitRatio < g2->deficitRatio) {
        return 1;
    }
    if (g1->deficitRatio > g2->deficitRatio) {
        return -1;
    }
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
        if (group->localNids[i] == nid) {
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
        used += process->scanAttr.actcLen[group->localNids[i]];
    }
    return used;
}

static uint64_t CalcRemoteUsed(const MigrationGroupAttr *group)
{
    uint64_t used = 0;
    for (int i = 0; i < group->targetCount; i++) {
        used += group->targets[i].usedPages;
    }
    return used;
}

static uint64_t CalcRemoteRemaining(const MigrationGroupAttr *group, int targetIdx)
{
    const GroupTargetAttr *target = &group->targets[targetIdx];
    if (target->usedPages >= target->quotaPages) {
        return 0;
    }
    return target->quotaPages - target->usedPages;
}

static int AppendMigPages(struct MigList *list, const GroupPageData *pages, uint64_t nr)
{
    if (nr == 0) {
        return 0;
    }
    uint64_t oldNr = list->nr;
    uint64_t newNr = oldNr + nr;
    uint64_t *addr = realloc(list->addr, sizeof(uint64_t) * newNr);
    if (!addr) {
        return -ENOMEM;
    }
    list->addr = addr;
    for (uint64_t i = 0; i < nr; i++) {
        list->addr[oldNr + i] = pages[i].addr;
    }
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
        int nid = group->localNids[i];
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

static int SelectLocalWithMostFreePages(const MigrationGroupAttr *group)
{
    int selected = group->localNids[0];
    uint64_t maxFree = 0;
    for (int i = 0; i < group->localCount; i++) {
        int nid = group->localNids[i];
        uint64_t freePages = GetNrFreeHugePagesByNode(nid);
        if (i == 0 || freePages > maxFree) {
            maxFree = freePages;
            selected = nid;
        }
    }
    return selected;
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
    uint64_t pageOff = 0;
    uint64_t remaining = MIN(demoteNeed, nrPages);
    for (int i = 0; i < group->targetCount && remaining > 0; i++) {
        int to = group->targets[i].nid;
        uint64_t targetRemaining = CalcRemoteRemaining(group, i);
        targetRemaining = MIN(targetRemaining, GetNrFreeHugePagesByNode(to));
        uint64_t targetMig = MIN(remaining, targetRemaining);
        uint64_t targetEnd = pageOff + targetMig;
        while (pageOff < targetEnd) {
            int from = pages[pageOff].nid;
            uint64_t start = pageOff;
            while (pageOff < targetEnd && pages[pageOff].nid == from) {
                pageOff++;
            }
            ret = AppendMigPages(&mlist[from][to], &pages[start], pageOff - start);
            if (ret) {
                free(pages);
                return ret;
            }
        }
        remaining -= targetMig;
    }
    free(pages);
    return 0;
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
    int to = SelectLocalWithMostFreePages(group);
    uint64_t nrMig = MIN(promoteNeed, nrPages);
    uint64_t selectedPerNode[MAX_NODES] = { 0 };
    uint64_t pageOff = 0;
    while (pageOff < nrMig) {
        int from = pages[pageOff].nid;
        uint64_t start = pageOff;
        while (pageOff < nrMig && pages[pageOff].nid == from) {
            pageOff++;
        }
        ret = AppendMigPages(&mlist[from][to], &pages[start], pageOff - start);
        if (ret) {
            free(pages);
            return ret;
        }
        selectedPerNode[from] += pageOff - start;
    }
    for (int nid = 0; nid < MAX_NODES; nid++) {
        remoteOffsets[nid] += selectedPerNode[nid];
    }
    free(pages);
    return 0;
}

static int BuildPromoteGroups(ProcessAttr *process, PromoteGroup *groups, int *groupCnt)
{
    *groupCnt = 0;
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &process->groupPolicy.groups[i];
        uint64_t localUsed = CalcLocalUsed(process, group);
        uint64_t remoteUsed = CalcRemoteUsed(group);
        if (localUsed >= group->localLimitPages || remoteUsed == 0) {
            continue;
        }
        groups[*groupCnt].groupIdx = i;
        groups[*groupCnt].localUsed = localUsed;
        groups[*groupCnt].remoteUsed = remoteUsed;
        groups[*groupCnt].deficit = group->localLimitPages - localUsed;
        groups[*groupCnt].deficitRatio = (double)groups[*groupCnt].deficit / group->localLimitPages;
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
        SMAP_LOGGER_INFO("grouped pid %d group %d warm-up promote %llu pages.",
                         process->pid, promoteGroups[i].groupIdx, promoteNeed);
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
        SMAP_LOGGER_INFO("grouped pid %d group %d localUsed %llu localLimit %llu remoteUsed %llu.",
                         process->pid, i, localUsed, group->localLimitPages, remoteUsed);
        if (localUsed <= group->localLimitPages) {
            if (localUsed == group->localLimitPages) {
                SMAP_LOGGER_INFO("grouped pid %d group %d idle.", process->pid, i);
            }
            continue;
        }
        uint64_t demoteNeed = localUsed - group->localLimitPages;
        uint64_t demoteAllowed = 0;
        for (int j = 0; j < group->targetCount; j++) {
            demoteAllowed += CalcRemoteRemaining(group, j);
        }
        uint64_t actualDemote = MIN(demoteNeed, demoteAllowed);
        SMAP_LOGGER_INFO("grouped pid %d group %d cool-down demote %llu pages, allowed %llu.",
                         process->pid, i, actualDemote, demoteAllowed);
        int ret = BuildDemoteMigList(process, group, mlist, actualDemote);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

int GroupedMigrationStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    if (!process || !process->groupPolicy.enabled) {
        return -EINVAL;
    }
    WarnUngroupedPages(process);
    int ret = RunPromoteStage(process, mlist);
    if (ret) {
        SMAP_LOGGER_ERROR("grouped pid %d promote stage failed: %d.", process->pid, ret);
        return ret;
    }
    ret = RunDemoteStage(process, mlist);
    if (ret) {
        SMAP_LOGGER_ERROR("grouped pid %d demote stage failed: %d.", process->pid, ret);
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
            int targetIdx = GroupTargetIndex(group, toNid);
            if (targetIdx >= 0) {
                group->targets[targetIdx].usedPages += successPages;
                SMAP_LOGGER_INFO("grouped pid %d group %d target %d used pages %llu.",
                                 process->pid, i, toNid, group->targets[targetIdx].usedPages);
                return;
            }
        }
        if (GroupHasLocal(group, toNid)) {
            int targetIdx = GroupTargetIndex(group, fromNid);
            if (targetIdx >= 0) {
                if (successPages > group->targets[targetIdx].usedPages) {
                    SMAP_LOGGER_WARNING("grouped pid %d group %d target %d used pages underflow.",
                                        process->pid, i, fromNid);
                    group->targets[targetIdx].usedPages = 0;
                } else {
                    group->targets[targetIdx].usedPages -= successPages;
                }
                SMAP_LOGGER_INFO("grouped pid %d group %d target %d used pages %llu.",
                                 process->pid, i, fromNid, group->targets[targetIdx].usedPages);
                return;
            }
        }
    }
    SMAP_LOGGER_WARNING("grouped pid %d cannot match migration result from %d to %d.",
                        process->pid, fromNid, toNid);
}
