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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/time.h>
#include <math.h>
#include "manage/access_ioctl.h"
#include "smap_user_log.h"
#include "advanced-strategy/scene.h"
#include "manage/manage.h"
#include "manage/device.h"
#include "manage/thread.h"
#include "manage/access_ioctl.h"
#include "manage/smap_ioctl.h"
#include "manage/device.h"
#include "smap_inner_interface.h"
#include "strategy.h"
#include "grouped_strategy.h"
#include "strategy_config.h"
#include "securec.h"
#include "smap_env.h"
#include "migration.h"

#define MAX_MIG_ADDR_PRINT_LEN 2
#define MAX_GROUP_SWAP_COMP_PLANS (MAX_2M_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT)
#define PAIR_PLAN_LOCAL_FREE_RESERVE_DIVISOR 20

typedef struct {
    pid_t pid;
    int localNid;
    int remoteNid;
    uint64_t localToRemoteSuccess;
    uint64_t remoteToLocalSuccess;
    bool hasLocalToRemote;
    bool hasRemoteToLocal;
} GroupSwapPairStat;

typedef struct {
    pid_t pid;
    int from;
    int to;
    uint64_t need;
    uint64_t built;
    bool shouldFreeze;
} GroupSwapCompPlan;

int AddMigList(struct MigrateMsg *mMsg, struct MigList *mList)
{
    int i;
    ssize_t j;
    if (!mMsg || !mList) {
        return 0;
    }
    if (mList->nr == 0 || mList->addr == NULL) {
        return 0;
    }
    int maxMigListCount = GetCurrentMaxNrPid() * MAX_PER_PID_MIG_LIST_COUNT;
    if (mMsg->cnt >= maxMigListCount) {
        SMAP_LOGGER_WARNING("Migration list reaches kernel limit %d.", maxMigListCount);
        return -ENOSPC;
    }
    SMAP_LOGGER_DEBUG("mList->nr: %lu.", mList->nr);
    mMsg->migList[mMsg->cnt].addr = malloc(sizeof(uint64_t) * mList->nr);
    if (!mMsg->migList[mMsg->cnt].addr) {
        SMAP_LOGGER_ERROR("migList->addr malloc failed.");
        return -ENOMEM;
    }
    mMsg->migList[mMsg->cnt].from = mList->from;
    mMsg->migList[mMsg->cnt].to = mList->to;
    mMsg->migList[mMsg->cnt].pid = mList->pid;
    mMsg->migList[mMsg->cnt].nr = mList->nr;
    for (i = 0; i < mList->nr; i++) {
        mMsg->migList[mMsg->cnt].addr[i] = mList->addr[i];
    }
    mMsg->cnt++;
    return 0;
}

int CollectNodeFreeSnapshot(bool hugePage, int nrLocalNuma, PairPlanContext *context)
{
    if (!context || nrLocalNuma <= 0 || nrLocalNuma > LOCAL_NUMA_NUM || nrLocalNuma + REMOTE_NUMA_NUM > MAX_NODES) {
        return -EINVAL;
    }

    PairPlanContext snapshot = {
        .nrLocalNuma = nrLocalNuma,
    };
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        uint64_t freePages = hugePage ? GetNrFreeHugePagesByNode(nid) : GetNrFreePagesByNode(nid);
        snapshot.freePages[nid] = freePages;
        snapshot.safetyReservePages[nid] =
            freePages / PAIR_PLAN_LOCAL_FREE_RESERVE_DIVISOR + (freePages % PAIR_PLAN_LOCAL_FREE_RESERVE_DIVISOR != 0);
    }
    *context = snapshot;
    return 0;
}

