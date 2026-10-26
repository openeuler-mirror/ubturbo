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
#include <stdbool.h>
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
#include "node_matrix.h"
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

        if (i == l1Len - 1) {
            NM_AT(sa->allocRemoteNrPages, l1Index, remoteNid) = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        } else {
            NM_AT(sa->allocRemoteNrPages, l1Index, remoteNid) = nrChunk;
            nrLeft -= nrChunk;
            SMAP_LOGGER_DEBUG("[alloc_remote*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        }
    }
    SMAP_LOGGER_INFO("Rebuild alloc account complete for pid %d", attr->pid);
}

static void CheckAccountAndNrPage(ProcessAttr *attr, bool returnFlag[SMAP_MAX_NUMNODES])
{
    int i, j;
    int nrLocalNuma = GetNrLocalNuma();
    /**
     * 每个远端numa的账本，和nrPage进行对比
     * 1、远端numa有账本，但是nrPage没有，保留旧账本（页数快照可能滞后于迁出结算，
     *    不能据此清空账本）
     * 2、nrPage有但是远端numa没有账本，
     *   1）检查L1的分布情况，平均分
     *   2）L1没有分布情况，按照CPU绑定情况，平均分
     */
    int cap = (int)GetNumaCap();
    for (j = nrLocalNuma; j < cap; j++) {
        uint32_t tmpTotal = 0;
        double ratio;
        if (NotInAttrL2(attr, j)) {
            continue;
        }
        if (attr->walkPage.nrPages[j] == 0) {
            continue;
        }
        for (i = 0; i < nrLocalNuma; i++) {
            tmpTotal += NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, j);
        }
        // nrPage有但是远端numa没有账本
        if (tmpTotal == 0) {
            NoAccountAlloc(j, attr);
            continue;
        }
        returnFlag[j] = false;
    }
}

