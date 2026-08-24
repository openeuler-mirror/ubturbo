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

static void NoAccountAlloc(int remoteNid, ProcessAttr *attr)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    int l1Nid[nrLocalNuma];
    int l1Len = 0;

    for (i = 0; i < nrLocalNuma; i++) {
        if (InAttrL1(attr, i)) {
            l1Nid[l1Len++] = i;
        }
    }
    if (l1Len == 0) {
        SMAP_LOGGER_WARNING("Aborted alloc account rebuild for pid %d due to missing L1 node", attr->pid);
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrLeft = attr->walkPage.nrPages[remoteNid];
    uint32_t nrChunk = nrLeft / l1Len;

    SMAP_LOGGER_INFO("Rebuilding alloc account for pid %d", attr->pid);
    for (i = 0; i < l1Len; i++) {
        int l1Index = l1Nid[i];
        int l2Index = remoteNid - nrLocalNuma;

        if (i == l1Len - 1) {
            sa->allocRemoteNrPages[l1Index][l2Index] = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        } else {
            sa->allocRemoteNrPages[l1Index][l2Index] = nrChunk;
            nrLeft -= nrChunk;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        }
    }
    SMAP_LOGGER_INFO("Rebuild alloc account complete for pid %d", attr->pid);
}

static void ClearNormalPidAccount(ProcessAttr *attr, int remoteNode, int nrLocalNuma)
{
    if (attr->state != PROC_MOVE) {
        for (int i = 0; i < nrLocalNuma; i++) {
            attr->strategyAttr.remoteNrPagesAfterMigrate[i][remoteNode] = 0;
        }
    }
}

static void CheckAccountAndNrPage(ProcessAttr *attr, bool returnFlag[REMOTE_NUMA_NUM])
{
    int i, j;
    int nrLocalNuma = GetNrLocalNuma();
    /**
     * 每个远端numa的账本，和nrPage进行对比
     * 1、远端numa有账本，但是nrPage没有，对账本进行清零
     * 2、nrPage有但是远端numa没有账本，
     *   1）检查L1的分布情况，平均分
     *   2）L1没有分布情况，按照CPU绑定情况，平均分
     */
    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        int remoteNid = nrLocalNuma + j;
        uint32_t tmpTotal = 0;
        double ratio;
        if (NotInAttrL2(attr, remoteNid)) {
            continue;
        }
        if (attr->walkPage.nrPages[remoteNid] == 0) {
            ClearNormalPidAccount(attr, j, nrLocalNuma);
            continue;
        }
        for (i = 0; i < nrLocalNuma; i++) {
            tmpTotal += attr->strategyAttr.remoteNrPagesAfterMigrate[i][j];
        }
        // nrPage有但是远端numa没有账本
        if (tmpTotal == 0) {
            NoAccountAlloc(remoteNid, attr);
            continue;
        }
        returnFlag[j] = false;
    }
}

static void CalNrPagesPerLocalNuma(ProcessAttr *attr)
{
    StrategyAttribute *sa = &attr->strategyAttr;

    for (int i = 0; i < GetNrLocalNuma(); i++) {
        uint64_t whiteNum = attr->scanAttr.actCount[i].whiteNum;
        /* 饱和减法：扫描失败或总页数为0时旧 whiteNum 可能残留，防止下溢成数十亿可迁页 */
        uint32_t nrLocal = attr->walkPage.nrPages[i] > whiteNum ? (uint32_t)(attr->walkPage.nrPages[i] - whiteNum) : 0;
        uint32_t nrRemote = 0;

        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            nrRemote += sa->allocRemoteNrPages[i][j];
        }
        sa->nrPagesPerLocalNuma[i] = nrLocal + nrRemote;

        SMAP_LOGGER_DEBUG(
            "[cal_local_total] pid=%d, local_node=%d total_pages=%u local_pages=%u white=%llu remote_pages=%u",
            attr->pid, i, sa->nrPagesPerLocalNuma[i], nrLocal, whiteNum, nrRemote);
    }
}