static int ComparePairPlan(const void *left, const void *right)
{
    const PairPlan *lhs = left;
    const PairPlan *rhs = right;

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

static int FindPairPidBudget(const PairPidBudget pidBudgets[], size_t pidBudgetCnt, pid_t pid)
{
    int found = -1;
    for (size_t i = 0; i < pidBudgetCnt; i++) {
        if (pidBudgets[i].pid != pid) {
            continue;
        }
        if (found >= 0) {
            return -EINVAL;
        }
        found = (int)i;
    }
    return found >= 0 ? found : -ENOENT;
}

static bool IsPairPlanDirectionValid(const PairPlan *plan)
{
    if (plan->demotePages > 0 && plan->promotePages > 0) {
        return false;
    }
    if (plan->targetPages > plan->actualPages) {
        return plan->promotePages == 0 && plan->demotePages <= plan->targetPages - plan->actualPages;
    }
    if (plan->actualPages > plan->targetPages) {
        return plan->demotePages == 0 && plan->promotePages <= plan->actualPages - plan->targetPages;
    }
    return plan->demotePages == 0 && plan->promotePages == 0;
}

static ProcessAttr *FindPairPlanProcessLocked(struct ProcessManager *manager, pid_t pid)
{
    for (ProcessAttr *process = manager->processes; process; process = process->next) {
        if (process->pid == pid) {
            return process;
        }
    }
    return NULL;
}

static void ConsumeBucketPages(uint64_t buckets[FREQ_BUCKETS_SIZE], uint64_t pages, bool highestFirst)
{
    for (int step = 0; step < FREQ_BUCKETS_SIZE && pages > 0; step++) {
        int bucket = highestFirst ? FREQ_BUCKETS_SIZE - 1 - step : step;
        uint64_t consumed = MIN(buckets[bucket], pages);
        buckets[bucket] -= consumed;
        pages -= consumed;
    }
}

static uint64_t CountPairSwapPages(const ProcessAttr *process, int localNid, int remoteNid, uint64_t localSelected,
                                   uint64_t remoteSelected)
{
    if (!process->scanAttr.actcData[localNid] || !process->scanAttr.actcData[remoteNid] ||
        process->scanAttr.actcLen[localNid] == 0 || process->scanAttr.actcLen[remoteNid] == 0) {
        return 0;
    }

    uint64_t localBuckets[FREQ_BUCKETS_SIZE] = { 0 };
    uint64_t remoteBuckets[FREQ_BUCKETS_SIZE] = { 0 };
    for (int bucket = 0; bucket < FREQ_BUCKETS_SIZE; bucket++) {
        localBuckets[bucket] = process->scanAttr.actCount[localNid].freqBuckets[bucket];
        remoteBuckets[bucket] = process->scanAttr.actCount[remoteNid].freqBuckets[bucket];
    }
    /* Net migration selects local cold pages and remote hot pages first. */
    ConsumeBucketPages(localBuckets, localSelected, false);
    ConsumeBucketPages(remoteBuckets, remoteSelected, true);

    uint64_t swappable = 0;
    int localBucket = 0;
    int remoteBucket = FREQ_BUCKETS_SIZE - 1;
    while (localBucket < remoteBucket) {
        while (localBucket < remoteBucket && localBuckets[localBucket] == 0) {
            localBucket++;
        }
        while (localBucket < remoteBucket && remoteBuckets[remoteBucket] == 0) {
            remoteBucket--;
        }
        if (localBucket >= remoteBucket) {
            break;
        }
        uint64_t paired = MIN(localBuckets[localBucket], remoteBuckets[remoteBucket]);
        swappable += paired;
        localBuckets[localBucket] -= paired;
        remoteBuckets[remoteBucket] -= paired;
    }
    return swappable;
}

static uint64_t CalcPairRemoteGuaranteeExtraSwap(const ProcessAttr *process, const PairPlan *plan,
                                                 uint64_t localSelected, uint64_t remoteSelected,
                                                 uint64_t baseSwapPages, uint64_t remainingSwapBudget)
{
    if (!process->scanAttr.actcData[plan->localNid] || !process->scanAttr.actcData[plan->remoteNid] ||
        process->scanAttr.actcLen[plan->localNid] == 0 || process->scanAttr.actcLen[plan->remoteNid] == 0) {
        return 0;
    }

    uint64_t localAfterBase = localSelected + baseSwapPages;
    uint64_t remoteAfterBase = remoteSelected + baseSwapPages;
    uint64_t remoteGuarantee = process->scanAttr.actCount[plan->remoteNid].remoteHotNum;
    if (remoteAfterBase >= remoteGuarantee) {
        return 0;
    }

    uint64_t localRemaining = process->scanAttr.actcLen[plan->localNid] > localAfterBase ?
                                  process->scanAttr.actcLen[plan->localNid] - localAfterBase :
                                  0;
    uint64_t remoteRemaining = process->scanAttr.actcLen[plan->remoteNid] > remoteAfterBase ?
                                   process->scanAttr.actcLen[plan->remoteNid] - remoteAfterBase :
                                   0;
    uint64_t missingHotPages = remoteGuarantee - remoteAfterBase;
    return MIN(MIN(missingHotPages, localRemaining), MIN(remoteRemaining, remainingSwapBudget));
}

static bool IsOnlyLocalForRemote(const PairPlan plans[], size_t planCnt, size_t current)
{
    const PairPlan *plan = &plans[current];
    int activeLocalCnt = 0;
    for (size_t i = 0; i < planCnt; i++) {
        if (plans[i].pid != plan->pid || plans[i].remoteNid != plan->remoteNid ||
            (plans[i].targetPages == 0 && plans[i].actualPages == 0 && plans[i].demotePages == 0 &&
             plans[i].promotePages == 0)) {
            continue;
        }
        activeLocalCnt++;
    }
    return activeLocalCnt == 1;
}

int BuildPairSwapPlans(struct ProcessManager *manager, PairPlan plans[], size_t planCnt, PairPlanContext *context,
                       PairPidBudget pidBudgets[], size_t pidBudgetCnt)
{
    if (!manager || !context || planCnt > MAX_PAIR_TARGET_COUNT || pidBudgetCnt > MAX_4K_PROCESSES_CNT ||
        (planCnt > 0 && (!plans || !pidBudgets || pidBudgetCnt == 0))) {
        return -EINVAL;
    }

    EnvMutexLock(&manager->lock);
    for (size_t i = 0; i < planCnt; i++) {
        int budgetIndex = FindPairPidBudget(pidBudgets, pidBudgetCnt, plans[i].pid);
        ProcessAttr *process = FindPairPlanProcessLocked(manager, plans[i].pid);
        if (budgetIndex < 0 || !process || process->groupPolicy.enabled || plans[i].localNid < 0 ||
            plans[i].localNid >= context->nrLocalNuma || plans[i].remoteNid < context->nrLocalNuma ||
            plans[i].remoteNid >= MAX_NODES || !IsPairPlanDirectionValid(&plans[i]) ||
            pidBudgets[budgetIndex].plannedPages > pidBudgets[budgetIndex].maxMigratePages) {
            EnvMutexUnlock(&manager->lock);
            return -EINVAL;
        }
    }

    pid_t currentPid = -1;
    uint64_t selectedFrom[MAX_NODES] = { 0 };
    for (size_t i = 0; i < planCnt; i++) {
        PairPlan *plan = &plans[i];
        ProcessAttr *process = FindPairPlanProcessLocked(manager, plan->pid);
        int budgetIndex = FindPairPidBudget(pidBudgets, pidBudgetCnt, plan->pid);
        PairPidBudget *budget = &pidBudgets[budgetIndex];
        plan->swapPages = 0;

        if (currentPid != plan->pid) {
            currentPid = plan->pid;
            int ret = memset_s(selectedFrom, sizeof(selectedFrom), 0, sizeof(selectedFrom));
            if (ret != EOK) {
                EnvMutexUnlock(&manager->lock);
                return ret;
            }
            for (size_t j = 0; j < planCnt; j++) {
                if (plans[j].pid == currentPid) {
                    selectedFrom[plans[j].localNid] += plans[j].demotePages;
                    selectedFrom[plans[j].remoteNid] += plans[j].promotePages;
                }
            }
        }
        if (!process->enableSwap || IsNodeForbidden(plan->remoteNid) || !IsOnlyLocalForRemote(plans, planCnt, i) ||
            (plan->targetPages == 0 && plan->demotePages == 0 && plan->promotePages == 0)) {
            continue;
        }

        uint64_t freePages = context->freePages[plan->localNid];
        uint64_t reservePages = context->safetyReservePages[plan->localNid];
        uint64_t safeCapacity = freePages > reservePages ? freePages - reservePages : 0;
        uint64_t localFree = safeCapacity > context->plannedPages[plan->localNid] ?
                                 safeCapacity - context->plannedPages[plan->localNid] :
                                 0;
        uint64_t pidRemaining = budget->maxMigratePages - budget->plannedPages;
        uint64_t swapLimit = MIN(MIN(localFree, pidRemaining / 2), UINT32_MAX);
        uint64_t swapPages = CountPairSwapPages(process, plan->localNid, plan->remoteNid, selectedFrom[plan->localNid],
                                                selectedFrom[plan->remoteNid]);
        swapPages = MIN(swapPages, swapLimit);
        uint64_t extraSwapPages = CalcPairRemoteGuaranteeExtraSwap(process, plan, selectedFrom[plan->localNid],
                                                                   selectedFrom[plan->remoteNid], swapPages,
                                                                   swapLimit - swapPages);
        swapPages += extraSwapPages;
        if (extraSwapPages > 0) {
            SMAP_LOGGER_DEBUG("Pid %d pair %d-%d adds %llu swap pages for remote hot guarantee.", process->pid,
                              plan->localNid, plan->remoteNid, extraSwapPages);
        }
        plan->swapPages = (uint32_t)swapPages;
        selectedFrom[plan->localNid] += swapPages;
        selectedFrom[plan->remoteNid] += swapPages;
        context->plannedPages[plan->localNid] += swapPages;
        budget->plannedPages += swapPages * 2;
    }
    EnvMutexUnlock(&manager->lock);
    return 0;
}

int BuildPairPlans(const PairPlan inputs[], size_t inputCnt, PairPlanContext *context, PairPidBudget pidBudgets[],
                   size_t pidBudgetCnt, PairPlan plans[], size_t planCap, size_t *planCnt)
{
    if (!context || !planCnt || inputCnt > planCap || inputCnt > MAX_PAIR_TARGET_COUNT ||
        pidBudgetCnt > MAX_4K_PROCESSES_CNT || context->nrLocalNuma <= 0 || context->nrLocalNuma > LOCAL_NUMA_NUM ||
        context->nrLocalNuma + REMOTE_NUMA_NUM > MAX_NODES ||
        (inputCnt > 0 && (!inputs || !plans || !pidBudgets || pidBudgetCnt == 0))) {
        return -EINVAL;
    }
    *planCnt = 0;
    if (inputCnt == 0) {
        return 0;
    }

    PairPlan *stagedPlans = malloc(inputCnt * sizeof(PairPlan));
    PairPidBudget *stagedPidBudgets = malloc(pidBudgetCnt * sizeof(PairPidBudget));
    if (!stagedPlans || !stagedPidBudgets) {
        free(stagedPlans);
        free(stagedPidBudgets);
        return -ENOMEM;
    }

    int ret = memcpy_s(stagedPlans, inputCnt * sizeof(PairPlan), inputs, inputCnt * sizeof(PairPlan));
    if (ret != EOK) {
        free(stagedPlans);
        free(stagedPidBudgets);
        return -ret;
    }
    ret = memcpy_s(stagedPidBudgets, pidBudgetCnt * sizeof(PairPidBudget), pidBudgets,
                   pidBudgetCnt * sizeof(PairPidBudget));
    if (ret != EOK) {
        free(stagedPlans);
        free(stagedPidBudgets);
        return -ret;
    }
    PairPlanContext stagedContext = *context;

    for (size_t i = 0; i < pidBudgetCnt; i++) {
        if (stagedPidBudgets[i].plannedPages > stagedPidBudgets[i].maxMigratePages) {
            ret = -EINVAL;
            goto OUT;
        }
        for (size_t j = i + 1; j < pidBudgetCnt; j++) {
            if (stagedPidBudgets[i].pid == stagedPidBudgets[j].pid) {
                ret = -EINVAL;
                goto OUT;
            }
        }
    }

    qsort(stagedPlans, inputCnt, sizeof(PairPlan), ComparePairPlan);
    for (size_t i = 0; i < inputCnt; i++) {
        PairPlan *plan = &stagedPlans[i];
        if (plan->localNid < 0 || plan->localNid >= context->nrLocalNuma || plan->remoteIndex < 0 ||
            plan->remoteIndex >= REMOTE_NUMA_NUM || plan->remoteNid != context->nrLocalNuma + plan->remoteIndex ||
            plan->remoteNid >= MAX_NODES || (i > 0 && ComparePairPlan(&stagedPlans[i - 1], plan) == 0)) {
            ret = -EINVAL;
            goto OUT;
        }

        int pidBudgetIndex = FindPairPidBudget(stagedPidBudgets, pidBudgetCnt, plan->pid);
        if (pidBudgetIndex < 0) {
            ret = pidBudgetIndex;
            goto OUT;
        }

        plan->demotePages = 0;
        plan->promotePages = 0;
        plan->swapPages = 0;
        uint64_t requestedPages;
        bool demote = plan->actualPages < plan->targetPages;
        if (demote) {
            requestedPages = plan->targetPages - plan->actualPages;
        } else if (plan->actualPages > plan->targetPages) {
            requestedPages = plan->actualPages - plan->targetPages;
        } else {
            continue;
        }

        PairPidBudget *pidBudget = &stagedPidBudgets[pidBudgetIndex];
        uint64_t pidRemaining = pidBudget->maxMigratePages - pidBudget->plannedPages;
        uint64_t migratePages = MIN(requestedPages, pidRemaining);

        if (demote) {
            /* Upper-layer targets own remote capacity; no remote reserve here. */
            plan->demotePages = (uint32_t)migratePages;
        } else {
            int destination = plan->localNid;
            uint64_t freePages = stagedContext.freePages[destination];
            uint64_t reservePages = stagedContext.safetyReservePages[destination];
            uint64_t plannedPages = stagedContext.plannedPages[destination];
            uint64_t safeFreePages = freePages <= reservePages || freePages - reservePages <= plannedPages ?
                                         0 :
                                         freePages - reservePages - plannedPages;
            migratePages = MIN(migratePages, safeFreePages);
            plan->promotePages = (uint32_t)migratePages;
            stagedContext.plannedPages[destination] += migratePages;
        }
        pidBudget->plannedPages += migratePages;
    }

    ret = memcpy_s(plans, planCap * sizeof(PairPlan), stagedPlans, inputCnt * sizeof(PairPlan));
    if (ret != EOK) {
        ret = -ret;
        goto OUT;
    }
    ret = memcpy_s(pidBudgets, pidBudgetCnt * sizeof(PairPidBudget), stagedPidBudgets,
                   pidBudgetCnt * sizeof(PairPidBudget));
    if (ret != EOK) {
        ret = -ret;
        goto OUT;
    }
    ret = memcpy_s(context->plannedPages, sizeof(context->plannedPages), stagedContext.plannedPages,
                   sizeof(context->plannedPages));
    if (ret != EOK) {
        ret = -ret;
        goto OUT;
    }
    *planCnt = inputCnt;

OUT:
    free(stagedPlans);
    free(stagedPidBudgets);
    return ret;
}

typedef struct {
    ProcessAttr *process;
    bool pairSeen[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    uint32_t nrMigratePages[MAX_NODES][MAX_NODES];
} PairPlanMatrixStage;

static PairPlanMatrixStage *FindPairPlanMatrixStage(PairPlanMatrixStage stages[], size_t stageCnt, pid_t pid)
{
    for (size_t i = 0; i < stageCnt; i++) {
        if (stages[i].process->pid == pid) {
            return &stages[i];
        }
    }
    return NULL;
}

int ApplyPairPlansForState(struct ProcessManager *manager, const PairPlan plans[], size_t planCnt)
{
    if (!manager || planCnt > MAX_PAIR_TARGET_COUNT || (planCnt > 0 && !plans)) {
        return -EINVAL;
    }

    PairPlanMatrixStage *stages = calloc(MAX_4K_PROCESSES_CNT, sizeof(PairPlanMatrixStage));
    if (!stages) {
        return -ENOMEM;
    }

    int ret = 0;
    size_t stageCnt = 0;
    EnvMutexLock(&manager->lock);
    if (manager->nrLocalNuma <= 0 || manager->nrLocalNuma > LOCAL_NUMA_NUM) {
        ret = -EINVAL;
        goto OUT;
    }
    for (ProcessAttr *process = manager->processes; process; process = process->next) {
        if (process->scanType != NORMAL_SCAN || process->groupPolicy.enabled || process->state != PROC_MIGRATE) {
            continue;
        }
        if (stageCnt == MAX_4K_PROCESSES_CNT) {
            ret = -EOVERFLOW;
            goto OUT;
        }
        stages[stageCnt++].process = process;
    }

    for (size_t i = 0; i < planCnt; i++) {
        const PairPlan *plan = &plans[i];
        PairPlanMatrixStage *stage = FindPairPlanMatrixStage(stages, stageCnt, plan->pid);
        if (!stage || plan->localNid < 0 || plan->localNid >= manager->nrLocalNuma || plan->remoteIndex < 0 ||
            plan->remoteIndex >= REMOTE_NUMA_NUM || plan->remoteNid != manager->nrLocalNuma + plan->remoteIndex ||
            plan->remoteNid >= MAX_NODES || stage->pairSeen[plan->localNid][plan->remoteIndex] ||
            !IsPairPlanDirectionValid(plan)) {
            ret = -EINVAL;
            goto OUT;
        }
        stage->pairSeen[plan->localNid][plan->remoteIndex] = true;
        if (plan->demotePages > UINT32_MAX - plan->swapPages || plan->promotePages > UINT32_MAX - plan->swapPages) {
            ret = -EOVERFLOW;
            goto OUT;
        }
        stage->nrMigratePages[plan->localNid][plan->remoteNid] = plan->demotePages + plan->swapPages;
        stage->nrMigratePages[plan->remoteNid][plan->localNid] = plan->promotePages + plan->swapPages;
    }

    for (size_t i = 0; i < stageCnt; i++) {
        ret = memcpy_s(stages[i].process->strategyAttr.nrMigratePages,
                       sizeof(stages[i].process->strategyAttr.nrMigratePages), stages[i].nrMigratePages,
                       sizeof(stages[i].nrMigratePages));
        if (ret != EOK) {
            ret = -ret;
            goto OUT;
        }
    }

OUT:
    EnvMutexUnlock(&manager->lock);
    free(stages);
    return ret;
}

int BuildAllPairPlans(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt)
{
    if (!manager || !plans || !planCnt || planCap > MAX_PAIR_TARGET_COUNT) {
        return -EINVAL;
    }
    *planCnt = 0;

    PairPlan *inputs = planCap == 0 ? NULL : calloc(planCap, sizeof(PairPlan));
    PairPlan *stagedPlans = planCap == 0 ? NULL : calloc(planCap, sizeof(PairPlan));
    PairPidBudget *pidBudgets = calloc(MAX_4K_PROCESSES_CNT, sizeof(PairPidBudget));
    if ((planCap > 0 && (!inputs || !stagedPlans)) || !pidBudgets) {
        free(inputs);
        free(stagedPlans);
        free(pidBudgets);
        return -ENOMEM;
    }

    bool hugePage;
    int nrLocalNuma;
    EnvMutexLock(&manager->lock);
    hugePage = manager->tracking.pageSize == PAGESIZE_2M;
    nrLocalNuma = manager->nrLocalNuma;
    EnvMutexUnlock(&manager->lock);

    PairPlanContext context;
    int ret = CollectNodeFreeSnapshot(hugePage, nrLocalNuma, &context);
    size_t inputCnt = 0;
    size_t pidBudgetCnt = 0;
    if (!ret) {
        ret = BuildAllPairPlanInputsForState(manager, inputs, planCap, &inputCnt, pidBudgets, MAX_4K_PROCESSES_CNT,
                                             &pidBudgetCnt, true);
    }
    if (!ret) {
        size_t activeInputCnt = 0;
        for (size_t i = 0; i < inputCnt; i++) {
            if (IsNodeForbidden(inputs[i].remoteNid)) {
                /* A disabled remote is frozen until it is enabled again. */
                continue;
            }
            inputs[activeInputCnt++] = inputs[i];
        }
        inputCnt = activeInputCnt;
    }
    size_t stagedPlanCnt = 0;
    if (!ret) {
        ret =
            BuildPairPlans(inputs, inputCnt, &context, pidBudgets, pidBudgetCnt, stagedPlans, planCap, &stagedPlanCnt);
    }
    if (!ret) {
        ret = BuildPairSwapPlans(manager, stagedPlans, stagedPlanCnt, &context, pidBudgets, pidBudgetCnt);
    }
    if (!ret) {
        ret = ApplyPairPlansForState(manager, stagedPlans, stagedPlanCnt);
    }
    if (!ret) {
        if (stagedPlanCnt > 0) {
            ret = memcpy_s(plans, planCap * sizeof(PairPlan), stagedPlans, stagedPlanCnt * sizeof(PairPlan));
            if (ret != EOK) {
                ret = -ret;
            }
        }
        if (!ret) {
            *planCnt = stagedPlanCnt;
        }
    }

    free(inputs);
    free(stagedPlans);
    free(pidBudgets);
    return ret;
}

static void FreeMigList(struct MigList mList[MAX_NODES][MAX_NODES])
{
    for (int from = 0; from < MAX_NODES; from++) {
        for (int to = 0; to < MAX_NODES; to++) {
            if (mList[from][to].addr) {
                free(mList[from][to].addr);
                mList[from][to].addr = NULL;
            }
        }
    }
}

static void StrategyInitMigList(struct MigList mList[MAX_NODES][MAX_NODES], int pid)
{
    for (int from = 0; from < MAX_NODES; from++) {
        for (int to = 0; to < MAX_NODES; to++) {
            mList[from][to].nr = 0;
            mList[from][to].pid = pid;
            mList[from][to].from = from;
            mList[from][to].to = to;
            mList[from][to].addr = NULL;
        }
    }
}

static int BuildMigrationMsg(ProcessAttr *process, struct MigrateMsg *mMsg, uint64_t *migratePage)
{
    int ret = CheckActcDataValid(process);
    if (ret) {
        return ret;
    }
    uint32_t nodes = process->numaAttr.numaNodes;
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    for (int i = LOCAL_NUMA_BITS; i < MAX_NODES; i++) {
        if (!(nodes & (1U << i))) {
            continue;
        }
        int nid = i - offset;
        if (IsNodeForbidden(nid) && process->groupPolicy.enabled) {
            SMAP_LOGGER_INFO("L2 node%d is forbiddened, pid %d stops migrate out.", nid, process->pid);
            return -EPERM;
        }
        if (IsRemoteNumaCriticalErr(nid)) {
            SMAP_LOGGER_WARNING("L2 node%d is critical error, pid %d stops migrate out.", nid, process->pid);
            return -ENODEV;
        }
    }
    if (!migratePage) {
        SMAP_LOGGER_ERROR("migratePage is null.");
        return -EINVAL;
    }
    struct MigList migList[MAX_NODES][MAX_NODES];
    StrategyInitMigList(migList, process->pid);
    ret = RunStrategy(process, migList, MAX_NODES);
    if (ret) {
        SMAP_LOGGER_ERROR("Run strategy for pid %d failed: %d.", process->pid, ret);
        FreeMigList(migList);
        return ret;
    }

    uint64_t nrMigTotal = 0;
    for (int from = 0; from < MAX_NODES; from++) {
        for (int to = 0; to < MAX_NODES; to++) {
            if (!migList[from][to].nr) {
                continue;
            }
            ret = AddMigList(mMsg, &migList[from][to]);
            if (ret == -ENOSPC) {
                SMAP_LOGGER_WARNING("Pid %d migration list is deferred by kernel entry limit.", process->pid);
                FreeMigList(migList);
                *migratePage += nrMigTotal;
                return 0;
            }
            if (ret) {
                SMAP_LOGGER_ERROR("Pid %d AddMigList %d %d failed: %d.", process->pid, from, to, ret);
                FreeMigList(migList);
                return ret;
            }
            nrMigTotal += migList[from][to].nr;
            SMAP_LOGGER_INFO("Numa %d --> Numa %d, mig %d pages.", from, to, migList[from][to].nr);
        }
    }
    FreeMigList(migList);
    *migratePage = *migratePage + nrMigTotal;
    SMAP_LOGGER_DEBUG("Pid %d migList len %llu.", process->pid, nrMigTotal);
    return 0;
}

static uint64_t GetMigListSuccessPages(const struct MigList *list)
{
    if (!list->successToUser || list->failedMigNr >= list->nr) {
        return 0;
    }
    uint64_t success = list->nr - list->failedMigNr;
    if (list->failedIsolatedNr >= success) {
        return 0;
    }
    return success - list->failedIsolatedNr;
}

static void RefreshPairAccountMask(ProcessAttr *process, int localNid, int remoteIndex)
{
    uint32_t localBit = 1U << localNid;
    if (process->strategyAttr.remoteNrPagesAfterMigrate[localNid][remoteIndex] > 0) {
        process->managedLocalState.accountLocalMask[remoteIndex] |= localBit;
    } else {
        process->managedLocalState.accountLocalMask[remoteIndex] &= ~localBit;
    }
    process->managedLocalState.managedLocalMask |= process->managedLocalState.accountLocalMask[remoteIndex];
}

void UpdateMigResult(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    ProcessAttr *current;
    uint64_t successMigCount;
    int fromNid;
    int toNid;
    for (int i = 0; i < mMsg->cnt; i++) {
        current = GetProcessAttrLocked(mMsg->migList[i].pid);
        if (!current || !mMsg->migList[i].successToUser) {
            continue;
        }

        fromNid = mMsg->migList[i].from;
        toNid = mMsg->migList[i].to;
        successMigCount = GetMigListSuccessPages(&mMsg->migList[i]);

        if (current->groupPolicy.enabled) {
            UpdateGroupedMigrationResult(current, fromNid, toNid, successMigCount);
            continue;
        }

        bool fromLocal = fromNid >= 0 && fromNid < manager->nrLocalNuma;
        bool toLocal = toNid >= 0 && toNid < manager->nrLocalNuma;
        int remoteNidEnd = manager->nrLocalNuma + REMOTE_NUMA_NUM;
        bool fromRemote = fromNid >= manager->nrLocalNuma && fromNid < remoteNidEnd;
        bool toRemote = toNid >= manager->nrLocalNuma && toNid < remoteNidEnd;
        if (fromLocal && toRemote) {
            int remoteIndex = toNid - manager->nrLocalNuma;
            uint32_t *account = &current->strategyAttr.remoteNrPagesAfterMigrate[fromNid][remoteIndex];
            if (successMigCount > UINT32_MAX - *account) {
                SMAP_LOGGER_WARNING("Pid %d Pair account overflow after demote from %d to %d "
                                    "by %llu pages; saturating.",
                                    current->pid, fromNid, toNid, successMigCount);
                *account = UINT32_MAX;
            } else {
                *account += successMigCount;
            }
            RefreshPairAccountMask(current, fromNid, remoteIndex);
        } else if (fromRemote && toLocal) {
            int remoteIndex = fromNid - manager->nrLocalNuma;
            uint32_t *account = &current->strategyAttr.remoteNrPagesAfterMigrate[toNid][remoteIndex];
            if (successMigCount > *account) {
                SMAP_LOGGER_WARNING("Pid %d Pair account has only %u pages while promote "
                                    "from %d to %d succeeded for %llu pages; clearing account.",
                                    current->pid, *account, fromNid, toNid, successMigCount);
                *account = 0;
            } else {
                *account -= successMigCount;
            }
            RefreshPairAccountMask(current, toNid, remoteIndex);
        } else {
            SMAP_LOGGER_DEBUG("Skip non-Pair migration result for pid %d from %d to %d.", current->pid, fromNid, toNid);
            continue;
        }
        SMAP_LOGGER_INFO("pid %d from %d to %d nr %llu failed_mig_nr %llu "
                         "failed_isolated_nr %llu success_mig_nr %llu.",
                         mMsg->migList[i].pid, mMsg->migList[i].from, mMsg->migList[i].to, mMsg->migList[i].nr,
                         mMsg->migList[i].failedMigNr, mMsg->migList[i].failedIsolatedNr, successMigCount);
    }
}

static void ResetGroupedSwapRuntimeLocked(ProcessAttr *process, bool freeze)
{
    process->groupSwapStableTotalRounds = 0;
    process->groupSwapTotalPagesValid = false;
    process->groupSwapFrozen = freeze;
    for (int i = 0; i < process->groupPolicy.groupCount; i++) {
        process->groupPolicy.groups[i].swapCandidateRounds = 0;
    }
}

static void FreezeGroupedSwapLocked(struct ProcessManager *manager, pid_t pid)
{
    ProcessAttr *process = GetProcessAttrLocked(pid);
    if (!process || !process->groupPolicy.enabled) {
        return;
    }
    ResetGroupedSwapRuntimeLocked(process, true);
    SMAP_LOGGER_ERROR("grouped pid %d swap frozen due to unbalanced swap migration.", pid);
}

static int FindGroupSwapPairStat(GroupSwapPairStat stats[], int statCnt, pid_t pid, int localNid, int remoteNid)
{
    for (int i = 0; i < statCnt; i++) {
        if (stats[i].pid == pid && stats[i].localNid == localNid && stats[i].remoteNid == remoteNid) {
            return i;
        }
    }
    return -1;
}

static int AddGroupSwapPairStat(GroupSwapPairStat stats[], int *statCnt, int maxStats, pid_t pid, int localNid,
                                int remoteNid)
{
    int idx = FindGroupSwapPairStat(stats, *statCnt, pid, localNid, remoteNid);
    if (idx >= 0) {
        return idx;
    }
    if (*statCnt >= maxStats) {
        return -ENOSPC;
    }
    idx = *statCnt;
    stats[idx].pid = pid;
    stats[idx].localNid = localNid;
    stats[idx].remoteNid = remoteNid;
    (*statCnt)++;
    return idx;
}

static void AddGroupSwapCompPlan(GroupSwapCompPlan plans[], int *planCnt, pid_t pid, int from, int to, uint64_t need)
{
    if (need == 0 || *planCnt >= MAX_GROUP_SWAP_COMP_PLANS) {
        return;
    }
    plans[*planCnt].pid = pid;
    plans[*planCnt].from = from;
    plans[*planCnt].to = to;
    plans[*planCnt].need = need;
    (*planCnt)++;
}

static int BuildGroupedSwapCompPlans(struct MigrateMsg *mMsg, struct ProcessManager *manager, GroupSwapCompPlan plans[],
                                     int *planCnt)
{
    GroupSwapPairStat stats[MAX_GROUP_SWAP_COMP_PLANS] = { 0 };
    int statCnt = 0;
    *planCnt = 0;

    for (int i = 0; i < mMsg->cnt; i++) {
        struct MigList *list = &mMsg->migList[i];
        ProcessAttr *process = GetProcessAttrLocked(list->pid);
        if (!process || !process->groupPolicy.enabled) {
            continue;
        }
        bool localToRemote = list->from < manager->nrLocalNuma && list->to >= manager->nrLocalNuma;
        bool remoteToLocal = list->from >= manager->nrLocalNuma && list->to < manager->nrLocalNuma;
        if (!localToRemote && !remoteToLocal) {
            continue;
        }
        int localNid = localToRemote ? list->from : list->to;
        int remoteNid = localToRemote ? list->to : list->from;
        int idx = AddGroupSwapPairStat(stats, &statCnt, MAX_GROUP_SWAP_COMP_PLANS, list->pid, localNid, remoteNid);
        if (idx < 0) {
            SMAP_LOGGER_ERROR("grouped pid %d swap compensation stat exceeds limit.", list->pid);
            return idx;
        }
        uint64_t success = GetMigListSuccessPages(list);
        if (localToRemote) {
            stats[idx].localToRemoteSuccess += success;
            stats[idx].hasLocalToRemote = true;
        } else {
            stats[idx].remoteToLocalSuccess += success;
            stats[idx].hasRemoteToLocal = true;
        }
    }

    for (int i = 0; i < statCnt; i++) {
        GroupSwapPairStat *stat = &stats[i];
        if (!stat->hasLocalToRemote || !stat->hasRemoteToLocal) {
            continue;
        }
        if (stat->remoteToLocalSuccess > stat->localToRemoteSuccess) {
            uint64_t need = stat->remoteToLocalSuccess - stat->localToRemoteSuccess;
            AddGroupSwapCompPlan(plans, planCnt, stat->pid, stat->localNid, stat->remoteNid, need);
            SMAP_LOGGER_WARNING("grouped pid %d swap imbalance local %d remote %d, compensate %llu pages %d -> %d.",
                                stat->pid, stat->localNid, stat->remoteNid, need, stat->localNid, stat->remoteNid);
        } else if (stat->localToRemoteSuccess > stat->remoteToLocalSuccess) {
            uint64_t need = stat->localToRemoteSuccess - stat->remoteToLocalSuccess;
            AddGroupSwapCompPlan(plans, planCnt, stat->pid, stat->remoteNid, stat->localNid, need);
            SMAP_LOGGER_WARNING("grouped pid %d swap imbalance local %d remote %d, compensate %llu pages %d -> %d.",
                                stat->pid, stat->localNid, stat->remoteNid, need, stat->remoteNid, stat->localNid);
        }
    }
    return 0;
}

static int AppendCompMigResults(struct MigrateMsg *mMsg, const struct MigrateMsg *compMsg)
{
    if (compMsg->cnt == 0) {
        return 0;
    }
    int oldCnt = mMsg->cnt;
    int newCnt = oldCnt + compMsg->cnt;
    struct MigList *newList = realloc(mMsg->migList, sizeof(struct MigList) * newCnt);
    if (!newList) {
        return -ENOMEM;
    }
    mMsg->migList = newList;
    for (int i = 0; i < compMsg->cnt; i++) {
        mMsg->migList[oldCnt + i] = compMsg->migList[i];
        mMsg->migList[oldCnt + i].addr = NULL;
    }
    mMsg->cnt = newCnt;
    return 0;
}

static uint64_t BuildCompMigListFromScan(ProcessAttr *process, GroupSwapCompPlan *plan, struct MigList *list)
{
    uint64_t sourcePages = process->scanAttr.actcLen[plan->from];
    uint64_t destFreePages = GetNrFreeHugePagesByNode(plan->to);
    uint64_t nr = MIN(plan->need, sourcePages);
    nr = MIN(nr, destFreePages);
    if (nr == 0 || !process->scanAttr.actcData[plan->from]) {
        plan->shouldFreeze = true;
        return 0;
    }

    list->addr = malloc(sizeof(uint64_t) * nr);
    if (!list->addr) {
        plan->shouldFreeze = true;
        return 0;
    }
    list->pid = plan->pid;
    list->from = plan->from;
    list->to = plan->to;
    list->nr = nr;
    for (uint64_t i = 0; i < nr; i++) {
        list->addr[i] = i;
    }
    plan->built = nr;
    if (nr < plan->need) {
        plan->shouldFreeze = true;
        SMAP_LOGGER_WARNING("grouped pid %d swap compensation only builds %llu/%llu pages from %d to %d.", plan->pid,
                            nr, plan->need, plan->from, plan->to);
    }
    return nr;
}

static int BuildGroupedSwapCompMsg(struct ProcessManager *manager, GroupSwapCompPlan plans[], int planCnt,
                                   struct MigrateMsg *compMsg, int compPlanIdx[])
{
    compMsg->migList = calloc(planCnt, sizeof(struct MigList));
    if (!compMsg->migList) {
        return -ENOMEM;
    }
    compMsg->cnt = 0;
    compMsg->mulMig.nrThread = 1;
    compMsg->mulMig.isMulThread = false;
    compMsg->mulMig.pageSize = manager->tracking.pageSize;

    EnvMutexLock(&manager->lock);
    for (int i = 0; i < planCnt; i++) {
        ProcessAttr *process = GetProcessAttrLocked(plans[i].pid);
        if (!process || !process->groupPolicy.enabled) {
            plans[i].shouldFreeze = true;
            continue;
        }
        struct MigList *list = &compMsg->migList[compMsg->cnt];
        uint64_t nr = BuildCompMigListFromScan(process, &plans[i], list);
        if (nr == 0) {
            continue;
        }
        list->pid = plans[i].pid;
        list->from = plans[i].from;
        list->to = plans[i].to;
        compPlanIdx[compMsg->cnt] = i;
        compMsg->cnt++;
    }
    EnvMutexUnlock(&manager->lock);
    return 0;
}

static void FreeCompMigMsg(struct MigrateMsg *compMsg)
{
    if (!compMsg || !compMsg->migList) {
        return;
    }
    for (int i = 0; i < compMsg->cnt; i++) {
        free(compMsg->migList[i].addr);
        compMsg->migList[i].addr = NULL;
    }
    free(compMsg->migList);
    compMsg->migList = NULL;
    compMsg->cnt = 0;
}

static void FreezeFailedCompPlans(struct ProcessManager *manager, GroupSwapCompPlan plans[], int planCnt)
{
    EnvMutexLock(&manager->lock);
    for (int i = 0; i < planCnt; i++) {
        if (plans[i].shouldFreeze) {
            FreezeGroupedSwapLocked(manager, plans[i].pid);
        }
    }
    EnvMutexUnlock(&manager->lock);
}

static int RunGroupedSwapCompensation(struct ProcessManager *manager, struct MigrateMsg *mMsg,
                                      GroupSwapCompPlan plans[], int planCnt)
{
    struct MigrateMsg compMsg = { 0 };
    int compPlanIdx[MAX_GROUP_SWAP_COMP_PLANS] = { 0 };
    int ret = BuildAllPidData();
    if (ret) {
        SMAP_LOGGER_ERROR("grouped swap compensation refresh pid data failed: %d.", ret);
        for (int i = 0; i < planCnt; i++) {
            plans[i].shouldFreeze = true;
        }
        FreezeFailedCompPlans(manager, plans, planCnt);
        return ret;
    }

    ret = BuildGroupedSwapCompMsg(manager, plans, planCnt, &compMsg, compPlanIdx);
    if (ret) {
        for (int i = 0; i < planCnt; i++) {
            plans[i].shouldFreeze = true;
        }
        FreezeFailedCompPlans(manager, plans, planCnt);
        return ret;
    }
    bool hasCompPages = false;
    for (int i = 0; i < compMsg.cnt; i++) {
        if (compMsg.migList[i].nr > 0) {
            hasCompPages = true;
            break;
        }
    }
    if (hasCompPages) {
        ret = ioctl(manager->fds.migrate, SMAP_MIG_MIGRATE, &compMsg);
        if (ret) {
            SMAP_LOGGER_ERROR("grouped swap compensation migrate failed: %d.", ret);
        }
    }

    for (int i = 0; i < compMsg.cnt; i++) {
        int planIdx = compPlanIdx[i];
        uint64_t success = GetMigListSuccessPages(&compMsg.migList[i]);
        if (ret || success < compMsg.migList[i].nr) {
            plans[planIdx].shouldFreeze = true;
            SMAP_LOGGER_ERROR("grouped pid %d swap compensation from %d to %d success %llu/%llu.", plans[planIdx].pid,
                              plans[planIdx].from, plans[planIdx].to, success, compMsg.migList[i].nr);
        }
    }
    int appendRet = AppendCompMigResults(mMsg, &compMsg);
    if (appendRet) {
        for (int i = 0; i < planCnt; i++) {
            plans[i].shouldFreeze = true;
        }
        ret = appendRet;
    }
    FreezeFailedCompPlans(manager, plans, planCnt);
    FreeCompMigMsg(&compMsg);
    return ret;
}

int DoMigration(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    int err = 0;
    SMAP_LOGGER_DEBUG("mMsg->cnt %d.", mMsg->cnt);

    uint64_t **tmpAddr = malloc(sizeof(*tmpAddr) * mMsg->cnt);
    if (!tmpAddr) {
        SMAP_LOGGER_ERROR("malloc tmp addr failed.");
        return -ENOMEM;
    }
    for (int i = 0; i < mMsg->cnt; i++) {
        SMAP_LOGGER_DEBUG("nr is %d, from is %d, to is %d, addr list :.", mMsg->migList[i].nr, mMsg->migList[i].from,
                          mMsg->migList[i].to);
        tmpAddr[i] = mMsg->migList[i].addr;
        for (int j = 0; j < mMsg->migList[i].nr; j++) {
            SMAP_LOGGER_DEBUG("%#llx .", mMsg->migList[i].addr[j]);
            if (j >= MAX_MIG_ADDR_PRINT_LEN) {
                SMAP_LOGGER_DEBUG(" ... .");
                break;
            }
        }
    }
    if (mMsg->cnt > 0) {
        err = ioctl(manager->fds.migrate, SMAP_MIG_MIGRATE, mMsg);
    }
    for (int i = 0; i < mMsg->cnt; i++) {
        if (tmpAddr[i]) {
            free(tmpAddr[i]);
        }
    }
    free(tmpAddr);
    return err;
}

static int InitMigrateMsg(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    PidType type = GetPidType(manager);
    // 按照pid粒度进行迁移，申请的迁移数组大小为pid的数量*2
    int maxProcessCnt = GetCurrentMaxNrPid();
    int maxPathNum = maxProcessCnt * MAX_PER_PID_MIG_LIST_COUNT;
    mMsg->cnt = 0;
    mMsg->migList = calloc(maxPathNum, sizeof(struct MigList));
    if (!mMsg->migList) {
        SMAP_LOGGER_ERROR("mMsg->migList malloc failed.");
        return -ENOMEM;
    }
    mMsg->mulMig.nrThread = 1;
    mMsg->mulMig.isMulThread = false;
    mMsg->mulMig.pageSize = manager->tracking.pageSize;
    return 0;
}

static int CleanStrategyAttribute(struct ProcessManager *manager)
{
    ProcessAttr *current;
    EnvMutexLock(&manager->lock);
    for (current = manager->processes; current; current = current->next) {
        int ret;
        StrategyAttribute *strAttr = &current->strategyAttr;
        for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
            ret = memset_s(strAttr->l3RemoteMemRatio[i], sizeof(strAttr->l3RemoteMemRatio[0]), 0,
                           sizeof(strAttr->l3RemoteMemRatio[0]));
            if (ret != EOK) {
                EnvMutexUnlock(&manager->lock);
                return ret;
            }
            ret = memset_s(strAttr->allocRemoteNrPages[i], sizeof(strAttr->allocRemoteNrPages[0]), 0,
                           sizeof(strAttr->allocRemoteNrPages[0]));
            if (ret != EOK) {
                EnvMutexUnlock(&manager->lock);
                return ret;
            }
        }
        ret = memset_s(strAttr->nrPagesPerLocalNuma, sizeof(strAttr->nrPagesPerLocalNuma), 0,
                       sizeof(strAttr->nrPagesPerLocalNuma));
        if (ret != EOK) {
            EnvMutexUnlock(&manager->lock);
            return ret;
        }
        for (int i = 0; i < MAX_NODES; i++) {
            ret = memset_s(strAttr->nrMigratePages[i], sizeof(strAttr->nrMigratePages[0]), 0,
                           sizeof(strAttr->nrMigratePages[0]));
            if (ret != EOK) {
                EnvMutexUnlock(&manager->lock);
                return ret;
            }
        }
        ret = memset_s(strAttr->ubBwRestrict, sizeof(strAttr->ubBwRestrict), 0, sizeof(strAttr->ubBwRestrict));
        if (ret != EOK) {
            EnvMutexUnlock(&manager->lock);
            return ret;
        }
    }
    EnvMutexUnlock(&manager->lock);
    return 0;
}

static int PerformMigrationPreparation(struct ProcessManager *manager)
{
    int ret = 0;
    ProcessAttr *current = manager->processes;

    if (!current) {
        return -EINVAL;
    }
    ret = CleanStrategyAttribute(manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Clean StrategyAttribute failed! ret: %d.", ret);
        return ret;
    }
    ret = BuildAllPidData();
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Build all pid data failed! ret:%d.", ret);
        return ret;
    }
    if (ret > 0) {
        SMAP_LOGGER_WARNING("Build pid data failed! nums:%d.", ret);
        return 0;
    }
    SmapAutoRemoveRemoteEmptyProcessesWithFreshData();
    return 0;
}