static void CalNrPagesPerLocalNuma(ProcessAttr *attr)
{
    StrategyAttribute *sa = &attr->strategyAttr;
    int cap = (int)GetNumaCap();
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();

    for (int i = 0; i < nrLocalNuma; i++) {
        uint64_t whiteNum = attr->scanAttr.actCount[i].whiteNum;
        /* 饱和减法：扫描失败或总页数为0时旧 whiteNum 可能残留，防止下溢成数十亿可迁页 */
        uint32_t nrLocal = attr->walkPage.nrPages[i] > whiteNum ? (uint32_t)(attr->walkPage.nrPages[i] - whiteNum) : 0;
        uint32_t nrRemote = 0;

        for (int j = nrLocalNuma; j < cap; j++) {
            nrRemote += NM_AT(sa->allocRemoteNrPages, i, j);
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
 * @param attr:     进程结构体指针
 * @param remoteNid: 远端NUMA的绝对nid
 * @param ratio:    比例数组
 * @param len:      比例数组长度
 */
static void CalRemoteAllocRatio(ProcessAttr *attr, int remoteNid, double *ratio, int *len)
{
    int i;
    int nrLocalNuma = GetNrLocalNuma();
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t acctTotal = 0;

    for (i = 0; i < nrLocalNuma; i++) {
        acctTotal += NM_AT(sa->remoteNrPagesAfterMigrate, i, remoteNid);
    }

    if (acctTotal == 0) {
        *len = 0;
    } else {
        for (i = 0; i < nrLocalNuma; i++) {
            ratio[i] = (double)NM_AT(sa->remoteNrPagesAfterMigrate, i, remoteNid) / acctTotal;
            SMAP_LOGGER_DEBUG("[alloc_remote_ratio] pid=%d local=%d remote=%d pages=%u/%u remote_ratio=%.2lf",
                              attr->pid, i, remoteNid, NM_AT(sa->remoteNrPagesAfterMigrate, i, remoteNid), acctTotal,
                              ratio[i]);
        }
        *len = i;
    }
}

/**
 * CalRemoteAllocPages - 根据比例计算各本地NUMA的迁移页数
 *
 * @param attr:     进程结构体指针
 * @param remoteNid: 远端NUMA的绝对nid
 * @param ratio:    比例数组
 * @param len:      比例数组长度
 */
static void CalRemoteAllocPages(ProcessAttr *attr, int remoteNid, double *ratio, int ratioLen)
{
    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTotal = attr->walkPage.nrPages[remoteNid];
    uint32_t nrLeft = nrTotal;

    for (int i = 0; i < ratioLen; i++) {
        if (ratio[i] == 0) {
            continue;
        }

        if (i == ratioLen - 1) {
            NM_AT(sa->allocRemoteNrPages, i, remoteNid) = nrLeft;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrLeft);
        } else {
            uint32_t nrTmp = nrTotal * ratio[i];
            NM_AT(sa->allocRemoteNrPages, i, remoteNid) = nrTmp;
            nrLeft -= nrTmp;
            SMAP_LOGGER_DEBUG("[alloc_remote] pid=%d local=%d remote=%d pages=%u", attr->pid, i, remoteNid, nrTmp);
        }
    }
}

void CalRemotePerLocalWithAccount(int remoteNid, ProcessAttr *attr)
{
    double ratioArr[SMAP_MAX_NUMNODES];
    int ratioLen = 0;

    // 1. 根据 remoteNrPagesAfterMigrate 账本计算分配比例
    CalRemoteAllocRatio(attr, remoteNid, ratioArr, &ratioLen);
    if (ratioLen == 0) {
        return;
    }
    // 2. 根据分配比例计算分配的页面数量
    CalRemoteAllocPages(attr, remoteNid, ratioArr, ratioLen);
}

static void CalRemotePerLocal(ProcessAttr *attr)
{
    int i, j;
    bool returnFlag[SMAP_MAX_NUMNODES];

    int cap = (int)GetNumaCap();
    for (i = 0; i < cap; i++) {
        returnFlag[i] = true;
    }

    // 检查账本和当前内存页分布情况，处理有远端内存，但是没有账本的情况
    CheckAccountAndNrPage(attr, returnFlag);
    int nrLocalNuma = GetNrLocalNuma();

    for (j = nrLocalNuma; j < cap; j++) {
        // 1、Pid本地单一numa占比：pid->本地numa/sum(所有本地numa总迁出量)
        if (NotInAttrL2(attr, j)) {
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

void CalRemoteNumaAllocPerPid(int i, int j, uint32_t tmpNrPagesToUse, uint32_t *tmpMaxAllocNrPages)
{
    if (GetProcessManager()->nr[VM_TYPE] + GetProcessManager()->nr[PROCESS_TYPE] == 0) {
        return;
    }
    double tmpRatioPerPid;

    // 再按比例分配tmpNrPagesToUse
    if (NM_AT(tmpMaxAllocNrPages, i, j) == 0) {
        return;
    }
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    // 根据比例计算每个PID的迁出比例，更新迁出的比例到l2RemoteMemRatio
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (attr->strategyAttr.nrPagesPerLocalNuma[i] == 0) { // 无可支配内存不参与分摊，且防 0/0 产生 NaN 污染 ratio
            continue;
        }
        // 1）每个PID最大迁出量/总最大迁出量 = 最大迁出量比例
        tmpRatioPerPid =
            (double)attr->strategyAttr.nrPagesPerLocalNuma[i] *
            ((NM_AT(attr->strategyAttr.initRemoteMemRatio, i, j) - NM_AT(attr->strategyAttr.l2RemoteMemRatio, i, j)) /
             HUNDRED) /
            NM_AT(tmpMaxAllocNrPages, i, j);
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 1: %u [%d][%d]: %.2lf %u.", tmpNrPagesToUse, i, j, tmpRatioPerPid,
                          attr->strategyAttr.nrPagesPerLocalNuma[i]);

        // 2）最大迁出量比例 * numa可用量 = 每个PID可用的量（即numa迁出的ratio ）
        NM_AT(attr->strategyAttr.l2RemoteMemRatio, i, j) +=
            ((double)tmpNrPagesToUse * tmpRatioPerPid / attr->strategyAttr.nrPagesPerLocalNuma[i]) * HUNDRED;
        SMAP_LOGGER_DEBUG("CalRemoteNumaAllocPerPid 2: %.2lf.", NM_AT(attr->strategyAttr.l2RemoteMemRatio, i, j));
    }
    PidSlotReleaseRefs(all, n);
}

static void CalAvailBorrowPage(uint32_t *availPrivatePages, uint32_t availSharedPages[SMAP_MAX_NUMNODES])
{
    struct RemoteNumaInfo *rmi = &GetProcessManager()->remoteNumaInfo;

    int cap = (int)GetNumaCap();
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    for (int j = nrLocalNuma; j < cap; j++) {
        if (rmi->sharedSize[j] > 0) {
            availSharedPages[j] = MBToPage(rmi->sharedSize[j]);
            SMAP_LOGGER_DEBUG("availSharedPages[%d] %llu", j, availSharedPages[j]);
        }
        for (int i = 0; i < nrLocalNuma && i < SMAP_MAX_NUMNODES; i++) {
            if (NM_AT(rmi->privateSize, i, j) > 0) {
                NM_AT(availPrivatePages, i, j) = MBToPage(NM_AT(rmi->privateSize, i, j));
                SMAP_LOGGER_DEBUG("availPrivatePages[%d][%d] %llu", i, j, NM_AT(availPrivatePages, i, j));
            }
        }
    }
}

static void AllocPrivatePage(uint32_t *tmpMaxAllocNrPages, uint32_t *availPrivatePages)
{
    int cap = (int)GetNumaCap();
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < nrLocalNuma; i++) {
        for (int j = nrLocalNuma; j < cap; j++) {
            if (NM_AT(tmpMaxAllocNrPages, i, j) == 0) {
                continue;
            }
            SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u.", i, j, NM_AT(tmpMaxAllocNrPages, i, j));
            SMAP_LOGGER_DEBUG("availPrivatePages 2 %llu.", NM_AT(availPrivatePages, i, j));
            if (NM_AT(availPrivatePages, i, j) == 0) {
                continue;
            }

            uint32_t tmpNrPagesToUse;
            // If 每个numa最大迁出量 > 专属numa：
            if (NM_AT(tmpMaxAllocNrPages, i, j) > NM_AT(availPrivatePages, i, j)) {
                tmpNrPagesToUse = NM_AT(availPrivatePages, i, j);
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                NM_AT(tmpMaxAllocNrPages, i, j) -= NM_AT(availPrivatePages, i, j);
            } else {
                // If 专属numa  > 每个numa最大迁出量：直接迁（迁出的ratio + remote_numa ID）
                tmpNrPagesToUse = NM_AT(tmpMaxAllocNrPages, i, j);
                CalRemoteNumaAllocPerPid(i, j, tmpNrPagesToUse, tmpMaxAllocNrPages);
                NM_AT(tmpMaxAllocNrPages, i, j) = 0;
            }
        }
    }
}

static void AllocBorrowPage(uint32_t *tmpMaxAllocNrPages, uint32_t *availPrivatePages,
                            uint32_t availSharedPages[SMAP_MAX_NUMNODES])
{
    int i, j;
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    // 优先使用专属的远端内存
    AllocPrivatePage(tmpMaxAllocNrPages, availPrivatePages);
    double tmpRatioPerLocalNuma[SMAP_MAX_NUMNODES];
    // 再使用共享远端内存
    int cap = (int)GetNumaCap();
    for (j = 0; j < cap; j++) {
        uint32_t tmpNrPagesCanMigOut = 0;
        if (availSharedPages[j] == 0) {
            continue;
        }
        for (i = 0; i < nrLocalNuma; i++) {
            tmpRatioPerLocalNuma[i] = 0;
            tmpNrPagesCanMigOut += NM_AT(tmpMaxAllocNrPages, i, j);
        }
        if (tmpNrPagesCanMigOut == 0) {
            continue;
        }
        for (i = 0; i < nrLocalNuma; i++) {
            tmpRatioPerLocalNuma[i] = (double)NM_AT(tmpMaxAllocNrPages, i, j) / tmpNrPagesCanMigOut;
            // 将共享远端内存，分给每个本地numa去迁出，按照各本地numa可迁出的比例分配
            uint32_t canUsePage = tmpRatioPerLocalNuma[i] * availSharedPages[j];
            SMAP_LOGGER_INFO("tmpRatioPerLocalNuma[%d] %.2lf, tmpNrPagesCanMigOut: %u, SharedBorrow[%d]: %u.", i,
                             tmpRatioPerLocalNuma[i], tmpNrPagesCanMigOut, j, availSharedPages[j]);
            if (canUsePage > NM_AT(tmpMaxAllocNrPages, i, j)) {
                CalRemoteNumaAllocPerPid(i, j, NM_AT(tmpMaxAllocNrPages, i, j), tmpMaxAllocNrPages);
            } else {
                CalRemoteNumaAllocPerPid(i, j, canUsePage, tmpMaxAllocNrPages);
            }
        }
    }
}

void AllocBorrowPagesForMemsize(ProcessAttr *attr, uint32_t *availPrivatePages,
                                uint32_t availSharedPages[SMAP_MAX_NUMNODES])
{
    int destNid = attr->migrateParam[0].nid;
    if (destNid < 0 || destNid >= SMAP_MAX_NUMNODES) {
        return;
    }

    StrategyAttribute *sa = &attr->strategyAttr;
    uint32_t nrTarget = KBToPage(attr->migrateParam[0].memSize);

    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < nrLocalNuma && i < SMAP_MAX_NUMNODES; i++) {
        if (!InAttrL1(attr, i)) {
            NM_AT(sa->memSize, i, destNid) = 0;
            SMAP_LOGGER_INFO("[memsize_clear] pid=%d local=%d remote=%d pages=0", attr->pid, i, destNid);
            continue;
        }

        // 先使用私有借用内存
        uint32_t nrTotal = sa->nrPagesPerLocalNuma[i];
        uint32_t nrAvail = MIN(nrTotal, NM_AT(availPrivatePages, i, destNid));
        SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         destNid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            nrTotal -= nrAvail;
            NM_AT(availPrivatePages, i, destNid) -= nrAvail;
            NM_AT(sa->memSize, i, destNid) = PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_private] pid=%d local=%d remote=%d pages=%u", attr->pid, i, destNid, nrAvail);
        } else {
            NM_AT(availPrivatePages, i, destNid) -= nrTarget;
            NM_AT(sa->memSize, i, destNid) = PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_private*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, destNid, nrTarget);
            break;
        }

        if (nrTotal == 0) {
            continue;
        }

        // 再使用共享借用内存
        nrAvail = MIN(nrTotal, availSharedPages[destNid]);
        SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d nrTarget=%u nrTotal=%u nrAvail=%u", attr->pid, i,
                         destNid, nrTarget, nrTotal, nrAvail);

        if (nrTarget >= nrAvail) {
            nrTarget -= nrAvail;
            availSharedPages[destNid] -= nrAvail;
            NM_AT(sa->memSize, i, destNid) += PageToKB(nrAvail);
            SMAP_LOGGER_INFO("[memsize_shared] pid=%d local=%d remote=%d pages=%u", attr->pid, i, destNid, nrAvail);
        } else {
            availSharedPages[destNid] -= nrTarget;
            NM_AT(sa->memSize, i, destNid) += PageToKB(nrTarget);
            SMAP_LOGGER_INFO("[memsize_shared*] pid=%d local=%d remote=%d pages=%u", attr->pid, i, destNid, nrTarget);
            break;
        }
    }
}

