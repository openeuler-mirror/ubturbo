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

    for (int nid = 0; nid < MAX_NODES; nid++) {
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
                count->freqSum += freq;
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

        /* 打印各频次桶的页面数（仅本地NUMA，跳过页面数为零的） */
        if (nid < nrLocalNuma) {
            for (int f = 0; f < FREQ_BUCKETS_SIZE; f++) {
                if (count->freqBuckets[f] > 0) {
                    SMAP_LOGGER_DEBUG("Node%d freq=%d pages=%u", nid, f, count->freqBuckets[f]);
                }
            }
        }
    }
}

/**
 * DistributeActcData - 将读取的数据分配到各node的actcData
 * @attr: ProcessAttr结构体指针
 * @pmb: ProcessMemBitmap结构体指针
 * @buf: 读取的数据缓冲区
 */
void DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf)
{
    /* 释放上一次的连续缓冲区（按actcData释放，第一个非空即缓冲区起始） */
    ResetActcData(attr->scanAttr.actcData, MAX_NODES);

    size_t actc_offset = 0;
    for (int nid = 0; nid < MAX_NODES; nid++) {
        attr->scanAttr.actcLen[nid] = pmb->nrPages[nid];
        if (pmb->nrPages[nid] == 0) {
            continue;
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

    for (int nid = 0; nid < MAX_NODES; nid++) {
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

static int BuildBitmapBuf(size_t *len, char **buf)
{
    char *tmpBuf;
    size_t tmpLen;
    int ret = AccessIoctlWalkPagemap(&tmpLen);
    if (ret) {
        SMAP_LOGGER_ERROR("access ioctl walk pagemap error: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("AccessIoctlWalkPagemap bufLen %zu.", tmpLen);
    if (tmpLen == 0) {
        SMAP_LOGGER_ERROR("Access ioctl walk pagemap len invalid: %zu.", tmpLen);
        return -EINVAL;
    }

    tmpBuf = malloc(tmpLen);
    if (!tmpBuf) {
        tmpLen = 0;
        return -ENOMEM;
    }

    *len = tmpLen;
    *buf = tmpBuf;

    return 0;
}

static int ParseBitmapPid(struct ProcessMemBitmap *pmb, char *buf, size_t *offset)
{
    int ret;
    size_t pidSize = sizeof(pmb->pid);

    ret = memcpy_s(&pmb->pid, pidSize, buf, pidSize);
    if (ret) {
        return -ret;
    }
    *offset += pidSize;
    return 0;
}

static int ParseBitmapNrPages(struct ProcessMemBitmap *pmb, char *buf, size_t *offset)
{
    int ret;
    size_t pageNumSize = sizeof(pmb->nrPages[0]);
    size_t tmpOffset = 0;

    for (int nid = 0; nid < MAX_NODES; nid++) {
        ret = memcpy_s(&pmb->nrPages[nid], pageNumSize, buf + tmpOffset, pageNumSize);
        if (ret) {
            return -ret;
        }
        tmpOffset += pageNumSize;
    }
    *offset += tmpOffset;
    return 0;
}

static int ParseBitmap(size_t bufLen, char *buf, size_t *offset, struct ProcessMemBitmap *pmb)
{
    size_t newOffset = *offset;

    int ret = ParseBitmapPid(pmb, buf + newOffset, &newOffset);
    if (ret) {
        SMAP_LOGGER_ERROR("ParseBitmapPid err: %d.", ret);
        return ret;
    }

    ret = ParseBitmapNrPages(pmb, buf + newOffset, &newOffset);
    if (ret) {
        SMAP_LOGGER_ERROR("ParseBitmapNrPages err: %d.", ret);
        return ret;
    }

    SMAP_LOGGER_INFO("read continue %zu %zu.", newOffset, bufLen);

    *offset = newOffset;
    return 0;
}

static int BuildAndFillBitmapBuf(size_t *len, char **buf)
{
    int ret;
    ret = BuildBitmapBuf(len, buf);
    if (ret) {
        SMAP_LOGGER_ERROR("Access ioctl walk pagemap error: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("Build bitmap buffer done.");
    ret = AccessRead(*len, *buf);
    if (ret) {
        SMAP_LOGGER_ERROR("Access read pagemap error: %d.", ret);
        free(*buf);
        return ret;
    }
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

int BuildAllPidData(void)
{
    int ret, failedCount = 0;
    char *buf;
    size_t bufLen;
    ret = BuildAndFillBitmapBuf(&bufLen, &buf);
    if (ret) {
        SMAP_LOGGER_ERROR("BuildAllPidData: build and fill BitmapBuf error: %d.", ret);
        return ret;
    }
    for (size_t offset = 0; offset < bufLen;) {
        struct ProcessMemBitmap pmb = { 0 };
        SMAP_LOGGER_INFO("Parse bitmap from %zu.", offset);
        ret = ParseBitmap(bufLen, buf, &offset, &pmb);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("parse bitmap failed.");
            failedCount++;
            break;
        }
        ProcessAttr *current = GetProcessAttr(pmb.pid);
        if (current && current->scanType == NORMAL_SCAN) {
            SetPidNrPages(current, pmb.nrPages, MAX_NODES);
            /*
             * 本地位直接由回读的各节点页数构造（nrPages[nid] != 0 即置位），
             * 如实反映本轮实际内存分布；远端位以接口配置的迁移目标
             * （targetConfig）为准，不随内核观测波动。
             */
            uint32_t numaNodes = 0;
            int nrLocalNuma = GetProcessManager()->nrLocalNuma;
            for (int nid = 0; nid < nrLocalNuma; nid++) {
                if (pmb.nrPages[nid] != 0) {
                    AddL1(&numaNodes, nid);
                }
            }
            for (uint32_t i = 0; i < current->targetConfig.count; i++) {
                AddL2ByNid(&numaNodes, current->targetConfig.targets[i].remoteNid);
            }
            /*
             * 本地位为空有两种情况：首个 walk 尚未完成（全零快照），或
             * 进程页已全部迁到远端。两种情况都保留现有本地位（纳管预置
             * 的观测过渡值/迁移前的分布），保证 GetAttrL1 始终返回合法
             * L1；待进程重新产生本地页后自然收敛到实际分布。
             */
            if ((numaNodes & BuildAllLocalNumaMask()) == 0) {
                numaNodes |= current->numaAttr.numaNodes & BuildAllLocalNumaMask();
            }
            current->numaAttr.numaNodes = numaNodes;
            SMAP_LOGGER_INFO("Pid %d, numaNodes %#x, nrLocalNuma %u.", current->pid, current->numaAttr.numaNodes,
                             GetProcessManager()->nrLocalNuma);
            ret = FillPidData(current, &pmb);
            if (ret) {
                SMAP_LOGGER_ERROR("Fill pid %d actc data failed.", current->pid);
                failedCount++;
                PutProcessAttr(current);
                continue;
            }
            if (!current->groupPolicy.enabled) {
                ret = RefreshManagedLocalStatePeriodic(current);
                if (ret) {
                    SMAP_LOGGER_ERROR("Refresh pid %d managed local state failed: %d.", current->pid, ret);
                    failedCount++;
                }
                CalibratePairAccount(current);
            }
        }
        PutProcessAttr(current);
    }
    CalcMigrateNrPagesPerPIDMuiltNuma();
    free(buf);
    return failedCount;
}