static int SetMigrateThreadNum(struct MigrateMsg *mMsg, uint64_t migratePages, bool isForcedSingleThread)
{
    if (!mMsg) {
        return -EINVAL;
    }
    if (mMsg->mulMig.pageSize == GetHugePageSize() && isForcedSingleThread) {
        if (migratePages) {
            mMsg->mulMig.isMulThread = false;
            mMsg->mulMig.nrThread = SIG_THREAD_MIG_OUT;
            SMAP_LOGGER_INFO("Forced signle thread detected, set 1 migration thread.");
            return 0;
        }
        SMAP_LOGGER_INFO("As for 0 pages, no migration.");
        return 0;
    }
    if (mMsg->mulMig.pageSize == GetHugePageSize() && migratePages > LESS_MIG_OUT_2M_PAGE_THRE) {
        mMsg->mulMig.isMulThread = true;
        if (migratePages <= MORE_MIG_OUT_2M_PAGE_THRE) {
            mMsg->mulMig.nrThread = LESS_THREAD_MIG_OUT;
        } else {
            mMsg->mulMig.nrThread = MORE_THREAD_MIG_OUT;
        }
    } else {
        mMsg->mulMig.isMulThread = false;
        mMsg->mulMig.nrThread = SIG_THREAD_MIG_OUT;
    }
    if (migratePages) {
        SMAP_LOGGER_INFO("As for %d pages, set %d migration thread(s).", migratePages, mMsg->mulMig.nrThread);
    } else {
        SMAP_LOGGER_INFO("As for 0 pages, no migration.");
    }
    return 0;
}