/**
 * CalRemoteAllocRatio - 根据历史贡献度计算各本地NUMA迁往某远端NUMA的比例
 *
 * @param attr:    进程结构体指针
 * @param l2Index: L2索引
 * @param ratio:   比例数组
 * @param len:     比例数组长度
 */
static void CalRemoteAllocRatio(ProcessAttr *attr, int l2Index, double *ratio, int *len)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    int l2Nid = l2Index + nrLocalNuma;
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t acctTotal = 0;

    for (i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
        acctTotal += sa->remoteNrPagesAfterMigrate[i][l2Index];
    }

    if (acctTotal == 0) {
        *len = 0;
    } else {
        for (i = 0; i < nrLocalNuma && i < LOCAL_NUMA_NUM; i++) {
            ratio[i] = (double)sa->remoteNrPagesAfterMigrate[i][l2Index] / acctTotal;
            SMAP_LOGGER_DEBUG("[alloc_remote_ratio] pid=%d local=%d remote=%d pages=%u/%u remote_ratio=%.2lf",
                              attr->pid, i, l2Nid, sa->remoteNrPagesAfterMigrate[i][l2Index], acctTotal, ratio[i]);
        }
        *len = i;
    }
}

/**
 * CalRemoteAllocPages - 根据比例计算各本地NUMA的迁移页数
 *
 * @param attr:    进程结构体指针
 * @param l2Index: L2索引
 * @param ratio:   比例数组
 * @param len:     比例数组长度
 */
static void CalRemoteAllocPages(ProcessAttr *attr, int l2Index, double *ratio, int ratioLen)
{
    int l2Nid = l2Index + GetNrLocalNuma();
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTotal = attr->walkPage.nrPages[l2Nid];
    uint32_t nrLeft = nrTotal;

    for (int i = 0; i < ratioLen; i++) {
        if (ratio[i] == 0) {
            continue;
        }

        if (i == ratioLen - 1) {
            sa->allocRemoteNrPages[i][l2Index] = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrLeft);
        } else {
            uint32_t nrTmp = nrTotal * ratio[i];
            sa->allocRemoteNrPages[i][l2Index] = nrTmp;
            nrLeft -= nrTmp;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTmp);
        }
    }
}

void CalRemotePerLocalWithAccount(int l2Index, ProcessAttr *attr)
{
    double ratioArr[LOCAL_NUMA_NUM];
    int ratioLen = 0;

    // 1. 根据 remoteNrPagesAfterMigrate 账本计算分配比例
    CalRemoteAllocRatio(attr, l2Index, ratioArr, &ratioLen);
    if (ratioLen == 0) {
        return;
    }
    // 2. 根据分配比例计算分配的页面数量
    CalRemoteAllocPages(attr, l2Index, ratioArr, ratioLen);
}

static void CalRemotePerLocal(ProcessAttr *attr)
{
    int i, j;
    bool returnFlag[REMOTE_NUMA_NUM];

    for (i = 0; i < REMOTE_NUMA_NUM; i++) {
        returnFlag[i] = true;
    }

    // 检查账本和当前内存页分布情况，处理有远端内存，但是没有账本的情况
    CheckAccountAndNrPage(attr, returnFlag);
    int nrLocalNuma = GetNrLocalNuma();

    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        // 1、Pid本地单一numa占比：pid->本地numa/sum(所有本地numa总迁出量)
        if (NotInAttrL2(attr, nrLocalNuma + j)) {
            continue;
        }
        if (returnFlag[j]) {
            continue;
        }
        // 有账本，并且nrPage[remoteNid] != 0，计算allocRemoteNrPages
        CalRemotePerLocalWithAccount(j, attr);
    }
}

void CalNrPagesLocalTotalPerPid(ProcessAttr *attr)
{
    // 计算每个本地numa，对应可迁出到远端每个numa的内存量
    CalRemotePerLocal(attr);

    // pid本地numa总使用量：Pid本地numa数量+Pid远端使用量
    CalNrPagesPerLocalNuma(attr);
}

