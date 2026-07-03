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

#include <stdlib.h>
#include <errno.h>
#include <sys/param.h>
#include <math.h>
#include "securec.h"
#include "smap_user_log.h"
#include "manage/manage.h"
#include "strategy.h"
#include "strategy_config.h"
#include "separate_strategy.h"

#define DEFAULT_FREQ_WT_2M 1
#define DEFAULT_SLOW_THRED_2M (2 * DEFAULT_FREQ_WT_2M)
#define MAX_MIGRATE_DIVISOR_2M 1
#define MULTI_NUMA_VM_OOM 66
#define PRIOR_MAX_VALUE 63
typedef struct NumaInfo {
    int localNid;
    int remoteNid;
} NumaInfo;

static int InitSeparateParam(ProcessAttr *process)
{
    process->separateParam.freqWt = DEFAULT_FREQ_WT_2M;
    process->separateParam.slowThred = GetSlowThresholdConfig();
    SMAP_LOGGER_DEBUG("InitSeparateParam done, freqWt: %u, slowThred: %u.", process->separateParam.freqWt,
                      process->separateParam.slowThred);
}

static bool ShouldMigrate(ProcessAttr *process)
{
    if (process->separateParam.maxMigrate < 1) {
        SMAP_LOGGER_DEBUG("Pid %d maxMigrate is invalid, skip migrate.", process->pid);
        return false;
    }
    if (process->walkPage.nrPage < 1) {
        SMAP_LOGGER_DEBUG("Pid %d nrPage is invalid, skip migrate.", process->pid);
        return false;
    }
    return true;
}

static void FreeMlist(struct MigList mlist[MAX_NODES][MAX_NODES])
{
    for (int from = 0; from < MAX_NODES; from++) {
        for (int to = 0; to < MAX_NODES; to++) {
            if (mlist[from][to].addr) {
                free(mlist[from][to].addr);
                mlist[from][to].addr = NULL;
            }
            mlist[from][to].nr = 0;
        }
    }
}

static actc_t FindKthFreqAsc(const uint32_t *buckets, uint64_t k)
{
    if (k == 0) {
        return 0;
    }

    uint64_t countSoFar = 0;
    for (int freq = 0; freq < FREQ_BUCKETS_SIZE; freq++) {
        countSoFar += buckets[freq];
        if (countSoFar >= k) {
            return (actc_t)freq;
        }
    }
    return FREQ_BUCKETS_SIZE - 1;
}

static actc_t FindKthFreqDesc(const uint32_t *buckets, uint64_t k)
{
    if (k == 0) {
        return 0;
    }

    uint64_t countSoFar = 0;
    for (int freq = FREQ_BUCKETS_SIZE - 1; freq >= 0; freq--) {
        countSoFar += buckets[freq];
        if (countSoFar >= k) {
            return (actc_t)freq;
        }
    }
    return 0;
}

static uint64_t BuildFreqBucketsForNode(ProcessAttr *process, int nid, uint64_t offset, uint32_t *buckets)
{
    uint64_t count = 0;
    uint64_t actcLen = process->scanAttr.actcLen[nid];
    ActcData *actcData = process->scanAttr.actcData[nid];

    if (!actcData || offset >= actcLen) {
        return 0;
    }

    for (uint64_t i = offset; i < actcLen; i++) {
        int freq = actcData[i].freq;
        freq = MIN(freq, FREQ_BUCKETS_SIZE - 1);
        buckets[freq]++;
        count++;
    }

    return count;
}

static uint64_t CalcFreqWeightForVm(ProcessAttr *process, int localNid, int remoteNid,
                                     const uint32_t *l2Buckets, uint64_t l2ActcLen)
{
    uint64_t freqWt;
    actc_t l1FreqMax = process->scanAttr.actCount[localNid].freqMax;
    actc_t l2FreqMax = process->scanAttr.actCount[remoteNid].freqMax;

    if (l2ActcLen > 0 && l2FreqMax > 0) {
        int percentile = GetRemoteFreqPercentileConfig();
        double numToSkip = ((double)(PERCENTAGE_BASE_INT - percentile) / PERCENTAGE_BASE_DOUBLE) * l2ActcLen;
        uint64_t skipCount = (uint64_t)floor(numToSkip);

        if (skipCount > 0 && skipCount < l2ActcLen) {
            l2FreqMax = FindKthFreqDesc(l2Buckets, skipCount + 1);
        }
    }

    if (l1FreqMax > 0) {
        freqWt = (uint64_t)((double)l2FreqMax / l1FreqMax);
    } else {
        freqWt = l2FreqMax;
    }

    freqWt = MAX(freqWt, process->separateParam.freqWt);
    freqWt = MAX(freqWt, 1);

    if (GetFreqWtConfig() > 0) {
        freqWt = GetFreqWtConfig();
    }

    return freqWt;
}