static inline long CalcDurationUs(struct timeval start, struct timeval end)
{
    long seconds, micros;

    seconds = end.tv_sec - start.tv_sec;
    micros = end.tv_usec - start.tv_usec;
    return seconds * US_PER_SEC + micros;
}

static void PrintMigSpeed(struct ProcessManager *manager, uint64_t nr, struct timeval start, struct timeval end)
{
    double timeUsed, migSpeed;
    double amount;
    long temp;
    uint32_t size;

    if (nr == 0) {
        return;
    }

    size = GetPageSize();
    amount = (double)nr * size / GIB;

    temp = CalcDurationUs(start, end);
    if (temp == 0) {
        return;
    }
    timeUsed = (double)temp;
    migSpeed = amount / timeUsed * US_PER_SEC;
    SMAP_LOGGER_DEBUG("Migrate %llu pages, used %.0lfus, speed %.2lfGB/sec.", nr, timeUsed, migSpeed);
}

static void CalProcessNuma(StrategyAttribute *strategyAttr)
{
    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < nrLocalNuma; i++) {
        if (strategyAttr->nrPagesPerLocalNuma[i] == 0) {
            continue;
        }
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            double resRatio = ((double)strategyAttr->allocRemoteNrPages[i][j] / strategyAttr->nrPagesPerLocalNuma[i]) -
                              (strategyAttr->l3RemoteMemRatio[i][j] / HUNDRED);
            int32_t tmp = ceil(strategyAttr->nrPagesPerLocalNuma[i] * resRatio);
            SMAP_LOGGER_DEBUG("CalProcessNuma l3Ratio[%d][%d]: %.2lf, resRatio: %.2lf, tmp: %d.", i, j,
                              strategyAttr->l3RemoteMemRatio[i][j], resRatio, tmp);

            if (tmp > 0) {
                strategyAttr->nrMigratePages[j + nrLocalNuma][i] = tmp;
                SMAP_LOGGER_DEBUG("CalProcessNuma tmp > 0[%d][%d]: %u.", j + nrLocalNuma, i,
                                  strategyAttr->nrMigratePages[j + nrLocalNuma][i]);
            }
            if (tmp < 0) {
                strategyAttr->nrMigratePages[i][j + nrLocalNuma] = (-tmp);
                SMAP_LOGGER_DEBUG("CalProcessNuma tmp < 0[%d][%d]:  %u.", i, j + nrLocalNuma,
                                  strategyAttr->nrMigratePages[i][j + nrLocalNuma]);
            }
        }
    }
}