void CalNrPagesLocalTotal(void)
{
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);

    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (IsMultiNumaVm(attr) && GetRunMode() == MEM_POOL_MODE) {
            continue;
        }
        SMAP_LOGGER_DEBUG("CalNrPagesLocalTotal pid: %d.", attr->pid);
        CalNrPagesLocalTotalPerPid(attr);
    }
    PidSlotReleaseRefs(all, n);
}

void CalRemoteNumaAllocPerPid(int i, int j, uint32_t tmpNrPagesToUse,
                              uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM])
{
    if (GetProcessManager()->nr[VM_TYPE] + GetProcessManager()->nr[PROCESS_TYPE] == 0) {
        return;
    }
    double tmpRatioPerPid;

    // 再按比例分配tmpNrPagesToUse
    if (tmpMaxAllocNrPages[i][j] == 0) {
        return;
    }
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    // 根据比例计算每个PID的迁出比例，更新迁出的比例到l2RemoteMemRatio
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        // 1）每个PID最大迁出量/总最大迁出量 = 最大迁出量比例
        tmpRatioPerPid =
            (double)attr->strategyAttr.nrPagesPerLocalNuma[i] *
            ((attr->strategyAttr.initRemoteMemRatio[i][j] - attr->strategyAttr.l2RemoteMemRatio[i][j]) / HUNDRED) /
            tmpMaxAllocNrPages[i][j];
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 1: %u [%d][%d]: %.2lf %u.", tmpNrPagesToUse, i, j, tmpRatioPerPid,
                          attr->strategyAttr.nrPagesPerLocalNuma[i]);

        // 2）最大迁出量比例 * numa可用量 = 每个PID可用的量（即numa迁出的ratio ）
        attr->strategyAttr.l2RemoteMemRatio[i][j] +=
            ((double)tmpNrPagesToUse * tmpRatioPerPid / attr->strategyAttr.nrPagesPerLocalNuma[i]) * HUNDRED;
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 2: %.2lf.", attr->strategyAttr.l2RemoteMemRatio[i][j]);
    }
    PidSlotReleaseRefs(all, n);
}

static void CalAvailBorrowPage(uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                               uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    struct RemoteNumaInfo *rmi = &GetProcessManager()->remoteNumaInfo;

    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        if (rmi->sharedSize[j] > 0) {
            availSharedPages[j] = MBToPage(rmi->sharedSize[j]);
            SMAP_LOGGER_DEBUG("availSharedPages[%d] %llu", j, availSharedPages[j]);
        }
        for (int i = 0; i < GetNrLocalNuma() && i < LOCAL_NUMA_NUM; i++) {
            if (rmi->privateSize[i][j] > 0) {
                availPrivatePages[i][j] = MBToPage(rmi->privateSize[i][j]);
                SMAP_LOGGER_DEBUG("availPrivatePages[%d][%d] %llu", i, j, availPrivatePages[i][j]);
            }
        }
    }
}

static void AllocPrivatePage(uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                             uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM])
{
    for (int i = 0; i < GetNrLocalNuma(); i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            if (tmpMaxAllocNrPages[i][j] == 0) {
                continue;
            }
            SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u.", i, j, tmpMaxAllocNrPages[i][j]);
            SMAP_LOGGER_DEBUG("availPrivatePages 2 %llu.", availPrivatePages[i][j]);
            if (availPrivatePages[i][j] == 0) {
                continue;
            }

            uint32_t tmpNrPagesToUse;
            // If 每个numa最大迁出量 > 专属numa：
            if (tmpMaxAllocNrPages[i][j] > availPrivatePages[i][j]) {
                tmpNrPagesToUse = availPrivatePages[i][j];
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                tmpMaxAllocNrPages[i][j] -= availPrivatePages[i][j];
            } else {
                // If 专属numa  > 每个numa最大迁出量：直接迁（迁出的ratio + remote_numa ID）
                tmpNrPagesToUse = tmpMaxAllocNrPages[i][j];
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                tmpMaxAllocNrPages[i][j] = 0;
            }
        }
    }
}