static uint64_t CalLowMigrateNumByBuckets(uint64_t migrateNum, uint64_t freqWt, uint32_t slowThred,
                                           const uint32_t *l1Buckets, const uint32_t *l2Buckets,
                                           uint64_t l1Count, uint64_t l2Count)
{
    uint64_t low = 0;
    uint64_t high = MIN(migrateNum, MIN(l1Count, l2Count));
    if (high == 0) {
        return 0;
    }

    while (low < high) {
        uint64_t mid = low + (high - low + 1) / 2;
        uint64_t freqL1 = freqWt * FindKthFreqAsc(l1Buckets, mid);
        uint64_t freqL2 = FindKthFreqDesc(l2Buckets, mid);
        if ((GetZeroFreqMigrateEnableConfig() && (freqL1 == 0) && (freqL2 > 0)) || (freqL1 + slowThred < freqL2)) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

static uint64_t CalcSwapNumForVm(ProcessAttr *process, int localNid, int remoteNid,
                                    const uint64_t numaOffset[], const uint64_t numaFreePage[])
{
    uint64_t migrateNum;
    uint64_t freqWt;
    uint32_t slowThred;

    // 获取统计数据
    ActCount *localActCount = &process->scanAttr.actCount[localNid];
    ActCount *remoteActCount = &process->scanAttr.actCount[remoteNid];

    // 计算有效范围
    uint64_t localOffset = numaOffset[localNid];
    uint64_t remoteOffset = numaOffset[remoteNid];
    uint64_t localLen = process->scanAttr.actcLen[localNid];
    uint64_t remoteLen = process->scanAttr.actcLen[remoteNid];

    if (localOffset >= localLen || remoteOffset >= remoteLen) {
        return 0;
    }

    // 构建频率桶
    uint32_t l1Buckets[FREQ_BUCKETS_SIZE] = {0};
    uint32_t l2Buckets[FREQ_BUCKETS_SIZE] = {0};

    uint64_t l1Count = BuildFreqBucketsForNode(process, localNid, localOffset, l1Buckets);
    uint64_t l2Count = BuildFreqBucketsForNode(process, remoteNid, remoteOffset, l2Buckets);

    if (l1Count == 0 || l2Count == 0) {
        return 0;
    }

    // 计算基础迁移数量（复用原有 MIN 链逻辑）
    migrateNum = MIN(localLen - localOffset, remoteLen - remoteOffset);
    migrateNum = MIN(migrateNum, process->separateParam.maxMigrate);
    migrateNum = MIN(migrateNum, numaFreePage[localNid]);
    migrateNum = MIN(migrateNum, remoteActCount->freqNum);  // 远端有频次的页面数

    // 检查是否开启 swap
    if (!process->enableSwap) {
        SMAP_LOGGER_INFO("Pid %d not enable swap.", process->pid);
        return 0;
    }

    // 计算频率权重和阈值
    freqWt = CalcFreqWeightForVm(process, localNid, remoteNid, l2Buckets, remoteLen - remoteOffset);
    slowThred = process->separateParam.slowThred * freqWt;

    SMAP_LOGGER_INFO("Pid %d VM swap: freqWt %lu, slowThred: %u, localNid: %d, remoteNid: %d.",
                      process->pid, freqWt, slowThred, localNid, remoteNid);

    // 使用桶统计 + 二分查找计算迁移数量
    migrateNum = CalLowMigrateNumByBuckets(migrateNum, freqWt, slowThred,
                                            l1Buckets, l2Buckets, l1Count, l2Count);

    return migrateNum;
}

static uint64_t CalcSwapNum(ProcessAttr *process, int localNid, int remoteNid,
                              const uint64_t numaOffset[], const uint64_t numaFreePage[])
{
    if (process->type == PROCESS_TYPE) {
        uint64_t migrateNum;
        uint64_t lastFreqNum;
        uint64_t lastZeroNum;
        ActCount *localActCount = &process->scanAttr.actCount[localNid];
        ActCount *remoteActCount = &process->scanAttr.actCount[remoteNid];
        uint64_t localLen = process->scanAttr.actcLen[localNid] - numaOffset[localNid];
        uint64_t localFree = localActCount->pageNum - localActCount->whiteNum;
        uint64_t remoteFree = remoteActCount->pageNum - remoteActCount->whiteNum;
        uint64_t freqBucketZero = localActCount->freqBuckets[0];
        /* The number of pages with swap should not exceed 1% */
        uint64_t maxByTotal = process->walkPage.nrPage / HUNDRED;
        if (freqBucketZero > numaOffset[localNid]) {
            lastZeroNum = freqBucketZero - numaOffset[localNid];
        } else {
            lastZeroNum = 0;
        }
        if (remoteActCount->freqNum > numaOffset[remoteNid]) {
            lastFreqNum = remoteActCount->freqNum - numaOffset[remoteNid];
        } else {
            lastFreqNum = 0;
        }
        migrateNum = MIN(localLen, process->separateParam.maxMigrate);
        migrateNum = MIN(migrateNum, numaFreePage[localNid]);
        migrateNum = MIN(migrateNum, lastZeroNum);
        migrateNum = MIN(migrateNum, lastFreqNum);
        migrateNum = MIN(migrateNum, localFree);
        migrateNum = MIN(migrateNum, remoteFree);
        migrateNum = MIN(migrateNum, maxByTotal);

        return migrateNum;
    } else if (process->type == VM_TYPE) {
        return CalcSwapNumForVm(process, localNid, remoteNid, numaOffset, numaFreePage);
    }

    return 0;
}

void FindThreshold(const SelectionMode mode, uint64_t nrMig, const uint32_t *buckets, int *thresholdFreq,
                          uint32_t *takeAtThreshold)
{
    (*thresholdFreq) = mode == SELECT_TOP_K ? 0 : FREQ_BUCKETS_SIZE;
    (*takeAtThreshold) = 0;
    size_t countSoFar = 0;

    if (mode == SELECT_TOP_K) {
        for (int i = FREQ_BUCKETS_SIZE - 1; i >= 0; --i) {
            if (buckets[i] == 0) {
                continue;
            }
            if (countSoFar + buckets[i] >= nrMig) {
                (*thresholdFreq) = i;
                (*takeAtThreshold) = nrMig - countSoFar;
                break;
            }
            countSoFar += buckets[i];
        }
    } else {
        for (int i = 0; i < FREQ_BUCKETS_SIZE; ++i) {
            if (buckets[i] == 0) {
                continue;
            }
            if (countSoFar + buckets[i] >= nrMig) {
                (*thresholdFreq) = i;
                (*takeAtThreshold) = nrMig - countSoFar;
                break;
            }
            countSoFar += buckets[i];
        }
    }
}

static void CollectPagesVm(const SelectionMode mode, uint64_t actcLen, ActcData *currentData,
                           struct MigList *currMlist, uint64_t nrMig, int thresholdFreq,
                           uint32_t takeAtThreshold, uint32_t *selectedBuckets)
{
    int nrLocalNuma = GetNrLocalNuma();

    /* Pre-pass: 统计边界频次页面的 prior 分布 */
    uint64_t priorCounts[PRIOR_MAX_VALUE + 1] = {0};
    for (size_t i = 0; i < actcLen; ++i) {
        if (currentData[i].isSelected) {
            continue;
        }
        if (currentData[i].freq != (actc_t)thresholdFreq) {
            continue;
        }
        priorCounts[currentData[i].prior]++;
    }

    /* 计算每个 prior 级别需取多少页 */
    uint32_t remaining = takeAtThreshold;
    uint64_t takeFrom[PRIOR_MAX_VALUE + 1] = {0};
    for (int p = PRIOR_MAX_VALUE; p >= 0 && remaining > 0; --p) {
        uint64_t take = MIN(priorCounts[p], remaining);
        takeFrom[p] = take;
        remaining -= take;
    }

    /* Unified pass: 一次遍历收集全部页面，天然升序 */
    size_t collectedCount = 0;
    for (size_t i = 0; i < actcLen && collectedCount < nrMig; ++i) {
        if (currentData[i].isSelected) {
            continue;
        }

        int freq = currentData[i].freq;
        bool shouldTake = (mode == SELECT_TOP_K) ?
                          (freq > thresholdFreq) : (freq < thresholdFreq);

        if (!shouldTake && freq == thresholdFreq && takeFrom[currentData[i].prior] > 0) {
            shouldTake = true;
            takeFrom[currentData[i].prior]--;
        }
        if (!shouldTake) {
            continue;
        }

        if (currMlist->from < nrLocalNuma && currentData[i].isWhiteListPage) {
            continue;
        }

        currMlist->addr[collectedCount++] = i;
        selectedBuckets[MIN(freq, FREQ_BUCKETS_SIZE - 1)]++;
        currentData[i].isSelected = 1;
    }

    currMlist->nr = collectedCount;
}

static void CollectPages4K(const SelectionMode mode, uint64_t actcLen, ActcData *currentData,
                         struct MigList *currMlist, uint64_t nrMig, int thresholdFreq, uint32_t takeAtThreshold,
                         uint32_t *selectedBuckets)
{
    int nrLocalNuma = GetNrLocalNuma();
    uint32_t tmp = takeAtThreshold;
    size_t collectedCount = 0;
    for (size_t i = 0; i < actcLen && collectedCount < nrMig; ++i) {
        if (currentData[i].isSelected) {
            continue;
        }
        int freq = currentData[i].freq;
        bool shouldTake = false;

        if (mode == SELECT_TOP_K) {
            shouldTake = (freq > thresholdFreq) || (freq == thresholdFreq && tmp > 0);
        } else {
            shouldTake = (freq < thresholdFreq) || (freq == thresholdFreq && tmp > 0);
        }

        if (shouldTake) {
            if (currMlist->from < nrLocalNuma && currentData[i].isWhiteListPage) {
                continue;
            }
            currMlist->addr[collectedCount++] = i;
            int bucketIdx = MIN(freq, FREQ_BUCKETS_SIZE - 1);
            selectedBuckets[bucketIdx]++;
            currentData[i].isSelected = 1;
            if (freq == thresholdFreq) {
                tmp--;
            }
        }
    }
    currMlist->nr = collectedCount;
}

static int BuildSelectKMlistAddr(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                 const uint64_t numaOffset[MAX_NODES], int from, int to, SelectionMode mode)
{
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t offset = numaOffset[from];
    uint64_t n = process->scanAttr.actcLen[from];
    ActcData *currentData = process->scanAttr.actcData[from];
    struct MigList *currentMig = &mlist[from][to];
    ActCount *actCount = &process->scanAttr.actCount[from];
    uint32_t *selectedBuckets = process->scanAttr.selectedBuckets[from];
    uint32_t remainBuckets[FREQ_BUCKETS_SIZE];

    if (offset >= n) {
        currentMig->nr = 0;
        return 0;
    }

    uint64_t remainLen = n - offset;
    uint64_t nrMig = mlist[from][to].nr;
    nrMig = MIN(nrMig, remainLen);
    currentMig->nr = nrMig;
    if (nrMig == 0) {
        return 0;
    }
    if (!currentData) {
        currentMig->nr = 0;
        currentMig->addr = NULL;
        return -EINVAL;
    }
    currentMig->addr = calloc(nrMig, sizeof(uint64_t));
    if (!currentMig->addr) {
        return -ENOMEM;
    }

    /* 计算剩余范围的频次桶 = freqBuckets - selectedBuckets */
    /* freqBuckets 在 CalcActcStats 中已排除白名单页面，迁出场景正确 */
    for (int f = 0; f < FREQ_BUCKETS_SIZE; f++) {
        remainBuckets[f] = actCount->freqBuckets[f] - selectedBuckets[f];
    }

    int thresholdFreq;
    uint32_t takeAtThreshold;
    FindThreshold(mode, nrMig, remainBuckets, &thresholdFreq, &takeAtThreshold);

    if (process->type == VM_TYPE) {
        CollectPagesVm(mode, n, currentData, currentMig, nrMig, thresholdFreq, takeAtThreshold, selectedBuckets);
    } else {
        CollectPages4K(mode, n, currentData, currentMig, nrMig, thresholdFreq, takeAtThreshold, selectedBuckets);
    }

    return 0;
}

static int SeparateStrategyInner(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                   uint64_t numaOffset[MAX_NODES], struct NumaInfo info, uint64_t swapNum)
{
    int ret = 0;
    int localNid = info.localNid;
    int remoteNid = info.remoteNid;
    StrategyAttribute *strat = &process->strategyAttr;
    uint32_t demotePageNr = strat->nrMigratePages[localNid][remoteNid];
    uint32_t promotePageNr = strat->nrMigratePages[remoteNid][localNid];
    SMAP_LOGGER_INFO("Pid %lld NUMA%u to NUMA%u swap %llu demote %u promote %u\n", process->pid, localNid, remoteNid,
                     swapNum, demotePageNr, promotePageNr);

    if (demotePageNr > 0) {
        mlist[localNid][remoteNid].nr = swapNum + demotePageNr;
        mlist[remoteNid][localNid].nr = swapNum;
    } else if (promotePageNr > 0) {
        mlist[remoteNid][localNid].nr = swapNum + promotePageNr;
        mlist[localNid][remoteNid].nr = swapNum;
    } else {
        mlist[remoteNid][localNid].nr = mlist[localNid][remoteNid].nr = swapNum;
    }

    ret = BuildSelectKMlistAddr(process, mlist, numaOffset, localNid, remoteNid, SELECT_BOTTOM_K);
    if (ret) {
        SMAP_LOGGER_ERROR("Build Mlist addr error.\n");
        return ret;
    }
    ret = BuildSelectKMlistAddr(process, mlist, numaOffset, remoteNid, localNid, SELECT_TOP_K);
    if (ret) {
        SMAP_LOGGER_ERROR("Build Mlist addr error.\n");
        return ret;
    }

    numaOffset[localNid] += mlist[localNid][remoteNid].nr;
    numaOffset[remoteNid] += mlist[remoteNid][localNid].nr;

    return ret;
}

static uint64_t CalcAdditionalPage(ProcessAttr *process, int localNid, int remoteNid, uint64_t aimNum,
                                   const uint64_t numaOffest[], const uint64_t numaFreePage[])
{
    uint64_t additionalNum;
    uint64_t swappableNum = process->scanAttr.actcLen[localNid] - numaOffest[localNid];

    additionalNum = MIN(swappableNum, aimNum);
    additionalNum = MIN(additionalNum, process->separateParam.maxMigrate);
    additionalNum = MIN(additionalNum, numaFreePage[localNid]);
    additionalNum = MIN(additionalNum, numaFreePage[remoteNid]);
    return additionalNum;
}

static void UpdateSwapNum(ProcessAttr *process, uint64_t swapNum[LOCAL_NUMA_BITS][MAX_NODES], int localNumaNum,
                          uint64_t numaFreePage[MAX_NODES])
{
    uint64_t numaOffset[MAX_NODES] = { 0 };
    StrategyAttribute *sa = &process->strategyAttr;

    for (int nid1 = 0; nid1 < MAX_NODES; nid1++) {
        for (int nid2 = 0; nid2 < MAX_NODES; nid2++) {
            numaOffset[nid1] += sa->nrMigratePages[nid1][nid2];
        }
    }
    for (int remoteNid = localNumaNum; remoteNid < localNumaNum + REMOTE_NUMA_NUM; remoteNid++) {
        int remoteIndex = remoteNid - localNumaNum;
        if (NotInAttrL2(process, remoteNid)) {
            continue;
        }
        for (int localNid = 0; localNid < localNumaNum; localNid++) {
            if (NotInAttrL1(process, localNid)) {
                continue;
            }
            // memSize and nrMigratePages both being zero means no swap should happen for processes in MIG_MEMSIZE_MODE.
            if (process->migrateMode == MIG_MEMSIZE_MODE && sa->memSize[localNid][remoteIndex] == 0 &&
                sa->nrMigratePages[localNid][remoteNid] == 0) {
                continue;
            }
            swapNum[localNid][remoteNid] = CalcSwapNum(process, localNid, remoteNid, numaOffset, numaFreePage);
            numaOffset[localNid] += swapNum[localNid][remoteNid];
            numaOffset[remoteNid] += swapNum[localNid][remoteNid];
        }
        uint64_t remoteGuarantee = process->scanAttr.actCount[remoteNid].remoteHotNum;
        for (int localNid = 0; localNid < localNumaNum; localNid++) {
            if (numaOffset[remoteNid] >= remoteGuarantee) {
                break;
            }
            if (NotInAttrL1(process, localNid)) {
                continue;
            }
            uint64_t newAddedNum = CalcAdditionalPage(
                process, localNid, remoteNid, remoteGuarantee - numaOffset[remoteNid], numaOffset, numaFreePage);
            swapNum[localNid][remoteNid] += newAddedNum;
            numaOffset[localNid] += newAddedNum;
            numaOffset[remoteNid] += newAddedNum;
        }
    }
}

int SeparateStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    int ret = 0;
    if (process->separateParam.freqWt == 0) {
        InitSeparateParam(process);
    }
    CalcMaxMigrate(MAX_MIGRATE_DIVISOR_2M, process->walkPage.nrPage, &process->separateParam.maxMigrate);
    if (!ShouldMigrate(process)) {
        return 0;
    }
    uint64_t numaOffset[MAX_NODES] = { 0 };
    uint64_t swapNum[LOCAL_NUMA_BITS][MAX_NODES] = { 0 };
    uint64_t numaFreePages[MAX_NODES] = { 0 };
    for (int nid = 0; nid < MAX_NODES; ++nid) {
        if (InAttrL1(process, nid) || InAttrL2(process, nid)) {
            numaFreePages[nid] = IsHugeMode() ? GetNrFreeHugePagesByNode(nid) : GetNrFreePagesByNode(nid);
        }
    }

    int localNumaNum = GetNrLocalNuma();
    NumaInfo info;
    UpdateSwapNum(process, swapNum, localNumaNum, numaFreePages);
    for (info.localNid = 0; info.localNid < localNumaNum; info.localNid++) {
        if (NotInAttrL1(process, info.localNid)) {
            continue;
        }
        for (info.remoteNid = localNumaNum; info.remoteNid < localNumaNum + REMOTE_NUMA_NUM; info.remoteNid++) {
            if (NotInAttrL2(process, info.remoteNid)) {
                continue;
            }
            if ((ret = SeparateStrategyInner(process, mlist, numaOffset, info,
                                               swapNum[info.localNid][info.remoteNid])) != 0) {
                FreeMlist(mlist);
                return ret;
            }
        }
    }
    return 0;
}

static int FreqAscFunc(const void *actc1, const void *actc2)
{
    LevelActcData *a1 = (LevelActcData *)actc1;
    LevelActcData *a2 = (LevelActcData *)actc2;

    if (a1->freq < a2->freq) {
        return -1;
    } else if (a1->freq > a2->freq) {
        return 1;
    } else {
        return (int)a2->prior - (int)a1->prior;
    }
}

static int AddrAscFunc(const void *a, const void *b)
{
    uint64_t addr1 = *(uint64_t *)a;
    uint64_t addr2 = *(uint64_t *)b;
    if (addr1 == addr2) {
        return 0;
    }
    return addr1 < addr2 ? -1 : 1;
}

static int FreqDescFunc(const void *actc1, const void *actc2)
{
    LevelActcData *a1 = (LevelActcData *)actc1;
    LevelActcData *a2 = (LevelActcData *)actc2;

    if (a1->freq < a2->freq) {
        return 1;
    } else if (a1->freq > a2->freq) {
        return -1;
    } else {
        return (int)a1->prior - (int)a2->prior;
    }
}

static uint64_t CalMultiNumaVmLowMigrateNum(uint64_t migrateNum, uint64_t freqWt, uint32_t slowThred,
                                            LevelActcData *levelActcData[NR_LEVEL])
{
    uint64_t low = 0;
    uint64_t high = migrateNum;
    // calculate migrate num based on freq
    while (low < high) {
        uint64_t mid = low + (high - low + 1) / 2;
        if (mid < 1) {
            return 0;
        }
        uint64_t freqL1 = freqWt * levelActcData[L1][mid - 1].freq;
        uint64_t freqL2 = levelActcData[L2][mid - 1].freq;
        if ((GetZeroFreqMigrateEnableConfig() && (freqL1 == 0) && (freqL2 > 0)) || (freqL1 + slowThred < freqL2)) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

static uint64_t CalcMultiNumaVmSwapNumByFreq(ProcessAttr *process, LevelActcData *levelActcData[NR_LEVEL],
                                             uint64_t levelActcLen[NR_LEVEL])
{
    uint64_t freqWt = 0;
    uint64_t freqNum = 0;
    uint32_t slowThred = 0;
    uint16_t l1FreqMax = 0;
    uint16_t l2FreqMax = 0;
    for (int i = 0; i < levelActcLen[L2]; i++) {
        if (levelActcData[L2][i].freq > 0) {
            freqNum++;
        }
    }

    if (levelActcLen[L1] > 0) {
        l1FreqMax = levelActcData[L1][levelActcLen[L1] - 1].freq;
    }
    if (levelActcLen[L2] > 0) {
        l2FreqMax = levelActcData[L2][0].freq;
    }

    uint64_t freeHugePage[NR_LEVEL] = { 0 };
    for (int nid = 0; nid < MAX_NODES; ++nid) {
        if (InAttrL1(process, nid)) {
            freeHugePage[L1] += GetNrFreeHugePagesByNode(nid);
        }
        if (InAttrL2(process, nid)) {
            freeHugePage[L2] += GetNrFreeHugePagesByNode(nid);
        }
    }

    uint64_t migrateNum = MIN(levelActcLen[L1], levelActcLen[L2]);
    migrateNum = MIN(migrateNum, freeHugePage[L1]);
    migrateNum = MIN(migrateNum, freqNum);
    if (levelActcLen[L2] > 0 && l2FreqMax > 0) {
        int percentile = GetRemoteFreqPercentileConfig();
        double numToSkip = ((double)(PERCENTAGE_BASE_INT - percentile) / PERCENTAGE_BASE_DOUBLE) * levelActcLen[L2];
        size_t maxIndex = (size_t)floor(numToSkip);
        l2FreqMax = levelActcData[L2][maxIndex].freq;
    } else {
        l2FreqMax = 0;
    }
    if (l1FreqMax) {
        freqWt = (uint64_t)((double)l2FreqMax / l1FreqMax);
    } else {
        freqWt = l2FreqMax;
    }
    freqWt = MAX(freqWt, process->separateParam.freqWt);
    slowThred = process->separateParam.slowThred * freqWt;
    SMAP_LOGGER_DEBUG("Pid %d freqWt %lu, slowThred: %u.", process->pid, freqWt, slowThred);

    migrateNum = CalMultiNumaVmLowMigrateNum(migrateNum, freqWt, slowThred, levelActcData);
    return migrateNum;
}

static int GroupMigPagesByNode(LevelActcData *levelActcData[NR_LEVEL], uint64_t swapNum, uint64_t nrMig[MAX_NODES],
                               uint64_t *migAddrArray[MAX_NODES])
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (nrMig[i] > 0) {
            migAddrArray[i] = calloc(nrMig[i], sizeof(uint64_t));
            if (!migAddrArray[i]) {
                return -ENOMEM;
            }
        }
    }

    uint64_t counters[MAX_NODES] = { 0 };
    for (uint64_t i = 0; i < swapNum; i++) {
        int l1node = levelActcData[L1][i].node;
        int l2node = levelActcData[L2][i].node;
        if (l1node < 0 || l1node >= MAX_NODES || l2node < 0 || l2node >= MAX_NODES) {
            continue;
        }
        if (counters[l1node] >= nrMig[l1node] || counters[l2node] >= nrMig[l2node]) {
            continue;
        }
        migAddrArray[l1node][counters[l1node]] = levelActcData[L1][i].addr;
        migAddrArray[l2node][counters[l2node]] = levelActcData[L2][i].addr;
        counters[l1node]++;
        counters[l2node]++;
    }

    return 0;
}

static int BuildMigListForDirection(MigrateDirection dir, uint64_t *migAddrArray[MAX_NODES], uint64_t nrMig[MAX_NODES],
                                    uint64_t destFreeList[MAX_NODES], struct MigList mlist[MAX_NODES][MAX_NODES])
{
    int nrLocalNuma = GetNrLocalNuma();
    if (dir != PROMOTE && dir != DEMOTE) {
        return -EINVAL;
    }
    int sourceStart = dir == PROMOTE ? nrLocalNuma : 0;
    int sourceEnd = dir == PROMOTE ? nrLocalNuma + REMOTE_NUMA_NUM : nrLocalNuma;
    int destStart = dir == PROMOTE ? 0 : nrLocalNuma;
    int destEnd = dir == PROMOTE ? nrLocalNuma : nrLocalNuma + REMOTE_NUMA_NUM;
    for (int from = sourceStart; from < sourceEnd; from++) {
        if (!migAddrArray[from] || nrMig[from] == 0) {
            continue;
        }

        uint64_t curMigIndex = 0;
        uint64_t srcPagesCount = nrMig[from];

        for (int to = destStart; to < destEnd; to++) {
            if (destFreeList[to] == 0) {
                continue;
            }

            uint64_t pagesToThisDest = MIN(destFreeList[to], srcPagesCount - curMigIndex);
            if (pagesToThisDest == 0) {
                continue;
            }
            SMAP_LOGGER_INFO("migrate %lu pages from NUMA %d to NUMA %d", pagesToThisDest, from, to);
            mlist[from][to].nr = pagesToThisDest;
            mlist[from][to].addr = calloc(pagesToThisDest, sizeof(uint64_t));
            if (!mlist[from][to].addr) {
                return -ENOMEM;
            }

            for (uint64_t i = 0; i < pagesToThisDest; i++) {
                mlist[from][to].addr[i] = migAddrArray[from][curMigIndex + i];
            }

            // 地址升序排序，确保 convert_pos_to_paddr_sorted 正确工作
            if (pagesToThisDest > 1) {
                qsort(mlist[from][to].addr, pagesToThisDest, sizeof(uint64_t), AddrAscFunc);
            }

            destFreeList[to] -= pagesToThisDest;
            curMigIndex += pagesToThisDest;

            if (curMigIndex >= srcPagesCount) {
                break;
            }
        }

        if (curMigIndex < srcPagesCount) {
            SMAP_LOGGER_ERROR("srcNid %d, Some pages cannot be migrated due to low memory in the destNid.", from);
            return -MULTI_NUMA_VM_OOM;
        }
    }
    return 0;
}

static int BuildDemoteMultiNumaMigLists(uint64_t *migAddrArray[MAX_NODES], uint64_t nrMig[MAX_NODES],
                                        struct MigList mlist[MAX_NODES][MAX_NODES],
                                        RemoteMigInfo remoteMigInfo[REMOTE_NUMA_NUM])
{
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t freeHugePages[MAX_NODES] = { 0 };

    for (int nid = nrLocalNuma; nid < MAX_NODES; nid++) {
        if (remoteMigInfo[nid - nrLocalNuma].dir == DEMOTE && remoteMigInfo[nid - nrLocalNuma].nrMig > 0) {
            freeHugePages[nid] = GetNrFreeHugePagesByNode(nid);
            freeHugePages[nid] = MIN(freeHugePages[nid], remoteMigInfo[nid - nrLocalNuma].nrMig);
        }
    }

    return BuildMigListForDirection(DEMOTE, migAddrArray, nrMig, freeHugePages, mlist);
}

static void CountMigrationPerNode(uint64_t nrMig[MAX_NODES], LevelActcData *levelActcData[NR_LEVEL], uint64_t swapNum,
                                  int nrLocalNuma)
{
    for (uint64_t i = 0; i < swapNum; i++) {
        int l1node = levelActcData[L1][i].node;
        int l2node = levelActcData[L2][i].node;
        if (l2node >= nrLocalNuma && l2node < nrLocalNuma + REMOTE_NUMA_NUM && l1node >= 0 && l1node < nrLocalNuma) {
            nrMig[l1node]++;
            nrMig[l2node]++;
        }
    }
}

static void FreeMultiNumaVmData(LevelActcData *levelActcData[NR_LEVEL], uint64_t *migAddrArray[MAX_NODES])
{
    for (int i = 0; i < NR_LEVEL; i++) {
        if (levelActcData[i]) {
            free(levelActcData[i]);
            levelActcData[i] = NULL;
        }
    }

    for (int i = 0; i < MAX_NODES; i++) {
        if (migAddrArray[i]) {
            free(migAddrArray[i]);
            migAddrArray[i] = NULL;
        }
    }
}

static int BuildLevelActcData(ProcessAttr *process, LevelActcData *levelActcData, uint64_t nrPages,
                              uint64_t *levelActcLen, int level)
{
    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < MAX_NODES; i++) {
        if (process->scanAttr.actcLen[i] == 0) {
            continue;
        }
        if (level == L2 && i < nrLocalNuma) {
            continue;
        }
        if (level == L1 && i >= nrLocalNuma) {
            continue;
        }

        uint64_t actcIdx;
        for (uint32_t idx = 0; idx < process->scanAttr.actcLen[i]; idx++) {
            actcIdx = *levelActcLen;
            if (actcIdx >= nrPages) {
                break;
            }
            levelActcData[actcIdx].addr = idx;
            levelActcData[actcIdx].node = i;
            levelActcData[actcIdx].freq = process->scanAttr.actcData[i][idx].freq;
            levelActcData[actcIdx].prior = process->scanAttr.actcData[i][idx].prior;
            *levelActcLen = *levelActcLen + 1;
        }
    }

    SMAP_LOGGER_INFO("Pid %d level: %d, levelActcLen %lu.", process->pid, level, *levelActcLen);

    if (*levelActcLen > 0) {
        if (level == L1) {
            qsort(levelActcData, *levelActcLen, sizeof(LevelActcData), FreqAscFunc);
        } else {
            qsort(levelActcData, *levelActcLen, sizeof(LevelActcData), FreqDescFunc);
        }
    }

    return 0;
}

static int PromoteMultiNumaVmStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                      RemoteMigInfo remoteMigInfo[REMOTE_NUMA_NUM], int nrLocalNuma)
{
    uint64_t localFreeHugePages[LOCAL_NUMA_NUM] = { 0 };
    uint64_t numaOffset[MAX_NODES] = { 0 };
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        if (InAttrL1(process, nid)) {
            localFreeHugePages[nid] = GetNrFreeHugePagesByNode(nid);
        }
    }

    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        if (remoteMigInfo[i].dir != PROMOTE || remoteMigInfo[i].nrMig == 0) {
            continue;
        }
        int from = i + nrLocalNuma;
        // use topk algorithm instead of qsort
        for (int to = 0; to < nrLocalNuma; to++) {
            if (!InAttrL1(process, to)) {
                continue;
            }
            mlist[from][to].nr = MIN(remoteMigInfo[i].nrMig, localFreeHugePages[to]);
            int ret = BuildSelectKMlistAddr(process, mlist, numaOffset, from, to, SELECT_TOP_K);
            if (ret) {
                SMAP_LOGGER_ERROR("PromoteMultiNumaVmStrategy build mlist addr failed %d.", ret);
                return ret;
            }
            SMAP_LOGGER_INFO("migrate %lu pages from NUMA %d to NUMA %d", mlist[from][to].nr, from, to);
            localFreeHugePages[to] -= mlist[from][to].nr;
            remoteMigInfo[i].nrMig -= mlist[from][to].nr;
        }
        if (remoteMigInfo[i].nrMig > 0) {
            SMAP_LOGGER_ERROR("Pid %d insufficient memory in the destNid.", process->pid);
            process->isLowMem = true;
            return -MULTI_NUMA_VM_OOM;
        }
    }
    return 0;
}