typedef struct {
    int numaId;
    int amount;
} NumaMemReduce;

#define MAX_MIG_REDUCE_NUMA 20

int CompareMigIn(const void *a, const void *b)
{
    return ((NumaMemReduce *)a)->amount - ((NumaMemReduce *)b)->amount;
}

int CompareMigOut(const void *b, const void *a)
{
    return ((NumaMemReduce *)a)->amount - ((NumaMemReduce *)b)->amount;
}

static void NumaSwapMemPool(ProcessAttr *current)
{
    if (IsMultiNumaVm(current)) {
        return;
    }

    for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
        if (!InAttrL1(current, i)) {
            continue;
        }

        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            StrategyAttribute *sa = &current->strategyAttr;
            int l2Node = GetNrLocalNuma() + j;
            uint32_t targetNum = KBToPage(sa->memSize[i][j]);
            uint64_t whiteNum = current->scanAttr.actCount[i].whiteNum;
            /* 饱和减法：旧 whiteNum 残留时防止 localNum 下溢成超大值 */
            uint32_t localNum =
                current->walkPage.nrPages[i] > whiteNum ? (uint32_t)(current->walkPage.nrPages[i] - whiteNum) : 0;
            uint32_t remoteNum = sa->remoteNrPagesAfterMigrate[i][j];
            uint32_t migNum;

            if (remoteNum == 0 && targetNum == 0) {
                continue;
            }

            if (remoteNum > targetNum) {
                migNum = remoteNum - targetNum;
                sa->nrMigratePages[l2Node][i] = migNum;
                SMAP_LOGGER_INFO("[swap_pool] pid=%d src=%d dst=%d remote_pages=%u target_pages=%u mig_pages=%u",
                                 current->pid, l2Node, i, remoteNum, targetNum, migNum);
            } else {
                migNum = MIN(localNum, targetNum - remoteNum);
                sa->nrMigratePages[i][l2Node] = migNum;
                SMAP_LOGGER_INFO("[swap_pool] pid=%d src=%d dst=%d remote_pages=%u target_pages=%u mig_pages=%u",
                                 current->pid, i, l2Node, remoteNum, targetNum, migNum);
            }
        }
    }
}