static void AllocBorrowPage(uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                            uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                            uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    int i, j;
    // 优先使用专属的远端内存
    AllocPrivatePage(tmpMaxAllocNrPages, availPrivatePages);
    double tmpRatioPerLocalNuma[LOCAL_NUMA_NUM];
    // 再使用共享远端内存
    for (j = 0; j < REMOTE_NUMA_NUM; j++) {
        uint32_t tmpNrPagesCanMigOut = 0;
        if (availSharedPages[j] == 0) {
            continue;
        }
        for (i = 0; i < GetNrLocalNuma(); i++) {
            tmpRatioPerLocalNuma[i] = 0;
            tmpNrPagesCanMigOut += tmpMaxAllocNrPages[i][j];
        }
        if (tmpNrPagesCanMigOut == 0) {
            continue;
        }
        for (i = 0; i < GetNrLocalNuma(); i++) {
            tmpRatioPerLocalNuma[i] = (double)tmpMaxAllocNrPages[i][j] / tmpNrPagesCanMigOut;
            // 将共享远端内存，分给每个本地numa去迁出，按照各本地numa可迁出的比例分配
            uint32_t canUsePage = tmpRatioPerLocalNuma[i] * availSharedPages[j];
            SMAP_LOGGER_INFO("tmpRatioPerLocalNuma[%d] %.2lf, tmpNrPagesCanMigOut: %u, SharedBorrow[%d]: %u.", i,
                             tmpRatioPerLocalNuma[i], tmpNrPagesCanMigOut, j, availSharedPages[j]);
            if (canUsePage > tmpMaxAllocNrPages[i][j]) {
                CalRemoteNumaAllocPerPid(i, j, tmpMaxAllocNrPages[i][j], tmpMaxAllocNrPages);
            } else {
                CalRemoteNumaAllocPerPid(i, j, canUsePage, tmpMaxAllocNrPages);
            }
        }
    }
}

void AllocBorrowPagesForMemsize(ProcessAttr *attr, uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM],
                                uint32_t availSharedPages[REMOTE_NUMA_NUM])
{
    int l2Nid = attr->migrateParam[0].nid;
    int l2Index = l2Nid - GetNrLocalNuma();
    if (l2Index < 0 || l2Index >= REMOTE_NUMA_NUM) {
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTarget = KBToPage(attr->migrateParam[0].memSize);

    for (int i = 0; i < GetNrLocalNuma() && i < LOCAL_NUMA_NUM; i++) {
        if (!InAttrL1(attr, i)) {
            sa->memSize[i][l2Index] = 0;
            SMAP_LOGGER_INFO("[memsize_clear] pid=%d local=%d remote=%d pages=0", attr->pid, i, l2Nid);
            continue;
        }

        // 先使用私有借用内存
        uint32_t nrTotal = sa->nrPagesPerLocalNuma[i];
        uint32_t nrAvail = MIN(nrTotal, availPrivatePages[i][l2Index]);
        SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         l2Nid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            nrTotal -= nrAvail;
            availPrivatePages[i][l2Index] -= nrAvail;
            sa->memSize[i][l2Index] = PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrAvail);
        } else {
            availPrivatePages[i][l2Index] -= nrTarget;
            sa->memSize[i][l2Index] = PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_private*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTarget);
            break;
        }

        if (nrTotal == 0) {
            continue;
        }

        // 再使用共享借用内存
        nrAvail = MIN(nrTotal, availSharedPages[l2Index]);
        SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         l2Nid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            availSharedPages[l2Index] -= nrAvail;
            sa->memSize[i][l2Index] += PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrAvail);
        } else {
            availSharedPages[l2Index] -= nrTarget;
            sa->memSize[i][l2Index] += PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_shared*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, l2Nid, nrTarget);
            break;
        }
    }
}