static void FreeDemoteMultiNumaVmData(LevelActcData *l1ActcData, uint64_t *migAddrArray[MAX_NODES])
{
    if (l1ActcData) {
        free(l1ActcData);
    }

    for (int i = 0; i < MAX_NODES; i++) {
        if (migAddrArray[i]) {
            free(migAddrArray[i]);
            migAddrArray[i] = NULL;
        }
    }
}

static int DemoteMultiNumaVmStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                     RemoteMigInfo remoteMigInfo[REMOTE_NUMA_NUM], uint64_t nrPages[NR_LEVEL],
                                     uint64_t demoteNum)
{
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t nrMig[MAX_NODES] = { 0 };
    uint64_t counters[MAX_NODES] = { 0 };
    uint64_t *migAddrArray[MAX_NODES] = { NULL };
    uint64_t l1ActcLen = 0;
    LevelActcData *l1ActcData = calloc(nrPages[L1], sizeof(LevelActcData));
    if (!l1ActcData) {
        return -ENOMEM;
    }

    int ret = BuildLevelActcData(process, l1ActcData, nrPages[L1], &l1ActcLen, L1);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build L1 actc data for process %d, ret: %d.", process->pid, ret);
        goto cleanup;
    }

    for (uint64_t i = 0; i < demoteNum && i < l1ActcLen; i++) {
        int node = l1ActcData[i].node;
        if (node >= 0 && node < nrLocalNuma) {
            nrMig[node] += 1;
        }
    }

    for (int i = 0; i < MAX_NODES; i++) {
        if (nrMig[i] > 0) {
            migAddrArray[i] = calloc(nrMig[i], sizeof(uint64_t));
            if (!migAddrArray[i]) {
                ret = -ENOMEM;
                goto cleanup;
            }
        }
    }

    for (uint64_t i = 0; i < demoteNum && i < l1ActcLen; i++) {
        int node = l1ActcData[i].node;
        if (node < 0 || node >= nrLocalNuma || counters[node] >= nrMig[node]) {
            continue;
        }

        migAddrArray[node][counters[node]++] = l1ActcData[i].addr;
    }

    ret = BuildDemoteMultiNumaMigLists(migAddrArray, nrMig, mlist, remoteMigInfo);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build migration lists for demote direction, ret: %d.", ret);
        process->isLowMem = ret == -MULTI_NUMA_VM_OOM ? true : false;
    }