static void ApplyUbBwStop(ProcessAttr *current, struct ProcessManager *manager)
{
    uint32_t threshold = manager->ubBwMonitor.ubBwThreshold;
    if (!IsBwMonitorEnabled(manager) || manager->ubBwMonitor.currentFluxRet) {
        return;
    }

    for (int nid = 0; nid < MAX_NODES; nid++)
        current->strategyAttr.ubBwRestrict[nid] = UB_BW_NORMAL;

    for (int i = 0; i < manager->ubBwMonitor.currentFluxMb.len; i++) {
        int nid = manager->ubBwMonitor.currentFluxMb.flux[i].numaId;
        uint32_t totalBw =
            manager->ubBwMonitor.currentFluxMb.flux[i].readMb + manager->ubBwMonitor.currentFluxMb.flux[i].writeMb;

        if (totalBw >= threshold) {
            current->strategyAttr.ubBwRestrict[nid] = UB_BW_SWAP_STOP;
            SMAP_LOGGER_INFO("UB BW threshold: numa %d bw %uMB/s >= threshold %uMB/s, set UB_BW_SWAP_STOP flag.", nid,
                             totalBw, threshold);
        }
    }
}

static void NumaMigReduceDeal(ProcessAttr *current)
{
    if (current->migrateMode == MIG_MEMSIZE_MODE) {
        NumaSwapMemPool(current);
    } else {
        CalProcessNuma(&current->strategyAttr);
    }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            if (current->strategyAttr.nrMigratePages[i][j] > 0) {
                SMAP_LOGGER_INFO("After reduce migrate info: from: %d, to %d, num: %u.", i, j,
                                 current->strategyAttr.nrMigratePages[i][j]);
            }
        }
    }
}