void CalRemoteNumaSizeAllocPerNuma(void)
{
    /* 节点矩阵按 SMAP_MAX_NUMNODES 行 × cap 列扁平分配（列跨度 = cap） */
    uint32_t *tmpMaxAllocNrPages = calloc(NM_COUNT(SMAP_MAX_NUMNODES), sizeof(*tmpMaxAllocNrPages));
    uint32_t *availPrivatePages = calloc(NM_COUNT(SMAP_MAX_NUMNODES), sizeof(*availPrivatePages));
    uint32_t *availSharedPages = calloc(SMAP_MAX_NUMNODES, sizeof(*availSharedPages));
    if (tmpMaxAllocNrPages == NULL || availPrivatePages == NULL || availSharedPages == NULL) {
        SMAP_LOGGER_ERROR("Alloc numa size arrays failed.");
        free(tmpMaxAllocNrPages);
        free(availPrivatePages);
        free(availSharedPages);
        return;
    }
    CalAvailBorrowPage(availPrivatePages, availSharedPages);
    int i, j;

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
    int cap = (int)GetNumaCap();
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        if (attr->migrateMode == MIG_MEMSIZE_MODE) {
            continue;
        }

        StrategyAttribute *sa = &attr->strategyAttr;
        for (i = 0; i < nrLocalNuma; i++) {
            for (j = 0; j < cap; j++) {
                NM_AT(tmpMaxAllocNrPages, i, j) +=
                    sa->nrPagesPerLocalNuma[i] * NM_AT(sa->initRemoteMemRatio, i, j) / HUNDRED;
                SMAP_LOGGER_DEBUG("tmpMaxAllocNrPages[%d][%d]=%u, initRemoteMemRatio=%.2lf.", i, j,
                                  NM_AT(tmpMaxAllocNrPages, i, j), NM_AT(sa->initRemoteMemRatio, i, j));

                NM_AT(sa->l2RemoteMemRatio, i, j) = 0;
            }
        }
    }

    PidSlotReleaseRefs(all, n);

    // 用远端借用的内存计算每个pid，每个numa可迁出的比例
    AllocBorrowPage(tmpMaxAllocNrPages, availPrivatePages, availSharedPages);
    free(tmpMaxAllocNrPages);
    free(availPrivatePages);
    free(availSharedPages);
}