cleanup:
    FreeDemoteMultiNumaVmData(l1ActcData, migAddrArray);
    return ret;
}

static int BuildSwapMigLists(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                             uint64_t nrMig[MAX_NODES], uint64_t *migAddrArray[MAX_NODES], int nrLocalNuma)
{
    // promote
    uint64_t freeHugePages[MAX_NODES] = { 0 };
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        if (InAttrL1(process, nid)) {
            freeHugePages[nid] = GetNrFreeHugePagesByNode(nid);
        }
    }

    int ret = BuildMigListForDirection(PROMOTE, migAddrArray, nrMig, freeHugePages, mlist);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build promote mig lists for pid %d, ret: %d.", process->pid, ret);
        return ret;
    }

    // demote
    for (int nid = nrLocalNuma; nid < MAX_NODES; nid++) {
        if (InAttrL2(process, nid)) {
            freeHugePages[nid] = nrMig[nid];
        }
    }

    ret = BuildMigListForDirection(DEMOTE, migAddrArray, nrMig, freeHugePages, mlist);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build demote mig lists for pid %d, ret :%d.", process->pid, ret);
    }

    return ret;
}

static int SwapMultiNumaVmStrategy(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES],
                                   uint64_t nrPages[NR_LEVEL], int nrLocalNuma)
{
    uint64_t nrMig[MAX_NODES] = { 0 };
    uint64_t *migAddrArray[MAX_NODES] = { NULL };
    uint64_t levelActcLen[NR_LEVEL] = { 0 };
    LevelActcData *levelActcData[NR_LEVEL] = { NULL };

    for (int i = 0; i < NR_LEVEL; i++) {
        if (nrPages[i] > 0) {
            levelActcData[i] = calloc(nrPages[i], sizeof(LevelActcData));
            if (!levelActcData[i]) {
                FreeMultiNumaVmData(levelActcData, migAddrArray);
                return -ENOMEM;
            }
        }
    }

    int ret = BuildLevelActcData(process, levelActcData[L1], nrPages[L1], &levelActcLen[L1], L1);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build L1 actc data for pid %d, ret: %d", process->pid, ret);
        FreeMultiNumaVmData(levelActcData, migAddrArray);
        return ret;
    }

    ret = BuildLevelActcData(process, levelActcData[L2], nrPages[L2], &levelActcLen[L2], L2);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build L2 actc data for pid %d, ret: %d", process->pid, ret);
        FreeMultiNumaVmData(levelActcData, migAddrArray);
        return ret;
    }
    uint64_t swapMigrateNum = CalcMultiNumaVmSwapNumByFreq(process, levelActcData, levelActcLen);
    SMAP_LOGGER_INFO("Pid %d swap num:%lu after CalcMultiNumaVmSwapNumByFreq.", process->pid, swapMigrateNum);
    if (swapMigrateNum == 0) {
        FreeMultiNumaVmData(levelActcData, migAddrArray);
        return 0;
    }

    CountMigrationPerNode(nrMig, levelActcData, swapMigrateNum, nrLocalNuma);

    ret = GroupMigPagesByNode(levelActcData, swapMigrateNum, nrMig, migAddrArray);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to classify pages by node for pid %d, ret: %d", process->pid, ret);
        FreeMultiNumaVmData(levelActcData, migAddrArray);
        return ret;
    }

    ret = BuildSwapMigLists(process, mlist, nrMig, migAddrArray, nrLocalNuma);
    if (ret) {
        SMAP_LOGGER_ERROR("Failed to build mig lists for pid %d, ret: %d", process->pid, ret);
    }
    FreeMultiNumaVmData(levelActcData, migAddrArray);
    return ret;
}