static ProcessAttr *FindMigrationCandidateLocked(struct ProcessManager *manager, pid_t pid)
{
    for (ProcessAttr *current = manager->processes; current; current = current->next) {
        if (current->pid == pid) {
            return current;
        }
    }
    return NULL;
}

static int PreMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg, uint64_t *migratePages)
{
    int ret;
    ProcessAttr *current;
    bool isForcedSingleThread = false;
    size_t candidateCnt = 0;
    size_t planCnt = 0;
    pid_t *candidatePids = calloc(MAX_4K_PROCESSES_CNT, sizeof(pid_t));
    PairPlan *plans = calloc(MAX_PAIR_TARGET_COUNT, sizeof(PairPlan));
    if (!candidatePids || !plans) {
        free(candidatePids);
        free(plans);
        return -ENOMEM;
    }

    EnvMutexLock(&manager->lock);
    ret = InitMigrateMsg(mMsg, manager);
    if (ret) {
        EnvMutexUnlock(&manager->lock);
        SMAP_LOGGER_ERROR("InitMigrateMsg failed! ret:%d.", ret);
        goto OUT;
    }
    for (current = manager->processes; current; current = current->next) {
        if (current->scanType != NORMAL_SCAN) {
            continue;
        }
        if (current->state != PROC_IDLE) {
            SMAP_LOGGER_DEBUG("pid %d state is %d, skip migration.", current->pid, current->state);
            continue;
        }
        if (current->pendingTargetConfigValid) {
            ret = ApplyPendingMigrationTargets(current);
            if (ret) {
                SMAP_LOGGER_ERROR("Apply pending migration target before migration failed, "
                                  "pid %d ret %d.",
                                  current->pid, ret);
                ret = 0;
                continue;
            }
        }
        if (current->pendingGroupPolicy.valid) {
            /* Retry a previously failed deferred refresh before building new migrations. */
            ret = ApplyPendingGroupedPolicy(current);
            if (ret) {
                SMAP_LOGGER_ERROR("Apply pending grouped policy before migration failed, pid %d ret %d.", current->pid,
                                  ret);
                ret = 0;
                continue;
            }
        }
        SMAP_LOGGER_INFO("+++++++scan_and_strategy_thread: processing pid %d.", current->pid);
        if (candidateCnt == MAX_4K_PROCESSES_CNT) {
            ret = -EOVERFLOW;
            break;
        }
        ApplyUbBwStop(current, manager);
        current->state = PROC_MIGRATE;
        candidatePids[candidateCnt++] = current->pid;
        SMAP_LOGGER_DEBUG("change pid %d state from idle to migrate.", current->pid);
    }
    EnvMutexUnlock(&manager->lock);

    if (ret) {
        goto ROLLBACK;
    }

    /*
     * All candidate processes are in PROC_MIGRATE, so concurrent target
     * updates are deferred until PostMigration and cannot race this plan.
     */
    ret = BuildAllPairPlans(manager, plans, MAX_PAIR_TARGET_COUNT, &planCnt);
    if (ret) {
        SMAP_LOGGER_ERROR("Build all pair plans failed: %d.", ret);
        goto ROLLBACK;
    }

    EnvMutexLock(&manager->lock);
    for (size_t i = 0; i < candidateCnt; i++) {
        current = FindMigrationCandidateLocked(manager, candidatePids[i]);
        if (!current || current->state != PROC_MIGRATE) {
            continue;
        }
        if (current->groupPolicy.enabled) {
            NumaMigReduceDeal(current);
        }
        // 识别每个进程的待迁移冷热页
        ret = BuildMigrationMsg(current, mMsg, migratePages);
        SMAP_LOGGER_INFO("Add process: %d to migrate msg ret: %d.", current->pid, ret);
        isForcedSingleThread = isForcedSingleThread || current->vmPidAttr.mmapType;
    }
    EnvMutexUnlock(&manager->lock);

    SetMigrateThreadNum(mMsg, *migratePages, isForcedSingleThread);
    free(candidatePids);
    free(plans);
    return 0;

ROLLBACK:
    EnvMutexLock(&manager->lock);
    for (size_t i = 0; i < candidateCnt; i++) {
        current = FindMigrationCandidateLocked(manager, candidatePids[i]);
        if (current && current->state == PROC_MIGRATE) {
            current->state = PROC_IDLE;
        }
    }
    EnvMutexUnlock(&manager->lock);
    free(mMsg->migList);
    mMsg->migList = NULL;
OUT:
    free(candidatePids);
    free(plans);
    return ret;
}

static void PostMigration(struct ProcessManager *manager, struct MigrateMsg *mMsg)
{
    GroupSwapCompPlan plans[MAX_GROUP_SWAP_COMP_PLANS] = { 0 };
    int planCnt = 0;

    EnvMutexLock(&manager->lock);
    int ret = BuildGroupedSwapCompPlans(mMsg, manager, plans, &planCnt);
    EnvMutexUnlock(&manager->lock);
    if (!ret && planCnt > 0) {
        ret = RunGroupedSwapCompensation(manager, mMsg, plans, planCnt);
        if (ret) {
            SMAP_LOGGER_ERROR("Run grouped swap compensation failed: %d.", ret);
        }
    } else if (ret) {
        EnvMutexLock(&manager->lock);
        for (int i = 0; i < mMsg->cnt; i++) {
            ProcessAttr *process = GetProcessAttrLocked(mMsg->migList[i].pid);
            if (process && process->groupPolicy.enabled) {
                ResetGroupedSwapRuntimeLocked(process, true);
            }
        }
        EnvMutexUnlock(&manager->lock);
    }

    EnvMutexLock(&manager->lock);
    UpdateMigResult(mMsg, manager);
    free(mMsg->migList);
    mMsg->migList = NULL;
    for (ProcessAttr *current = manager->processes; current; current = current->next) {
        if (current->state == PROC_MIGRATE) {
            /*
             * The old migration result is now settled; it is safe to publish
             * pending normal targets and grouped policy.
             */
            int ret = ApplyPendingMigrationTargets(current);
            if (ret) {
                SMAP_LOGGER_ERROR("Apply pending migration target after migration failed, "
                                  "pid %d ret %d.",
                                  current->pid, ret);
            }
            ret = ApplyPendingGroupedPolicy(current);
            if (ret) {
                SMAP_LOGGER_ERROR("Apply pending grouped policy after migration failed, pid %d ret %d.", current->pid,
                                  ret);
            }
            SMAP_LOGGER_DEBUG("set pid %d state from migrate to idle.", current->pid);
            current->state = PROC_IDLE;
        }
    }
    EnvMutexUnlock(&manager->lock);
}

static int PerformMigration(struct ProcessManager *manager)
{
    int ret;
    uint64_t migratePages = 0;
    struct MigrateMsg mMsg;
    struct timeval start, end;

    ret = PreMigration(manager, &mMsg, &migratePages);
    if (ret) {
        SMAP_LOGGER_ERROR("PreMigration failed! ret: %d.", ret);
        return ret;
    }

    // 调用迁移接口
    gettimeofday(&start, NULL);
    ret = DoMigration(&mMsg, manager);
    gettimeofday(&end, NULL);
    PrintMigSpeed(manager, migratePages, start, end);
    SMAP_LOGGER_INFO("Do migration result: %d.", ret);
    PostMigration(manager, &mMsg);
    if (ret) {
        SMAP_LOGGER_INFO("Do migration failed! migration_failure_count=%d.", ret);
        return ret;
    }
    return ret;
}