void ClearRemoteMemUsed(void)
{
    int nrLocal = GetProcessManager()->nrLocalNuma;
    struct RemoteNumaInfo *info = &GetProcessManager()->remoteNumaInfo;
    /* privateUsedInfo 由 InitRemoteNumaInfoArrays 在内核上报 nrLocalNuma 后
     * 延迟分配；未分配时（如运行早期/DT 环境）无从清理，直接返回 */
    if (info->privateUsedInfo == NULL) {
        int cap = (int)GetNumaCap();
        for (int j = 0; j < cap; j++) {
            info->usedInfo[j].used = 0;
            info->usedInfo[j].ifUsedFreshed = true;
        }
        SMAP_LOGGER_DEBUG("Smap clear remote mem used end (private arrays not allocated).");
        return;
    }
    int cap = (int)GetNumaCap();
    for (int j = 0; j < cap; j++) {
        info->usedInfo[j].used = 0;
        info->usedInfo[j].ifUsedFreshed = true;
        /* privateUsedInfo 第一维仅分配 nrLocalNuma 行 */
        for (int i = 0; i < nrLocal; i++) {
            NM_AT(info->privateUsedInfo, i, j).used = 0;
            NM_AT(info->privateUsedInfo, i, j).ifUsedFreshed = true;
        }
    }
    SMAP_LOGGER_DEBUG("Smap clear remote mem used end.");
}

