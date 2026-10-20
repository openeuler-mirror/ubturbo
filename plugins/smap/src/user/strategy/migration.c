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
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/time.h>
#include <math.h>
#include "manage/access_ioctl.h"
#include "smap_user_log.h"
#include "advanced-strategy/scene.h"
#include "manage/manage.h"
#include "manage/manage_internal.h"
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

static int AddMigListCapped(struct MigrateMsg *mMsg, struct MigList *mList, int cap)
{
    int i;
    if (!mMsg || !mList) {
        return 0;
    }
    if (mList->nr == 0 || mList->addr == NULL) {
        return 0;
    }
    if (cap <= 0 || mMsg->cnt >= cap) {
        SMAP_LOGGER_WARNING("Migration list reaches kernel limit %d.", cap);
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

int AddMigList(struct MigrateMsg *mMsg, struct MigList *mList)
{
    return AddMigListCapped(mMsg, mList, GetCurrentMaxNrPid() * MAX_PER_PID_MIG_LIST_COUNT);
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

    for (size_t i = 0; i < planCnt; i++) {
        int budgetIndex = FindPairPidBudget(pidBudgets, pidBudgetCnt, plans[i].pid);
        ProcessAttr *process = GetProcessAttr(plans[i].pid);
        bool invalid = (budgetIndex < 0 || !process || process->groupPolicy.enabled || plans[i].localNid < 0 ||
                        plans[i].localNid >= context->nrLocalNuma || plans[i].remoteNid < context->nrLocalNuma ||
                        plans[i].remoteNid >= MAX_NODES || !IsPairPlanDirectionValid(&plans[i]) ||
                        pidBudgets[budgetIndex].plannedPages > pidBudgets[budgetIndex].maxMigratePages);
        PutProcessAttr(process);
        if (invalid) {
            return -EINVAL;
        }
    }

    pid_t currentPid = -1;
    uint64_t selectedFrom[MAX_NODES] = { 0 };
    for (size_t i = 0; i < planCnt; i++) {
        PairPlan *plan = &plans[i];
        ProcessAttr *process = GetProcessAttr(plan->pid);
        int budgetIndex = FindPairPidBudget(pidBudgets, pidBudgetCnt, plan->pid);
        PairPidBudget *budget = &pidBudgets[budgetIndex];
        plan->swapPages = 0;

        if (currentPid != plan->pid) {
            currentPid = plan->pid;
            int ret = memset_s(selectedFrom, sizeof(selectedFrom), 0, sizeof(selectedFrom));
            if (ret != EOK) {
                PutProcessAttr(process);
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
            PutProcessAttr(process);
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
        PutProcessAttr(process);
    }
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
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t procCnt = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
    if (manager->nrLocalNuma <= 0 || manager->nrLocalNuma > LOCAL_NUMA_NUM) {
        ret = -EINVAL;
        goto OUT;
    }
    for (size_t k = 0; k < procCnt; k++) {
        ProcessAttr *process = all[k]->attr;
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
    PidSlotReleaseRefs(all, procCnt);
    free(stages);
    return ret;
}

/* 为迁移中的非 grouped PID 规划 pair；plans 为 NULL 时只写回进程状态。 */
int BuildAllPairPlans(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt)
{
    if (!manager || planCap > MAX_PAIR_TARGET_COUNT) {
        return -EINVAL;
    }
    if (planCnt) {
        *planCnt = 0;
    }

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
    hugePage = manager->tracking.pageSize == PAGESIZE_2M;
    nrLocalNuma = manager->nrLocalNuma;

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
        if (plans && stagedPlanCnt > 0) {
            ret = memcpy_s(plans, planCap * sizeof(PairPlan), stagedPlans, stagedPlanCnt * sizeof(PairPlan));
            if (ret != EOK) {
                ret = -ret;
            }
        }
        if (!ret && planCnt) {
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
    int nrLocalNuma = GetNrLocalNuma();
    /* Group mig_list entries per local node: for each local L, emit its back
       (remote->L) and out (L->remote) entries together so the kernel's
       first-active scan co-schedules migrate-back into L with migrate-out
       from L, avoiding a local node being filled without being freed. */
    for (int L = 0; L < nrLocalNuma; L++) {
        for (int R = nrLocalNuma; R < nrLocalNuma + REMOTE_NUMA_NUM; R++) {
            struct MigList *cells[2] = { &migList[R][L], &migList[L][R] }; /* back into L, out from L */
            for (int c = 0; c < 2; c++) {
                if (!cells[c]->nr) {
                    continue;
                }
                ret = AddMigListCapped(mMsg, cells[c], MAX_PER_PID_MIG_LIST_COUNT);
                if (ret == -ENOSPC) {
                    SMAP_LOGGER_WARNING("Pid %d migration list is deferred by kernel entry limit.", process->pid);
                    FreeMigList(migList);
                    *migratePage += nrMigTotal;
                    return 0;
                }
                if (ret) {
                    SMAP_LOGGER_ERROR("Pid %d AddMigList %d %d failed: %d.", process->pid, cells[c]->from, cells[c]->to,
                                      ret);
                    FreeMigList(migList);
                    return ret;
                }
                nrMigTotal += cells[c]->nr;
                SMAP_LOGGER_INFO("Numa %d --> Numa %d, mig %d pages.", cells[c]->from, cells[c]->to, cells[c]->nr);
            }
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

static void UpdateMigResultFrom(struct MigrateMsg *mMsg, int startIdx, struct ProcessManager *manager)
{
    ProcessAttr *current;
    uint64_t successMigCount;
    int fromNid;
    int toNid;
    for (int i = startIdx; i < mMsg->cnt; i++) {
        current = GetProcessAttr(mMsg->migList[i].pid);
        if (!current || !mMsg->migList[i].successToUser) {
            PutProcessAttr(current);
            continue;
        }

        fromNid = mMsg->migList[i].from;
        toNid = mMsg->migList[i].to;
        successMigCount = GetMigListSuccessPages(&mMsg->migList[i]);

        if (current->groupPolicy.enabled) {
            UpdateGroupedMigrationResult(current, fromNid, toNid, successMigCount);
            PutProcessAttr(current);
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
            PutProcessAttr(current);
            continue;
        }
        SMAP_LOGGER_INFO("pid %d from %d to %d nr %llu failed_mig_nr %llu "
                         "failed_isolated_nr %llu success_mig_nr %llu.",
                         mMsg->migList[i].pid, mMsg->migList[i].from, mMsg->migList[i].to, mMsg->migList[i].nr,
                         mMsg->migList[i].failedMigNr, mMsg->migList[i].failedIsolatedNr, successMigCount);
        PutProcessAttr(current);
    }
}

void UpdateMigResult(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    UpdateMigResultFrom(mMsg, 0, manager);
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
    (void)manager;
    ProcessAttr *process = GetProcessAttr(pid);
    if (!process || !process->groupPolicy.enabled) {
        PutProcessAttr(process);
        return;
    }
    ResetGroupedSwapRuntimeLocked(process, true);
    SMAP_LOGGER_ERROR("grouped pid %d swap frozen due to unbalanced swap migration.", pid);
    PutProcessAttr(process);
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
        ProcessAttr *process = GetProcessAttr(list->pid);
        if (!process || !process->groupPolicy.enabled) {
            PutProcessAttr(process);
            continue;
        }
        bool localToRemote = list->from < manager->nrLocalNuma && list->to >= manager->nrLocalNuma;
        bool remoteToLocal = list->from >= manager->nrLocalNuma && list->to < manager->nrLocalNuma;
        if (!localToRemote && !remoteToLocal) {
            PutProcessAttr(process);
            continue;
        }
        int localNid = localToRemote ? list->from : list->to;
        int remoteNid = localToRemote ? list->to : list->from;
        int idx = AddGroupSwapPairStat(stats, &statCnt, MAX_GROUP_SWAP_COMP_PLANS, list->pid, localNid, remoteNid);
        if (idx < 0) {
            SMAP_LOGGER_ERROR("grouped pid %d swap compensation stat exceeds limit.", list->pid);
            PutProcessAttr(process);
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
        PutProcessAttr(process);
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

/* 汇总迁移结果供 grouped swap 补偿使用；dst 不接管 addr。 */
static int AppendCycleMigResult(struct MigrateMsg *dst, const struct MigrateMsg *src)
{
    if (!dst || !src || src->cnt == 0) {
        return 0;
    }
    int oldCnt = dst->cnt;
    int newCnt = oldCnt + src->cnt;
    struct MigList *newList = malloc(sizeof(struct MigList) * newCnt);
    if (!newList) {
        return -ENOMEM;
    }
    for (int i = 0; i < oldCnt; i++) {
        newList[i] = dst->migList[i];
    }
    for (int i = 0; i < src->cnt; i++) {
        newList[oldCnt + i] = src->migList[i];
        newList[oldCnt + i].addr = NULL;
    }
    free(dst->migList);
    dst->migList = newList;
    dst->cnt = newCnt;
    return 0;
}

static uint64_t BuildCompMigListFromScan(ProcessAttr *process, GroupSwapCompPlan *plan, struct MigList *list)
{
    uint64_t sourcePages = process->scanAttr.actcLen[plan->from];
    uint64_t destFreePages = IsHugeMode() ? GetNrFreeHugePagesByNode(plan->to) : GetNrFreePagesByNode(plan->to);
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
    compMsg->pageSize = manager->tracking.pageSize;

    for (int i = 0; i < planCnt; i++) {
        ProcessAttr *process = GetProcessAttr(plans[i].pid);
        if (!process || !process->groupPolicy.enabled) {
            plans[i].shouldFreeze = true;
            PutProcessAttr(process);
            continue;
        }
        struct MigList *list = &compMsg->migList[compMsg->cnt];
        uint64_t nr = BuildCompMigListFromScan(process, &plans[i], list);
        if (nr == 0) {
            PutProcessAttr(process);
            continue;
        }
        list->pid = plans[i].pid;
        list->from = plans[i].from;
        list->to = plans[i].to;
        compPlanIdx[compMsg->cnt] = i;
        compMsg->cnt++;
        PutProcessAttr(process);
    }
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
    for (int i = 0; i < planCnt; i++) {
        if (plans[i].shouldFreeze) {
            FreezeGroupedSwapLocked(manager, plans[i].pid);
        }
    }
}

static int RunGroupedSwapCompensation(struct ProcessManager *manager, struct MigrateMsg *mMsg,
                                      GroupSwapCompPlan plans[], int planCnt, ProcessAttr *process)
{
    struct MigrateMsg compMsg = { 0 };
    int compPlanIdx[MAX_GROUP_SWAP_COMP_PLANS] = { 0 };
    /* per-pid 事件流水传 process 仅刷新该 pid；轮询模式传 NULL 刷新全部 pmмоеп */
    int ret = process ? BuildPidData(process) : RefreshAllPidData();

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
    int appendRet = AppendCycleMigResult(mMsg, &compMsg);
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

static int DecideThreadNum(struct MigrateMsg *mMsg)
{
    uint64_t total = 0;
    bool forcedSingle = false;
    int nThread = 0;

    if (mMsg->pageSize != (int)GetHugePageSize()) {
        return SIG_THREAD_MIG_OUT;
    }

    for (int i = 0; i < mMsg->cnt; i++) {
        total += mMsg->migList[i].nr;
    }
    if (total == 0 || total <= LESS_MIG_OUT_HUGE_PAGE_THRE) {
        SMAP_LOGGER_INFO("As for %llu 2M pages, set 1 migration thread.", (unsigned long long)total);
        return SIG_THREAD_MIG_OUT;
    }

    /* SHARED mmap 的 pid 强制单线程；检查范围为本消息内的 pid（per-pid 消息即当前 pid） */
    for (int i = 0; i < mMsg->cnt && !forcedSingle; i++) {
        ProcessAttr *attr = GetProcessAttr(mMsg->migList[i].pid);
        forcedSingle = attr && attr->vmPidAttr.mmapType == MMAP_SHARED;
        PutProcessAttr(attr);
    }
    if (forcedSingle) {
        SMAP_LOGGER_INFO("Forced single thread detected (SHARED mmap), set 1 migration thread.");
        return SIG_THREAD_MIG_OUT;
    }
    nThread = (total <= MORE_MIG_OUT_HUGE_PAGE_THRE) ? LESS_THREAD_MIG_OUT : MORE_THREAD_MIG_OUT;
    SMAP_LOGGER_INFO("As for %llu 2M pages, set %d migration threads.", (unsigned long long)total, nThread);
    return nThread;
}

static void *SubMigrateThreadFn(void *arg)
{
    struct SubMigrateCtx *ctx = arg;
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    for (uint32_t c = ctx->cpuMin; c <= ctx->cpuMax; c++) {
        CPU_SET((int)c, &cpuset);
    }
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        SMAP_LOGGER_WARNING("migrate thread setaffinity (%u-%u) failed.", ctx->cpuMin, ctx->cpuMax);
    }
    ctx->ret = (ctx->msg.cnt > 0) ? ioctl(ctx->fd, SMAP_MIG_MIGRATE, &ctx->msg) : 0;
    return NULL;
}

static void FreeSubMigrateMsgs(struct SubMigrateCtx *subs, int nrThreads)
{
    for (int i = 0; i < nrThreads; i++) {
        free(subs[i].msg.migList);
        free(subs[i].origIdx);
    }
}

static int BuildSubMigrateMsgs(struct MigrateMsg *mMsg, int nrThreads, struct SubMigrateCtx *subs)
{
    if (nrThreads <= 0) {
        return -EINVAL;
    }
    for (int i = 0; i < nrThreads; i++) {
        subs[i].msg.cnt = 0;
        subs[i].msg.pageSize = mMsg->pageSize;
        subs[i].msg.migList = calloc(mMsg->cnt, sizeof(struct MigList));
        subs[i].origIdx = calloc(mMsg->cnt, sizeof(int));
        if (!subs[i].msg.migList || !subs[i].origIdx) {
            FreeSubMigrateMsgs(subs, nrThreads);
            return -ENOMEM;
        }
    }
    for (int j = 0; j < mMsg->cnt; j++) {
        uint64_t nr = mMsg->migList[j].nr;
        if (nr == 0) {
            continue;
        }
        uint64_t chunk = nr / nrThreads;
        int rem = (int)(nr % nrThreads);
        uint64_t offset = 0;
        for (int i = 0; i < nrThreads; i++) {
            uint64_t subNr = chunk + (i < rem ? 1ULL : 0ULL);
            if (subNr == 0) {
                continue;
            }
            int k = subs[i].msg.cnt;
            subs[i].msg.migList[k].pid = mMsg->migList[j].pid;
            subs[i].msg.migList[k].from = mMsg->migList[j].from;
            subs[i].msg.migList[k].to = mMsg->migList[j].to;
            subs[i].msg.migList[k].nr = subNr;
            subs[i].msg.migList[k].addr = mMsg->migList[j].addr + offset;
            subs[i].msg.migList[k].successToUser = false;
            subs[i].msg.migList[k].failedMigNr = 0;
            subs[i].msg.migList[k].failedIsolatedNr = 0;
            subs[i].origIdx[k] = j;
            subs[i].msg.cnt++;
            offset += subNr;
        }
    }
    return 0;
}

static void AggregateSubMigResults(struct SubMigrateCtx *subs, int nrThreads, struct MigrateMsg *mMsg)
{
    for (int i = 0; i < nrThreads; i++) {
        for (int k = 0; k < subs[i].msg.cnt; k++) {
            int orig = subs[i].origIdx[k];
            if (subs[i].ret < 0) {
                /* ioctl整体失败（如全局状态竞争-EAGAIN）时内核不回填统计字段，
                 * 子条目仍为初始值0，若直接聚合会把下发数虚报为成功数，
                 * 全部计为隔离失败，保证successMigCount真实 */
                mMsg->migList[orig].failedIsolatedNr += subs[i].msg.migList[k].nr;
            } else {
                mMsg->migList[orig].failedMigNr += subs[i].msg.migList[k].failedMigNr;
                mMsg->migList[orig].failedIsolatedNr += subs[i].msg.migList[k].failedIsolatedNr;
            }
            if (subs[i].msg.migList[k].successToUser) {
                mMsg->migList[orig].successToUser = true;
            }
        }
    }
}

/* 汇总子线程返回值：任一线程ioctl整体失败（负errno）即返回首个错误码，
 * 全部ioctl成功返回0。部分失败页数（正值）不进入返回值，
 * 实际迁移成功/失败量由UpdateMigResult日志承载 */
static int AggregateSubThreadRet(const struct SubMigrateCtx *subs, int nrThreads)
{
    for (int i = 0; i < nrThreads; i++) {
        if (subs[i].ret < 0) {
            return subs[i].ret;
        }
    }
    return 0;
}

static int RunMultiThreadedMigrate(struct MigrateMsg *mMsg, struct ProcessManager *manager, int nrThreads)
{
    int err = 0;
    struct SubMigrateCtx *subs = NULL;
    pthread_t *tids = NULL;
    int buildRet = 0;

    if (nrThreads <= SIG_THREAD_MIG_OUT || nrThreads > MORE_THREAD_MIG_OUT) {
        return -EINVAL;
    }

    subs = calloc(nrThreads, sizeof(struct SubMigrateCtx));
    tids = calloc(nrThreads, sizeof(pthread_t));
    if (!subs || !tids) {
        FreeSubMigrateMsgs(subs, nrThreads);
        free(subs);
        free(tids);
        return -ENOMEM;
    }
    buildRet = BuildSubMigrateMsgs(mMsg, nrThreads, subs);
    if (buildRet) {
        FreeSubMigrateMsgs(subs, nrThreads);
        free(subs);
        free(tids);
        return buildRet;
    }
    for (int i = 0; i < nrThreads; i++) {
        subs[i].fd = manager->fds.migrate;
        subs[i].ret = 0;
        subs[i].cpuMin = GetScanCpuMinConfig();
        subs[i].cpuMax = GetScanCpuMaxConfig();
        int pr = pthread_create(&tids[i], NULL, SubMigrateThreadFn, &subs[i]);
        if (pr != 0) {
            SMAP_LOGGER_ERROR("pthread_create %d failed: %d.", i, pr);
            subs[i].ret = -EFAULT;
        }
    }
    for (int i = 0; i < nrThreads; i++) {
        if (tids[i] != 0) {
            pthread_join(tids[i], NULL);
        }
    }
    err = AggregateSubThreadRet(subs, nrThreads);
    AggregateSubMigResults(subs, nrThreads, mMsg);
    FreeSubMigrateMsgs(subs, nrThreads);
    free(subs);
    free(tids);
    return err;
}

int DoMigration(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    int err = 0;
    int nThread = 0;
    SMAP_LOGGER_DEBUG("mMsg->cnt %d.", mMsg->cnt);
    uint64_t **tmpAddr = malloc(sizeof(*tmpAddr) * mMsg->cnt);
    if (!tmpAddr) {
        SMAP_LOGGER_ERROR("malloc tmp addr failed.");
        return -ENOMEM;
    }
    for (int i = 0; i < mMsg->cnt; i++) {
        tmpAddr[i] = mMsg->migList[i].addr;
    }

    nThread = DecideThreadNum(mMsg);
    if (nThread == SIG_THREAD_MIG_OUT) {
        err = (mMsg->cnt > 0) ? ioctl(manager->fds.migrate, SMAP_MIG_MIGRATE, mMsg) : 0;
    } else {
        err = RunMultiThreadedMigrate(mMsg, manager, nThread);
    }
    if (err < 0) {
        SMAP_LOGGER_ERROR("DoMigration ioctl failed, ret: %d.", err);
    }

    for (int i = 0; i < mMsg->cnt; i++) {
        if (tmpAddr[i]) {
            free(tmpAddr[i]);
            mMsg->migList[i].addr = NULL;
        }
    }
    free(tmpAddr);
    return err;
}

static int InitPidMigrateMsg(struct MigrateMsg *mMsg, struct ProcessManager *manager)
{
    // per-pid迁移消息：容量与内核单pid条目上限对齐
    mMsg->cnt = 0;
    mMsg->migList = calloc(MAX_PER_PID_MIG_LIST_COUNT, sizeof(struct MigList));
    if (!mMsg->migList) {
        SMAP_LOGGER_ERROR("mMsg->migList malloc failed.");
        return -ENOMEM;
    }
    mMsg->pageSize = manager->tracking.pageSize;
    return 0;
}

static void FreePidMigrateMsg(struct MigrateMsg *mMsg)
{
    if (!mMsg || !mMsg->migList) {
        return;
    }
    for (int i = 0; i < mMsg->cnt; i++) {
        free(mMsg->migList[i].addr);
        mMsg->migList[i].addr = NULL;
    }
    free(mMsg->migList);
    mMsg->migList = NULL;
    mMsg->cnt = 0;
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
        if (nid < 0 || nid >= MAX_NODES) {
            SMAP_LOGGER_ERROR("UB BW: invalid numa id %d, skip.", nid);
            continue;
        }
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

static int UpdateScanTime(ProcessAttr *process)
{
    struct AccessAddPidPayload payload;
    payload.pid = process->pid;
    payload.numaNodes = process->numaAttr.numaNodes;
    payload.type = process->scanType;
    payload.duration = GetFileConfSwitchConfig() ? GetMigratePeriodConfig() : process->sceneInfo.cycles.migCycle;
    payload.pidType = process->type;
    payload.scanTime = GetFileConfSwitchConfig() ? GetScanPeriodConfig() : process->sceneInfo.cycles.scanCycle;
    int ret = AccessIoctlAddPid(1, &payload);
    if (ret) {
        SMAP_LOGGER_ERROR("Update scan time failed for pid %d, ret=%d.", process->pid, ret);
    } else {
        process->scanTime = payload.scanTime;
        process->duration = payload.duration;
        SMAP_LOGGER_INFO("Update pid %d scan cycle to %dms.", process->pid, payload.scanTime);
    }
    return ret;
}

static void UpdateScenePerPid(ProcessAttr *process)
{
    if (process->scanType == NORMAL_SCAN) {
        SetProcessSceneAttr(process);
    }
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

static int HandleScenePerPid(ProcessAttr *current)
{
    PageType pageType = IsHugeMode() ? PAGETYPE_HUGE : PAGETYPE_NORMAL;
    SceneInfo *info = &current->sceneInfo;

    GetProcessSceneAttr(info->currScene, info, pageType);
    if (current->isFirstScan) {
        if (current->walkPage.nrPage == 0) {
            current->scanTime = DEFAULT_SCAN_PERIOD;
            return 0;
        }
        HandleHighScan(current);
        return 0;
    }
    if (info->currScene != info->lastScene && UpdateScanTime(current)) {
        SMAP_LOGGER_WARNING("Update scan time failed for pid %d.", current->pid);
    }
    return 0;
}

static void RestoreProcessScanTimePerPid(ProcessAttr *process)
{
    if (!process->isFirstScan) {
        return;
    }
    if (process->walkPage.nrPage == 0) {
        process->scanTime = DEFAULT_SCAN_PERIOD;
        SMAP_LOGGER_INFO("Skip pid %d scan cycle restore, nrPages=0, keep high-freq scan.", process->pid);
        return;
    }
    if (process->scanType == NORMAL_SCAN) {
        HandleHighScan(process);
    }
}

static void UpdatePeriodFromConfigPerPid(ProcessAttr *process)
{
    RestoreProcessScanTimePerPid(process);
    if (!process->isFirstScan &&
        (process->scanTime != GetScanPeriodConfig() || process->duration != GetMigratePeriodConfig())) {
        if (UpdateScanTime(process)) {
            SMAP_LOGGER_WARNING("Update pid %d scan and migrate periods from config failed.", process->pid);
        }
    }
}

/* 批量接口（供周期维护旧口径调用）：对全部受管 pid 逐个执行 per-pid 处理 */
static void RestoreProcessScanTime(struct ProcessManager *manager);
static void UpdatePeriodFromConfig(struct ProcessManager *manager);
static void UpdateScene(struct ProcessManager *manager)
{
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        if (all[k]->attr) {
            UpdateScenePerPid(all[k]->attr);
        }
    }
    PidSlotReleaseRefs(all, n);
}

static int HandleScene(struct ProcessManager *manager)
{
    int ret = 0;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n && ret == 0; k++) {
        if (all[k]->attr) {
            ret = HandleScenePerPid(all[k]->attr);
        }
    }
    PidSlotReleaseRefs(all, n);
    return ret;
}

static void UpdatePeriodFromConfig(struct ProcessManager *manager)
{
    if (!GetFileConfSwitchConfig()) {
        return;
    }
    RestoreProcessScanTime(manager);
    if (GetMigratePeriodChanged()) {
        SMAP_LOGGER_INFO("Start update migrate period time from config to %u.", GetMigratePeriodConfig());
        manager->migPeriod = GetMigratePeriodConfig();
        SetMigratePeriodChanged(false);
    }
    if (GetScanPeriodChanged()) {
        SMAP_LOGGER_INFO("Start update scan period time from config");
        struct PidSlot *all[MAX_PID_SLOTS];
        size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
        for (size_t k = 0; k < n; k++) {
            if (all[k]->attr) {
                UpdatePeriodFromConfigPerPid(all[k]->attr);
            }
        }
        PidSlotReleaseRefs(all, n);
        SetScanPeriodChanged(false);
    }
}

static void RestoreProcessScanTime(struct ProcessManager *manager)
{
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        if (all[k]->attr) {
            RestoreProcessScanTimePerPid(all[k]->attr);
        }
    }
    PidSlotReleaseRefs(all, n);
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

    if (GetScanCpuChanged()) {
        IoctlSetScanCpuRange(GetScanCpuMinConfig(), GetScanCpuMaxConfig());
        SMAP_LOGGER_INFO("Start update scan cpu (%u-%u).", GetScanCpuMinConfig(), GetScanCpuMaxConfig());
        SetScanCpuChanged(false);
    }

    if (GetRemoteScanModeChanged()) {
        IoctlSetRemoteScanMode(GetRemoteScanModeConfig());
        SetRemoteScanModeChanged(false);
    }
}
/* 每个扫描完成事件触发一次 PID 迁移流水；daemon 负责周期维护。 */

/* 迁移完成后重启该 pid 的下一个扫描窗口（TRACKING_CMD 复用 arg=pid 语义）；
 * -ENOENT 表示 pid 已不被内核跟踪（正常结束路径）。 */
static int RestartPidScan(pid_t pid)
{
    return AccessIoctlRestartPid(pid);
}

/* 仅按新增远端需求裁剪目标页数：先用 private 容量，再用 shared 容量。 */
static void ClipPidTargetsToCapacity(ProcessAttr *process, PairTarget targets[], size_t targetCnt, int nrLocalNuma,
                                     uint64_t privateAvail[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                     uint64_t sharedAvail[REMOTE_NUMA_NUM])
{
    if (!process || process->ignoreRemoteCapacity) {
        return;
    }
    for (size_t i = 0; i < targetCnt; i++) {
        int remote = targets[i].remoteNid - nrLocalNuma;
        int local = targets[i].localNid;
        uint64_t baseline = process->strategyAttr.remoteNrPagesAfterMigrate[local][remote];
        if (targets[i].requestedPages <= baseline) {
            continue;
        }
        uint64_t demand = targets[i].requestedPages - baseline;
        uint64_t privateTaken = (uint64_t)MIN(demand, privateAvail[local][remote]);
        uint64_t sharedTaken = MIN(demand - privateTaken, sharedAvail[remote]);

        targets[i].requestedPages = (uint32_t)(baseline + privateTaken + sharedTaken);
        privateAvail[local][remote] -= privateTaken;
        sharedAvail[remote] -= sharedTaken;
    }
}

/* 在远端账本锁内裁剪计划并预扣在途页数。 */
static int BuildPidPairPlans(struct ProcessManager *manager, ProcessAttr *process)
{
    PairRequestContext requestContext = { 0 };
    PairTarget targets[LOCAL_NUMA_NUM * REMOTE_NUMA_NUM] = { 0 };
    PairPlan plans[LOCAL_NUMA_NUM * REMOTE_NUMA_NUM] = { 0 };
    PairRequestSummary summary = { 0 };
    uint32_t nrMigratePages[MAX_NODES][MAX_NODES] = { 0 };
    uint64_t privateAvail[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { { 0 } };
    uint64_t sharedAvail[REMOTE_NUMA_NUM] = { 0 };
    uint64_t selectedFrom[MAX_NODES] = { 0 };
    uint64_t plannedPages = 0;
    size_t targetCnt = 0;
    int ret;

    if (!process || process->groupPolicy.enabled || manager->nrLocalNuma <= 0 ||
        manager->nrLocalNuma > LOCAL_NUMA_NUM || manager->tracking.pageSize == 0) {
        return -EINVAL;
    }
    requestContext.nrLocalNuma = manager->nrLocalNuma;
    requestContext.pageSizeKB = manager->tracking.pageSize / KIB;
    BuildPairCapacityLocalMasks(manager, process, &requestContext);
    ret = BuildPairRequestedTargets(process, &requestContext, targets, LOCAL_NUMA_NUM * REMOTE_NUMA_NUM, &targetCnt,
                                    &summary);
    if (ret) {
        return ret;
    }

    /* ---------- 在线账本临界区：快照 → 剪辑 → 规划 → 预扣 ---------- */
    EnvMutexLock(&manager->remoteNumaInfo.lock);
    SnapshotRemoteCapacityLocked(privateAvail, sharedAvail);
    ClipPidTargetsToCapacity(process, targets, targetCnt, manager->nrLocalNuma, privateAvail, sharedAvail);

    for (size_t i = 0; i < targetCnt; i++) {
        PairPlan *plan = &plans[i];
        uint64_t requestedPages;

        if (IsNodeForbidden(targets[i].remoteNid)) {
            /* A disabled remote is frozen until it is enabled again. */
            continue;
        }
        plan->pid = process->pid;
        plan->localNid = targets[i].localNid;
        plan->remoteNid = targets[i].remoteNid;
        plan->remoteIndex = targets[i].remoteNid - manager->nrLocalNuma;
        plan->targetPages = targets[i].requestedPages;
        plan->actualPages = process->strategyAttr.remoteNrPagesAfterMigrate[plan->localNid][plan->remoteIndex];
        if (plan->actualPages < plan->targetPages) {
            requestedPages = plan->targetPages - plan->actualPages;
            plan->demotePages = (uint32_t)MIN(requestedPages, process->walkPage.nrPage - plannedPages);
        } else {
            requestedPages = plan->actualPages - plan->targetPages;
            plan->promotePages = (uint32_t)MIN(requestedPages, process->walkPage.nrPage - plannedPages);
        }
        plannedPages += plan->demotePages + plan->promotePages;
    }

    for (size_t i = 0; i < targetCnt && plannedPages < process->walkPage.nrPage; i++) {
        PairPlan *plan = &plans[i];
        uint64_t swapLimit = MIN((process->walkPage.nrPage - plannedPages) / 2, (uint64_t)UINT32_MAX);
        uint64_t swapPages;

        if (!process->enableSwap || IsNodeForbidden(plan->remoteNid) || !IsOnlyLocalForRemote(plans, targetCnt, i) ||
            plan->promotePages > 0 ||
            (plan->targetPages == 0 && plan->demotePages == 0 && plan->promotePages == 0)) {
            /* promote 方向（目标要求把远端页迁回本地）不做 swap：swap 会把本地
             * 冷页再送回远端，与本周期的 promote 目标反向抵消，远端清空目标
             * 永远无法达成，只能靠周期性长尾收敛（远端页数每周期仅减 swap 缺口）。 */
            continue;
        }
        swapPages = CountPairSwapPages(process, plan->localNid, plan->remoteNid, selectedFrom[plan->localNid],
                                       selectedFrom[plan->remoteNid]);
        swapPages = MIN(swapPages, swapLimit);
        swapPages += CalcPairRemoteGuaranteeExtraSwap(process, plan, selectedFrom[plan->localNid],
                                                      selectedFrom[plan->remoteNid], swapPages, swapLimit - swapPages);
        plan->swapPages = (uint32_t)swapPages;
        selectedFrom[plan->localNid] += swapPages;
        selectedFrom[plan->remoteNid] += swapPages;
        plannedPages += swapPages * 2;
    }

    for (size_t i = 0; i < targetCnt; i++) {
        nrMigratePages[plans[i].localNid][plans[i].remoteNid] = plans[i].demotePages + plans[i].swapPages;
        nrMigratePages[plans[i].remoteNid][plans[i].localNid] = plans[i].promotePages + plans[i].swapPages;
    }
    ret = memcpy_s(process->strategyAttr.nrMigratePages, sizeof(process->strategyAttr.nrMigratePages), nrMigratePages,
                   sizeof(nrMigratePages));
    if (ret == EOK) {
        /* 在途预扣：迁移执行之前把计划量挂进账本，结算按计划全额回收 */
        LedgerPlanInFlightLocked(nrMigratePages, manager->nrLocalNuma);
    } else {
        ret = -ret;
    }
    EnvMutexUnlock(&manager->remoteNumaInfo.lock);
    return ret;
}

/* per-pid 收尾：swap 补偿（单 pid 可重试）→ 结果即时结算 → 计划全额回收
 * 在途 → pending 发布 → 恢复 PROC_IDLE（持 attrLock，与候选切换对称）。 */
static void PostPidMigration(struct ProcessManager *manager, ProcessAttr *process, struct MigrateMsg *mMsg)
{
    GroupSwapCompPlan plans[MAX_GROUP_SWAP_COMP_PLANS] = { 0 };
    int planCnt = 0;
    int ret;

    ret = BuildGroupedSwapCompPlans(mMsg, manager, plans, &planCnt);
    if (!ret && planCnt > 0) {
        ret = RunGroupedSwapCompensation(manager, mMsg, plans, planCnt, process);
        if (ret) {
            SMAP_LOGGER_ERROR("Per-pid %d grouped swap compensation failed: %d.", process->pid, ret);
        }
    } else if (ret && process->groupPolicy.enabled) {
        ResetGroupedSwapRuntimeLocked(process, true);
    }

    UpdateMigResult(mMsg, manager);
    FreePidMigrateMsg(mMsg);
    /* 计划全额回收（成功部分待下一窗口 BuildPidData 刷新 attr 后由账本重建自然入账） */
    LedgerSettleInFlight(process->strategyAttr.nrMigratePages, manager->nrLocalNuma);

    if (ApplyPendingMigrationTargets(process)) {
        SMAP_LOGGER_ERROR("Apply pending migration target after pid %d migration failed.", process->pid);
    }
    if (ApplyPendingGroupedPolicy(process)) {
        SMAP_LOGGER_ERROR("Apply pending grouped policy after pid %d migration failed.", process->pid);
    }
}

/* 迁回等待期间不启动涉及 forbidden 远端节点的 PID 流水。 */
static bool IsPidAnyRemoteNodeForbidden(ProcessAttr *attr)
{
    int nrLocalNuma = GetNrLocalNuma();
    for (int remoteNid = nrLocalNuma; remoteNid < nrLocalNuma + REMOTE_NUMA_NUM; remoteNid++) {
        if (InAttrL2(attr, remoteNid) && IsNodeForbidden(remoteNid)) {
            return true;
        }
    }
    return false;
}

/* 新快照和账本均显示远端为空时自动移除；成功返回 true。 */
static bool SmapAutoRemovePidIfRemoteEmpty(ProcessAttr *attr)
{
    if (!attr || attr->state != PROC_IDLE || !attr->autoRemoveWhenRemoteEmpty || attr->syncWaitRemoteEmpty ||
        attr->groupPolicy.enabled || attr->scanType != NORMAL_SCAN) {
        return false;
    }
    if (attr->walkPage.nrPage == 0) {
        SMAP_LOGGER_INFO("pid %d page snapshot is empty, keep managed.", attr->pid);
        return false;
    }

    int nrLocalNuma = GetNrLocalNuma();
    uint64_t remoteSum = 0;
    for (int remoteIndex = 0; remoteIndex < REMOTE_NUMA_NUM; remoteIndex++) {
        int remoteNid = nrLocalNuma + remoteIndex;
        if (remoteNid >= MAX_NODES) {
            break;
        }
        remoteSum += attr->walkPage.nrPages[remoteNid];
    }
    uint64_t accountSum = 0;
    for (int r = 0; r < REMOTE_NUMA_NUM && nrLocalNuma > 0; r++) {
        for (int l = 0; l < nrLocalNuma && l < LOCAL_NUMA_NUM; l++) {
            accountSum += attr->strategyAttr.remoteNrPagesAfterMigrate[l][r];
        }
    }
    if (remoteSum != 0 || accountSum != 0) {
        return false;
    }

    SMAP_LOGGER_INFO("pid %d remote pages empty (%llu snapshot pages), auto remove from per-pid flow.", attr->pid,
                     attr->walkPage.nrPage);

    /* 先注销 mem_freq 监听，避免内核移除后 HUP 事件反复触发。 */
    struct PidSlot *slot = PidSlotGetRefByAttr(attr);
    if (slot) {
        EnvMutexLock(&slot->attrLock);
        EventLoopUnregisterSlotLocked(slot);
        EnvMutexUnlock(&slot->attrLock);
        PidSlotReleaseRefs(&slot, 1);
    }

    struct RemoveMsg msg = { 0 };
    msg.count = 1;
    msg.payload[0].pid = attr->pid;
    msg.payload[0].count = 0;

    int ret = IoctlRemoveProcess(&msg);
    if (ret) {
        SMAP_LOGGER_WARNING("per-pid auto remove ioctl for pid %d failed: %d.", attr->pid, ret);
        return false;
    }
    ClearManagedProcess(1, &msg.payload[0]);
    return true;
}

/* 执行单 PID 的扫描、规划和迁移；调用方须持有 slot 引用。 */
static int ProcessPidScanMigrate(struct ProcessManager *manager, struct PidSlot *slot)
{
    struct MigrateMsg msg = { 0 };
    ProcessAttr *attr = PidSlotAttr(slot);
    pid_t pid = slot->pid;
    uint64_t migratePages = 0;
    struct timeval start, end;
    int ret = 0;

    if (!attr || attr->scanType != NORMAL_SCAN || attr->state != PROC_IDLE) {
        goto enable;
    }
    ret = BuildPidData(attr);
    if (ret) {
        SMAP_LOGGER_ERROR("Build pid %d data failed: %d.", pid, ret);
        goto enable;
    }
    /* 已移除的 PID 不再重启扫描。 */
    if (SmapAutoRemovePidIfRemoteEmpty(attr)) {
        return 0;
    }
    if (GetFileConfSwitchConfig()) {
        UpdatePeriodFromConfigPerPid(attr);
    } else {
        UpdateScenePerPid(attr);
        ret = HandleScenePerPid(attr);
        if (ret) {
            goto enable;
        }
    }
    ApplyUbBwStop(attr, manager);
    /* 候选切换（attrLock 双检），与 API 线程状态读写序列化 */
    EnvMutexLock(&slot->attrLock);
    if (attr->state != PROC_IDLE) {
        EnvMutexUnlock(&slot->attrLock);
        goto enable;
    }
    if (IsPidAnyRemoteNodeForbidden(attr)) {
        EnvMutexUnlock(&slot->attrLock);
        goto enable;
    }
    attr->state = PROC_MIGRATE;
    EnvMutexUnlock(&slot->attrLock);

    if (attr->groupPolicy.enabled) {
        NumaMigReduceDeal(attr);
        ret = 0;
    } else {
        ret = BuildPidPairPlans(manager, attr);
    }
    if (ret) {
        goto settle;
    }
    ret = InitPidMigrateMsg(&msg, manager);
    if (ret) {
        goto settle;
    }
    ret = BuildMigrationMsg(attr, &msg, &migratePages);
    if (!ret && msg.cnt > 0) {
        gettimeofday(&start, NULL);
        ret = DoMigration(&msg, manager);
        gettimeofday(&end, NULL);
        PrintMigSpeed(manager, migratePages, start, end);
        SMAP_LOGGER_INFO("Pid %d migration pages %llu, result %d.", pid, migratePages, ret);
    }
settle:
    PostPidMigration(manager, attr, &msg);
    EnvMutexLock(&slot->attrLock);
    if (attr->state == PROC_MIGRATE) {
        attr->state = PROC_IDLE;
    }
    EnvMutexUnlock(&slot->attrLock);
enable:
    if (RestartPidScan(pid) && ret == 0) {
        ret = -EIO;
    }
    return ret;
}

/* 执行流水并归还事件循环转交的 slot 引用。 */
void PidMigrationWork(struct ProcessManager *manager, struct PidSlot *slot, pid_t pid)
{
    (void)ProcessPidScanMigrate(manager, slot);
    EventLoopResetEnqueued(slot);
    PidSlotReleaseRefs(&slot, 1);
}

/* 周期性维护配置、进程状态和远端容量；迁移由 PID worker 执行。 */
int ManagerDaemonWork(struct ProcessManager *manager)
{
    if (PidSlotEmpty(manager)) {
        return 0;
    }
    EventLoopEnsureRegistered(manager);
    StrategyConfigRead(STRATEGY_CONFIG_PATH); // 从配置文件中读取策略配置
    manager->ubBwMonitor.ubBwThreshold = GetUbBwThresholdConfig();
    if (GetFileConfSwitchConfig()) {
        SetAdaptMem(GetAdaptiveRatioEnableConfig());
    }
    // 由于进程销毁是异步，后续涉及ProcessAttr需要合理处理异常
    CheckAndRemoveInvalidProcess();
    CalcMigrateNrPagesPerPidMultiNuma();
    GetUbFluxMb();
    // 根据内存使用情况更新配比
    ConfigRatios(manager);
    SMAP_LOGGER_DEBUG("Ratio configured.");
    // 处理迁移参数
    MigrationUpdateMigrateModeAndScanCpu();
    UpdateRemoteNumaCriticalErr();
    RefreshRemoteRam(manager);
    // 迁移结束后：仅在开启带宽限制时配置ub_watch开启统计（下周期查询时得到纯业务带宽）
    if (IsBwMonitorEnabled(manager)) {
        ConfigUbWatch(manager->migPeriod);
    }
    return 0;
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