static int UpdateScanTime(ProcessAttr *process)
{
    struct AccessAddPidPayload payload;
    payload.pid = process->pid;
    payload.numaNodes = process->numaAttr.numaNodes;
    payload.type = process->scanType;
    payload.duration = process->duration;
    payload.scanTime = GetFileConfSwitchConfig() ? GetScanPeriodConfig() : process->sceneInfo.cycles.scanCycle;
    int ret = AccessIoctlAddPid(1, &payload);
    if (ret) {
        SMAP_LOGGER_ERROR("Update scan time failed for pid %d, ret=%d.", process->pid, ret);
    } else {
        process->scanTime = payload.scanTime;
        SMAP_LOGGER_INFO("Update pid %d scan cycle to %dms.", process->pid, payload.scanTime);
    }

    return ret;
}

static void UpdateScene(struct ProcessManager *manager)
{
    EnvMutexLock(&manager->lock);
    ProcessAttr *current = manager->processes;
    while (current) {
        if (current->scanType == NORMAL_SCAN) {
            SetProcessSceneAttr(current);
        }
        current = current->next;
    }
    EnvMutexUnlock(&manager->lock);
}

static inline void HandleHighScan(ProcessAttr *current)
{
    int ret = UpdateScanTime(current);
    if (ret) {
        SMAP_LOGGER_WARNING("Update scan time failed for pid %d.", current->pid);
    } else {
        current->isFirstScan = false;
    }
}

static int HandleScene(ThreadCtx *ctx)
{
    int ret = 0;
    Scene worstScene = LIGHT_STABLE_SCENE, scene;
    struct ProcessManager *manager = ctx->processManager;
    PidType type = GetPidType(manager);
    EnvMutexLock(&manager->lock);
    ProcessAttr *current = manager->processes;
    while (current) {
        // 更新进程的场景
        SceneInfo *info = &current->sceneInfo;
        GetProcessSceneAttr(info->currScene, info, current->type);
        if (current->isFirstScan) {
            if (current->walkPage.nrPage == 0) {
                current->scanTime = DEFAULT_SCAN_PERIOD;
            } else {
                HandleHighScan(current);
            }
        } else if (info->currScene != info->lastScene) {
            if (UpdateScanTime(current)) {
                SMAP_LOGGER_WARNING("Update scan time failed for pid %d.", current->pid);
            }
        }
        scene = current->sceneInfo.currScene;
        if (scene > worstScene) {
            worstScene = scene;
        }
        current = current->next;
    }
    EnvMutexUnlock(&manager->lock);

    if (manager->sceneInfo.currScene != worstScene) {
        SMAP_LOGGER_INFO("Manager changed scene from %d to %d.", manager->sceneInfo.currScene, worstScene);
        manager->sceneInfo.currScene = worstScene;
        GetProcessSceneAttr(worstScene, &manager->sceneInfo, type);
        SceneCycle *cycles = &manager->sceneInfo.cycles;
        ctx->period = cycles->migCycle;
    }
    return ret;
}

static void UpdateAllProcessScanTime(ThreadCtx *ctx)
{
    int ret;
    struct ProcessManager *manager = ctx->processManager;

    EnvMutexLock(&manager->lock);
    for (ProcessAttr *current = manager->processes; current; current = current->next) {
        if (current->isFirstScan) {
            continue;
        }
        if (current->scanType == NORMAL_SCAN) {
            ret = UpdateScanTime(current);
            if (ret) {
                SMAP_LOGGER_WARNING("Update pid %d scan cycle failed, ret=%d.", current->pid, ret);
            }
        }
    }
    EnvMutexUnlock(&manager->lock);
}

static void RestoreProcessScanTime(ThreadCtx *ctx)
{
    struct ProcessManager *manager = ctx->processManager;

    EnvMutexLock(&manager->lock);
    for (ProcessAttr *current = manager->processes; current; current = current->next) {
        if (current->isFirstScan) {
            if (current->walkPage.nrPage == 0) {
                current->scanTime = DEFAULT_SCAN_PERIOD;
                SMAP_LOGGER_INFO("Skip pid %d scan cycle restore, nrPages=0, keep high-freq scan.", current->pid);
                continue;
            }
            if (current->scanType == NORMAL_SCAN) {
                HandleHighScan(current);
            }
        }
    }
    EnvMutexUnlock(&manager->lock);
}

static void UpdatePeriodFromConfig(ThreadCtx *ctx)
{
    if (!GetFileConfSwitchConfig()) {
        return;
    }

    RestoreProcessScanTime(ctx);
    if (GetMigratePeriodChanged()) {
        SMAP_LOGGER_INFO("Start update migrate period time from config to %u.", GetMigratePeriodConfig());
        ctx->period = GetMigratePeriodConfig();
        SetMigratePeriodChanged(false);
    }

    // 为了计算ntimes需要先更新迁移周期
    if (GetScanPeriodChanged()) {
        SMAP_LOGGER_INFO("Start update scan period time from config\n");
        UpdateAllProcessScanTime(ctx);
        SetScanPeriodChanged(false);
    }
}

static void MigrationUpdateMigrateModeAndScanCpu(void)
{
    if (GetMigrateModeEnableConfig()) {
        if (GetMigrateModeChanged()) {
            IoctlUpdateUbDmaAvail(GetMigrateModeConfig());
            SMAP_LOGGER_INFO("Start update migrate mode from config to %u.", GetMigrateModeConfig());
            SetMigrateModeChanged(false);
        }
    }

    if (GetScanCpuEnableConfig()) {
        if (GetScanCpuChanged()) {
            IoctlSetScanCpuRange(GetScanCpuMinConfig(), GetScanCpuMaxConfig());
            SMAP_LOGGER_INFO("Start update scan cpu (%u-%u).", GetScanCpuMinConfig(), GetScanCpuMaxConfig());
            SetScanCpuChanged(false);
        }
    }
}

// 无管理进程时仅在tracking仍enable时关闭一次，之后直接跳过整个周期，
// 避免内核空转扫描和重复disable；新进程加入后下一周期恢复正常流程
static bool SkipCycleIfNoProcess(struct ProcessManager *manager, int *ret)
{
    if (manager->processes) {
        return false;
    }
    if (manager->tracking.trackingEnabled) {
        SMAP_LOGGER_DEBUG("No managed process, disable tracking and skip cycle.");
        *ret = DisableTracking(manager);
        if (*ret) {
            SMAP_LOGGER_ERROR("Disable tracking failed! ret:%d.", *ret);
        }
    }
    return true;
}

// 管理线程函数
int ScanMigrateWork(ThreadCtx *ctx)
{
    int ret = 0;
    struct ProcessManager *manager = ctx->processManager;

    if (SkipCycleIfNoProcess(manager, &ret)) {
        return ret;
    }

    ret = DisableTracking(manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Disable tracking failed! ret:%d.", ret);
        goto out;
    }
    SMAP_LOGGER_DEBUG("Tracking disabled.");
    StrategyConfigRead(STRATEGY_CONFIG_PATH); // 从配置文件中读取策略配置
    manager->ubBwMonitor.ubBwThreshold = GetUbBwThresholdConfig();
    if (GetFileConfSwitchConfig()) {
        SetAdaptMem(GetAdaptiveRatioEnableConfig());
    }
    // 由于进程销毁是异步，后续涉及ProcessAttr需要合理处理异常
    CheckAndRemoveInvalidProcess();
    ret = PerformMigrationPreparation(manager);
    if (ret) {
        SMAP_LOGGER_DEBUG("Migration preparation failed: %d.", ret);
        goto out;
    }

    GetUbFluxMb();

    // 更新虚机所处的场景
    UpdateScene(manager);
    SMAP_LOGGER_DEBUG("Scene updated.");
    // 根据内存使用情况更新配比
    ConfigRatios(manager);
    SMAP_LOGGER_DEBUG("Ratio configured.");
    // 处理迁移参数
    MigrationUpdateMigrateModeAndScanCpu();
    if (GetFileConfSwitchConfig()) {
        SMAP_LOGGER_DEBUG("Updating period from config.");
        UpdatePeriodFromConfig(ctx);
    } else {
        ret = HandleScene(ctx);
        if (ret) {
            SMAP_LOGGER_ERROR("Handle scene failed! ret:%d.", ret);
            goto out;
        }
        SMAP_LOGGER_DEBUG("Handle scene done.");
    }
    UpdateRemoteNumaCriticalErr();
    ret = PerformMigration(manager);
    SMAP_LOGGER_INFO("Migration result: %d.", ret);
    // 迁移结束后：仅在开启带宽限制时配置ub_watch开启统计（下周期查询时得到纯业务带宽）
    if (IsBwMonitorEnabled(manager)) {
        ConfigUbWatch(ctx->period);
    }
out:
    RefreshRemoteRam(manager);
    // 启动扫描
    EnableTracking(manager);
    SMAP_LOGGER_DEBUG("Tracking enabled.");
    return ret;
}

int MigrateRemoteNuma(struct ProcessManager *manager, struct MigrateNumaIoctlMsg *msg)
{
    SMAP_LOGGER_INFO("src %d, dest %d, addr count %d\n", msg->srcNid, msg->destNid, msg->count);
    for (int i = 0; i < msg->count; i++) {
        SMAP_LOGGER_INFO("memid[%d] %llu\n", i, msg->memids[i]);
    }

    int ret = ioctl(manager->fds.migrate, SMAP_MIG_MIGRATE_NUMA, msg);
    SMAP_LOGGER_INFO("migrate numa ioctl ret: %d.", ret);
    return ret ? -ENOMEM : 0;
}