void CalRemoteMemUsed(void)
{
    struct RemoteNumaInfo *remoteNumaInfo = &GetProcessManager()->remoteNumaInfo;
    int i, j;

    int nrLocal = GetProcessManager()->nrLocalNuma;
    /* privateUsedInfo 由 InitRemoteNumaInfoArrays 延迟分配，未分配时只累计
     * usedInfo（远端总账），跳过 per-local 私账统计 */
    bool privateReady = remoteNumaInfo->privateUsedInfo != NULL;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(GetProcessManager(), all, MAX_PID_SLOTS);
    /* cap 提到循环外：避免每个进程重复调用 GetNumaCap() */
    int cap = (int)GetNumaCap();
    // 计算每个本地远端对应按照ratio可迁出的最大量
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *attr = all[k]->attr;
        /* j 为远端绝对 nid，从 nrLocal 起遍历远端空间 */
        for (j = nrLocal; j < cap; j++) {
            remoteNumaInfo->usedInfo[j].used += attr->walkPage.nrPages[j];
            SMAP_LOGGER_DEBUG("usedInfo[%d].used: %llu, nrPages[%d]: %u.", j, remoteNumaInfo->usedInfo[j].used, j,
                              attr->walkPage.nrPages[j]);
            if (!privateReady) {
                continue;
            }
            /* allocRemoteNrPages/privateUsedInfo 第一维仅分配 nrLocalNuma 行 */
            for (i = 0; i < nrLocal; i++) {
                // 用迁回结算值 remoteNrPagesAfterMigrate 而非 allocRemoteNrPages：
                // 后者由 RebuildAllocAccount 从 walkPage 快照均分派生，扫描窗口滞后于
                // 迁回，导致迁回成功后 used 仍卡在旧快照派生值（如 36097/2=18048），
                // 使 CheckPrivateBorrowUsed 误判 used>size 而 reject migrate_back。
                NM_AT(remoteNumaInfo->privateUsedInfo, i, j).used +=
                    NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, j);
                SMAP_LOGGER_DEBUG("privateUsedInfo[%d][%d].used: %llu, remoteNrPagesAfterMigrate[%d][%d]: %u.", i, j,
                                  NM_AT(remoteNumaInfo->privateUsedInfo, i, j).used, i, j,
                                  NM_AT(attr->strategyAttr.remoteNrPagesAfterMigrate, i, j));
            }
        }
    }
    PidSlotReleaseRefs(all, n);
}