static void CalculateMigInfo(ProcessAttr *process, RemoteMigInfo remoteMigInfo[REMOTE_NUMA_NUM],
                             uint64_t nrPages[NR_LEVEL], uint64_t *demoteNum, uint64_t *promoteNum)
{
    int nrLocalNuma = GetNrLocalNuma();
    if (GetRunMode() == MEM_POOL_MODE) {
        for (int i = 0; i < process->remoteNumaCnt; i++) {
            int nid = process->migrateParam[i].nid;
            int index = nid - nrLocalNuma;
            uint64_t targetL2Page = KBToHugePage(process->migrateParam[i].memSize);
            if (targetL2Page > process->scanAttr.actcLen[nid]) {
                remoteMigInfo[index].dir = DEMOTE;
                remoteMigInfo[index].nrMig = targetL2Page - process->scanAttr.actcLen[nid];
                *demoteNum += remoteMigInfo[index].nrMig;
            } else if (targetL2Page < process->scanAttr.actcLen[nid]) {
                remoteMigInfo[index].dir = PROMOTE;
                remoteMigInfo[index].nrMig = process->scanAttr.actcLen[nid] - targetL2Page;
                *promoteNum += remoteMigInfo[index].nrMig;
            } else {
                remoteMigInfo[index].nrMig = 0;
                remoteMigInfo[index].dir = SWAP;
            }
        }
    } else {
        int l1Node = GetAttrL1(process);
        for (int l2Node = nrLocalNuma; l2Node < nrLocalNuma + REMOTE_NUMA_NUM; l2Node++) {
            if (NotInAttrL2(process, l2Node)) {
                continue;
            }
            int index = l2Node - nrLocalNuma;
            if (process->strategyAttr.nrMigratePages[l1Node][l2Node] > 0) {
                remoteMigInfo[index].dir = DEMOTE;
                remoteMigInfo[index].nrMig = process->strategyAttr.nrMigratePages[l1Node][l2Node];
                *demoteNum += remoteMigInfo[index].nrMig;
            } else if (process->strategyAttr.nrMigratePages[l2Node][l1Node] > 0) {
                remoteMigInfo[index].dir = PROMOTE;
                remoteMigInfo[index].nrMig = process->strategyAttr.nrMigratePages[l2Node][l1Node];
                *promoteNum += remoteMigInfo[index].nrMig;
            } else {
                remoteMigInfo[index].nrMig = 0;
                remoteMigInfo[index].dir = SWAP;
            }
        }
    }
}

