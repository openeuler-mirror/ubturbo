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

void SetGroupedProcessConfig(ProcessAttr *attr, pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy)
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

int ProcessAddGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy)
{
    int ret;
    int pidType = DetectPidType(pid);
    if (pidType < 0) {
        SMAP_LOGGER_ERROR("grouped pid %d check failed: %d.", pid, pidType);
        return pidType;
    }
    if (pidType != VM_TYPE) {
        SMAP_LOGGER_ERROR("grouped migrate out only supports VM, pid %d type %d.", pid, pidType);
        return -EINVAL;
    }
    if (!policy || !policy->enabled) {
        SMAP_LOGGER_ERROR("grouped policy of pid %d is invalid.", pid);
        return -EINVAL;
    }

    ProcessAttr *current = GetProcessAttr(pid);
    if (current) {
        SetGroupedProcessConfig(current, pid, nodeBitmap, policy);
        SMAP_LOGGER_INFO("Update grouped pid %d success, group count %d.", pid, policy->groupCount);
        ret = SyncAllProcessConfig();
        if (ret) {
            SMAP_LOGGER_WARNING("Synchronize grouped pid %d config maybe failed: %d.", pid, ret);
        }
        PutProcessAttr(current);
        return 0;
    }

    if (GetProcessManager()->nr[VM_TYPE] + GetProcessManager()->nr[PROCESS_TYPE] >= GetCurrentMaxNrPid()) {
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
    PidSlotAdd(GetProcessManager(), attr);
    GetProcessManager()->nr[VM_TYPE]++;
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

    ProcessAttr *current = GetProcessAttr(pid);
    if (!current || !current->groupPolicy.enabled || current->state != PROC_MIGRATE) {
        SMAP_LOGGER_ERROR("pid %d cannot save pending grouped policy.", pid);
        PutProcessAttr(current);
        return -EINVAL;
    }

    /* Only an already-managed grouped PID in PROC_MIGRATE can defer refresh. */
    current->pendingGroupPolicy.valid = true;
    current->pendingGroupPolicy.nodeBitmap = nodeBitmap;
    current->pendingGroupPolicy.policy = *policy;
    SMAP_LOGGER_INFO("Save pending grouped policy for pid %d, group count %d.", pid, policy->groupCount);
    PutProcessAttr(current);
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
        .pidType = attr->type,
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