void CalcMigrateNrPagesPerPidMultiNuma(void)
{
    struct RemoteNumaInfo *numaInfo = &GetProcessManager()->remoteNumaInfo;
    if (GetProcessManager()->nr[VM_TYPE] + GetProcessManager()->nr[PROCESS_TYPE] == 0) {
        // 无管理 pid：仍须清空残留 used，否则 remote_numa_info_set 把 size
        // 清零后，上一周期残留的 used 会让 CheckPrivateBorrowUsed/CheckBorrowUsed
        // 误判 used>size 而 reject migrate_back。
        EnvMutexLock(&numaInfo->lock);
        ClearRemoteMemUsed();
        EnvMutexUnlock(&numaInfo->lock);
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

/* 持远端账本锁重建已用页数，并扣除在途预扣后返回可用页数。
 * privateAvail 为 nrLocalNuma 行 × cap 列扁平矩阵（NM_AT 访问），
 * sharedAvail 按远端绝对 nid 定维（长度 cap），远端空间为 [nrLocal, cap)。 */
void SnapshotRemoteCapacityLocked(uint64_t *privateAvail, uint64_t *sharedAvail)
{
    struct RemoteNumaInfo *remoteNumaInfo = &GetProcessManager()->remoteNumaInfo;
    int nrLocal = GetNrLocalNuma();
    int cap = (int)GetNumaCap();

    ClearRemoteMemUsed();
    CalRemoteMemUsed();
    for (int remote = nrLocal; remote < cap; remote++) {
        uint64_t sharedCapPages = MBToPage(remoteNumaInfo->sharedSize[remote]);
        uint64_t usedSharedPages = remoteNumaInfo->usedInfo[remote].used;
        uint64_t inFlightSharedPages = 0;

        for (int local = 0; local < nrLocal; local++) {
            uint64_t privateCapPages = MBToPage(NM_AT(remoteNumaInfo->privateSize, local, remote));
            uint64_t usedPrivatePages = NM_AT(remoteNumaInfo->privateUsedInfo, local, remote).used +
                                        NM_AT(remoteNumaInfo->privateInFlight, local, remote);

            NM_AT(privateAvail, local, remote) =
                privateCapPages > usedPrivatePages ? privateCapPages - usedPrivatePages : 0;
            inFlightSharedPages += NM_AT(remoteNumaInfo->privateInFlight, local, remote);
        }
        sharedAvail[remote] = sharedCapPages > (usedSharedPages + inFlightSharedPages) ?
                                  sharedCapPages - usedSharedPages - inFlightSharedPages :
                                  0;
    }
}

/* 迁移结算后回收全部在途预扣；成功迁移的页数由后续快照重新入账。
 * planned 为 nrLocalNuma 行 × cap 列扁平矩阵（NM_AT 访问）。 */
void LedgerSettleInFlight(const uint32_t *planned, int nrLocalNuma)
{
    struct RemoteNumaInfo *remoteNumaInfo = &GetProcessManager()->remoteNumaInfo;
    int cap = (int)GetNumaCap();

    EnvMutexLock(&remoteNumaInfo->lock);
    for (int j = nrLocalNuma; j < cap; j++) {
        for (int i = 0; i < nrLocalNuma; i++) {
            uint32_t plannedPages = NM_AT(planned, i, j);
            if (plannedPages == 0) {
                continue;
            }
            uint64_t *inFlight = &NM_AT(remoteNumaInfo->privateInFlight, i, j);
            *inFlight = *inFlight >= plannedPages ? *inFlight - plannedPages : 0;
        }
    }
    EnvMutexUnlock(&remoteNumaInfo->lock);
}

/* 规划成功后按本地→远端绝对 nid 对累加在途预扣；调用方须持账本锁。 */
void LedgerPlanInFlightLocked(const uint32_t *nrMigratePages, int nrLocalNuma)
{
    struct RemoteNumaInfo *remoteNumaInfo = &GetProcessManager()->remoteNumaInfo;
    int cap = (int)GetNumaCap();

    for (int j = nrLocalNuma; j < cap; j++) {
        for (int i = 0; i < nrLocalNuma; i++) {
            NM_AT(remoteNumaInfo->privateInFlight, i, j) += NM_AT(nrMigratePages, i, j);
        }
    }
}
