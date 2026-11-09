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

void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len)
{
    attr->walkPage.nrPage = 0;
    for (int i = 0; i < len; i++) {
        attr->walkPage.nrPages[i] = nrPages[i];
        attr->walkPage.nrPage += nrPages[i];
    }
    SMAP_LOGGER_INFO("Pid %d nrPage %llu.", attr->pid, attr->walkPage.nrPage);
}

/**
 * CalcActcStats - 从actc_data数组计算统计数据
 * @attr: ProcessAttr结构体指针
 *
 * 遍历actc_data数组，计算freqMax、freqMin、freqNum、freqSum等统计数据。
 */
void CalcActcStats(ProcessAttr *attr)
{
    uint16_t remoteHotThreshold = GetRemoteHotThreshold();
    int nrLocalNuma = GetNrLocalNuma();
    int cap = (int)GetNumaCap();

    for (int nid = 0; nid < cap; nid++) {
        uint64_t actcLen = attr->scanAttr.actcLen[nid];
        ActcData *actc = attr->scanAttr.actcData[nid];
        ActCount *count = &attr->scanAttr.actCount[nid];

        (void)memset_s(count->freqBuckets, sizeof(count->freqBuckets), 0, sizeof(count->freqBuckets));
        (void)memset_s(attr->scanAttr.selectedBuckets[nid], sizeof(attr->scanAttr.selectedBuckets[nid]), 0,
                       sizeof(attr->scanAttr.selectedBuckets[nid]));

        if (actcLen == 0 || !actc) {
            (void)memset_s(count, sizeof(*count), 0, sizeof(*count));
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
                /*
                 * freqSum 保持“当前窗口频次总和”语义：非 VM 的 actc 已合成
                 * prior+freq，这里扣除 prior 还原，供 freq 查询类接口使用；
                 * VM 的 prior 语义不同、未参与合成，直接累加。
                 */
                if (attr->type != VM_TYPE) {
                    count->freqSum += (freq > actc[i].prior) ? (uint64_t)(freq - actc[i].prior) : 0;
                } else {
                    count->freqSum += freq;
                }
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

        /* 打印各频次桶的页面数（本地+远端NUMA，跳过页面数为零的） */
        for (int f = 0; f < FREQ_BUCKETS_SIZE; f++) {
            if (count->freqBuckets[f] > 0) {
                SMAP_LOGGER_DEBUG("Node%d freq=%d pages=%u", nid, f, count->freqBuckets[f]);
            }
        }
    }
}

/**
 * MergeAccPriorToFreq - 用内核累计频次(prior/acc) + 当前窗口频次(freq) 合成决策分数
 * @actc: 单个node的actc_data起始地址
 * @actcLen: 该node的页数
 *
 * 决策分数 = 累计频次(prior) + 当前窗口频次(freq) 直接相加：
 *   - 本轮被访问的页(freq>0)在同 prior 档内获得即时增量，不沉底；
 *   - 真冷页 prior=0 且 freq=0 仍为 0；
 *   - 相加结果饱和到 FREQ_BUCKETS_SIZE-1，避免越界。
 * 合成后下游 freqBuckets/分位数阈值/选页机制原样复用单一 freq 指标。
 * 新页起跑值 DECAY_CYCLE/2 高于冷门槛；file 页由选页层白名单拦截。
 * VM 的 prior 来自前端采样数组，语义与内核 acc 不同，由调用方跳过合成。
 */
static void MergeAccPriorToFreq(ActcData *actc, size_t actcLen)
{
    for (size_t i = 0; i < actcLen; i++) {
        /* merged = prior + freq：累计热度 + 当前窗口访问。 */
        int merged = (int)actc[i].prior + (int)actc[i].freq;
        actc[i].freq = (actc_t)MIN(merged, FREQ_BUCKETS_SIZE - 1);
    }
}

/**
 * DistributeActcData - 将读取的数据分配到各node的actcData
 * @attr: ProcessAttr结构体指针
 * @pmb: ProcessMemBitmap结构体指针
 * @buf: 读取的数据缓冲区
 *
 * 非 VM 在挂到 actcData 之前完成 prior+freq 合成，保证发布出去的缓冲区
 * 一定是已合并状态，查询侧不会看到“已发布但未合并”的中间态。
 */
void DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf)
{
    /* 释放上一次的连续缓冲区（按actcData释放，第一个非空即缓冲区起始） */
    ResetActcData(attr->scanAttr.actcData, (int)GetNumaCap());

    size_t actc_offset = 0;
    int cap = (int)GetNumaCap();
    for (int nid = 0; nid < cap; nid++) {
        attr->scanAttr.actcLen[nid] = pmb->nrPages[nid];
        if (pmb->nrPages[nid] == 0) {
            continue;
        }
        if (attr->type != VM_TYPE) {
            MergeAccPriorToFreq(buf + actc_offset, pmb->nrPages[nid]);
        }
        attr->scanAttr.actcData[nid] = buf + actc_offset;
        actc_offset += pmb->nrPages[nid];
    }
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

    ret = snprintf_s(path, sizeof(path), sizeof(path) - 1, "/proc/smap/%d/mem_freq", attr->pid);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("snprintf_s mem_freq path failed for pid %d: %d", attr->pid, ret);
        return -EINVAL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("open mem_freq file failed for pid %d: %d", attr->pid, errno);
        return -ENODEV;
    }

    int cap = (int)GetNumaCap();
    for (int nid = 0; nid < cap; nid++) {
        total_actc += pmb->nrPages[nid];
    }

    if (total_actc == 0) {
        SMAP_LOGGER_INFO("pid %d has no pages, skip read", attr->pid);
        close(fd);
        DistributeActcData(attr, pmb, NULL);
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

    DistributeActcData(attr, pmb, buf);

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

int RefreshManagedLocalStatePeriodic(ProcessAttr *attr)
{
    ProcessAttr candidate = *attr;
    int ret = RefreshManagedLocalState(&candidate, false);
    if (ret) {
        return ret;
    }

    /*
     * 内核不维护 numa_nodes 位图，numaAttr.numaNodes 由用户态在
     * bitmap 回读后按各节点页数合成；此处仅刷新本地观测状态。
     */
    attr->managedLocalState = candidate.managedLocalState;
    return 0;
}

int BuildPidData(ProcessAttr *attr)
{
    int ret;
    struct ProcessMemBitmap pmb;

    if (!attr) {
        return -EINVAL;
    }
    if (attr->scanType != NORMAL_SCAN) {
        return 0;
    }
    /* per-pid查询页数元数据；-ESRCH表示本周期内核未跟踪该pid，静默跳过 */
    ret = AccessIoctlWalkPagemap(attr->pid, &pmb);
    if (ret == -ESRCH) {
        return 0;
    }
    if (ret) {
        return ret;
    }
    /* nrPages 定宽同上，全量拷贝（cap 之外槽位内核写 0） */
    SetPidNrPages(attr, pmb.nrPages, SMAP_MAX_NUMNODES);
    /*
     * 本地位直接由回读的各节点页数构造（nrPages[nid] != 0 即置位），
     * 如实反映本轮实际内存分布；远端位以接口配置的迁移目标
     * （targetConfig）为准，不随内核观测波动。
     */
    NumaBitmap numaNodes = { 0 };
    int nrLocalNuma = GetProcessManager()->nrLocalNuma;
    if (!NumaBitmapAlloc(&numaNodes)) {
        SMAP_LOGGER_ERROR("Alloc numaNodes for pid %d failed.", attr->pid);
        return -ENOMEM;
    }
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        if (pmb.nrPages[nid] != 0) {
            NumaBitmapSet(&numaNodes, nid);
        }
    }
    for (uint32_t i = 0; i < attr->targetConfig.count; i++) {
        AddL2ByNid(&numaNodes, attr->targetConfig.targets[i].remoteNid);
    }
    /*
     * 本地位为空有两种情况：首个 walk 尚未完成（全零快照），或
     * 进程页已全部迁到远端。两种情况都保留现有本地位（纳管预置
     * 的观测过渡值/迁移前的分布），保证 GetAttrL1 始终返回合法
     * L1；待进程重新产生本地页后自然收敛到实际分布。
     */
    if ((NumaGetL1Count(&numaNodes, nrLocalNuma)) == 0) {
        for (int nid = 0; nid < nrLocalNuma; nid++) {
            if (NumaBitmapTest(&attr->numaAttr.numaNodes, nid)) {
                NumaBitmapSet(&numaNodes, nid);
            }
        }
    }
    /*
     * 原位覆写 attr 现有位图，避免在扫描热路径上释放读侧（查询/序列化
     * 线程）正引用的 bits；仅在属性未绑定或字数不一致时才替换句柄。
     */
    NumaBitmap *attrNodes = &attr->numaAttr.numaNodes;
    if (attrNodes->bits != NULL && attrNodes->nWords == numaNodes.nWords) {
        size_t bytes = (size_t)numaNodes.nWords * sizeof(unsigned long);
        (void)memcpy_s(attrNodes->bits, bytes, numaNodes.bits, bytes);
        NumaBitmapFree(&numaNodes);
    } else {
        NumaBitmapFree(attrNodes);
        *attrNodes = numaNodes;
    }
    SMAP_LOGGER_INFO("Pid %d, numaNodes nWords %u, nrLocalNuma %u.", attr->pid, attr->numaAttr.numaNodes.nWords,
                     GetProcessManager()->nrLocalNuma);
    ret = FillPidData(attr, &pmb);
    if (ret) {
        return ret;
    }
    if (!attr->groupPolicy.enabled) {
        ret = RefreshManagedLocalStatePeriodic(attr);
        if (ret) {
            SMAP_LOGGER_ERROR("Refresh pid %d managed local state failed: %d.", attr->pid, ret);
        }
        CalibratePairAccount(attr);
        if (attr->syncWaitRemoteEmpty) {
            attr->syncWaitRemoteEmptySnapshotValid = true;
        }
    }
    return ret;
}

int RefreshAllPidData(void)
{
    int ret, failedCount = 0;
    struct ProcessManager *manager = GetProcessManager();
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);

    for (size_t k = 0; k < n; k++) {
        /* 非NORMAL_SCAN类型由BuildPidData内部过滤（返回0跳过） */
        ret = BuildPidData(all[k]->attr);
        if (ret) {
            SMAP_LOGGER_ERROR("Fill pid %d actc data failed.", all[k]->attr->pid);
            failedCount++;
        }
    }
    PidSlotReleaseRefs(all, n);
    CalcMigrateNrPagesPerPidMultiNuma();
    return failedCount;
}