void CalRemoteNumaSizeAllocPerNuma(void)
{
    struct RemoteNumaInfo remoteNumaInfo = GetProcessManager()->remoteNumaInfo;
    uint32_t tmpMaxAllocNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    int i, j;

    uint32_t availPrivatePages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM] = { 0 };
    uint32_t availSharedPages[REMOTE_NUMA_NUM] = { 0 };
    CalAvailBorrowPage(availPrivatePages, availSharedPages);

    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);

    // 先满足迁移模式为 MIG_MEMSIZE_MODE 的进程
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            AllocBorrowPagesForMemsize(attr, availPrivatePages, availSharedPages);
        }
    }

    // 计算所有进程**想要**从各本地NUMA迁移到各远端NUMA的总页面数量
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            continue;
        }

        StrategyAttribute *sa = &attr->strategyAttr;
        for (i = 0; i < GetNrLocalNuma(); i++) {
            for (j = 0; j < REMOTE_NUMA_NUM; j++) {
                tmpMaxAllocNrPages[i][j] += sa->nrPagesPerLocalNuma[i] * sa->initRemoteMemRatio[i][j] / HUNDRED;
                SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u, initRemoteMemRatio=%.2lf.", i, j,
                                  tmpMaxAllocNrPages[i][j], sa->initRemoteMemRatio[i][j]);

                sa->l2RemoteMemRatio[i][j] = 0;
            }
        }
    }

    PidSlotReleaseRefs(all, n);

    // 用远端借用的内存计算每个pid，每个numa可迁出的比例
    AllocBorrowPage(tmpMaxAllocNrPages, availPrivatePages, availSharedPages);
}

void ClearRemoteMemUsed(void)
{
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        GetProcessManager()->remoteNumaInfo.usedInfo[j].used = 0;
        GetProcessManager()->remoteNumaInfo.usedInfo[j].ifUsedFreshed = true;
        for (int i = 0; i < LOCAL_NUMA_NUM; i++) {
            GetProcessManager()->remoteNumaInfo.privateUsedInfo[i][j].used = 0;
            GetProcessManager()->remoteNumaInfo.privateUsedInfo[i][j].ifUsedFreshed = true;
        }
    }
    SMAP_LOGGER_DEBUG("Smap clear remote mem used end.");
}

void CalRemoteMemUsed(void)
{
    struct RemoteNumaInfo *remoteNumaInfo = &GetProcessManager()->remoteNumaInfo;
    int i, j;

    int nrLocal = GetProcessManager()->nrLocalNuma;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    // 计算每个本地远端对应按照ratio可迁出的最大量
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        for (j = 0; j < REMOTE_NUMA_NUM; j++) {
            remoteNumaInfo->usedInfo[j].used += attr->walkPage.nrPages[j + nrLocal];
            SMAP_LOGGER_DEBUG("usedInfo[%d].used: %llu, nrPages[%d+%d]: %u.", j, remoteNumaInfo->usedInfo[j].used, j,
                              nrLocal, attr->walkPage.nrPages[j + nrLocal]);
            for (i = 0; i < LOCAL_NUMA_NUM; i++) {
                remoteNumaInfo->privateUsedInfo[i][j].used += attr->strategyAttr.allocRemoteNrPages[i][j];
                SMAP_LOGGER_DEBUG("privateUsedInfo[%d][%d].used: %llu, allocRemoteNrPages[%d][%d]: %u.", i, j,
                                  remoteNumaInfo->privateUsedInfo[i][j].used, i, j,
                                  attr->strategyAttr.allocRemoteNrPages[i][j]);
            }
        }
    }
    PidSlotReleaseRefs(all, n);
}

void CalcMigrateNrPagesPerPIDMuiltNuma(void)
{
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    if (GetProcessManager()->nr[VM_TYPE] + GetProcessManager()->nr[PROCESS_TYPE] == 0) {
        return;
    }
    // 根据账本信息，计算每个PID各本地numa可支配的内存；
    CalNrPagesLocalTotal();

    if (g_runMode == MEM_POOL_MODE) {
        return;
    }
    EnvMutexLock(&numaInfo->lock);
    ClearRemoteMemUsed();
    CalRemoteMemUsed();
    // 按照远端numa的粒度，和上一步计算的迁移量，计算每个本地numa相对于1个远端numa的比例
    CalRemoteNumaSizeAllocPerNuma();
    EnvMutexUnlock(&numaInfo->lock);
}