int SeparateStrategyMultiNumaVm(ProcessAttr *process, struct MigList mlist[MAX_NODES][MAX_NODES])
{
    int ret = 0;
    int nrLocalNuma = GetNrLocalNuma();
    uint64_t demoteNum = 0;
    uint64_t promoteNum = 0;

    RemoteMigInfo remoteMigInfo[REMOTE_NUMA_NUM] = { 0 };
    if (process->separateParam.freqWt == 0) {
        InitSeparateParam(process);
    }

    uint64_t nrPages[NR_LEVEL] = { 0 };
    for (int i = 0; i < MAX_NODES; i++) {
        if (process->scanAttr.actcLen[i] == 0) {
            continue;
        }
        if (i < nrLocalNuma) {
            nrPages[L1] += process->scanAttr.actcLen[i];
        } else {
            nrPages[L2] += process->scanAttr.actcLen[i];
        }
    }

    CalculateMigInfo(process, remoteMigInfo, nrPages, &demoteNum, &promoteNum);
    SMAP_LOGGER_INFO("Pid %d, demoteNum: %lu, promoteNum: %lu.", process->pid, demoteNum, promoteNum);
    if (promoteNum == 0 && demoteNum == 0) {
        return SwapMultiNumaVmStrategy(process, mlist, nrPages, nrLocalNuma);
    }

    if (demoteNum && nrPages[L1] > 0) {
        ret = DemoteMultiNumaVmStrategy(process, mlist, remoteMigInfo, nrPages, demoteNum);
        if (ret) {
            SMAP_LOGGER_ERROR("Demote mulit numa vm strategy failed for pid %d, ret %d.", process->pid, ret);
            return ret;
        }
    }

    if (promoteNum && nrPages[L2] > 0) {
        ret = PromoteMultiNumaVmStrategy(process, mlist, remoteMigInfo, nrLocalNuma);
        if (ret) {
            SMAP_LOGGER_ERROR("Promote mulit numa vm strategy failed for pid %d, ret %d.", process->pid, ret);
        }
    }

    return ret;
}
