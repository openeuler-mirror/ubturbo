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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "smap_user_log.h"
#include "manage.h"
#include "securec.h"
#include "oom_migrate.h"

#define MAX_MOVE_PAGES_BATCH 512 /* 单次 move_pages(2) 批量上限 */
#define NUMA_MAPS_LINE_LEN 1024   /* numa_maps 单行缓冲（与 MAX_LINE_LENGTH 等宽） */

/* numa_maps 段级本地过滤的流式游标：跨批次保留当前段内未枚举完的页。 */
typedef struct {
    char line[NUMA_MAPS_LINE_LEN];
    bool hasLine;          /* line[] 持有尚未枚举完毕的段 */
    unsigned long segStart;
    uint64_t segLocal;     /* 该段落在本地节点的总页数 */
    uint64_t segEmitted;   /* 该段已枚举的本地页数 */
} NumaScanCursor;

/*
 * move_pages(2) 的 mpol flags（内核 uapi 取值，避免依赖 <numaif.h>）。
 * 只接受 MPOL_MF_MOVE / MPOL_MF_MOVE_ALL，传 MPOL_MF_STRICT 返回 EINVAL；
 * 共享映射页须带 MOVE_ALL 才允许 isolate，否则逐页 -EACCES。
 */
#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif
#ifndef MPOL_MF_MOVE_ALL
#define MPOL_MF_MOVE_ALL (1 << 2)
#endif
#define URGENT_MOVE_PAGES_FLAGS (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)

/* move_pages(2) 薄封装（mockable seam）：UT 经 run_dt.sh strip static 后拦截此处，不触达真实 syscall。 */
static long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status, int flags)
{
    return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}

/* 解析一行 numa_maps：取段起始地址，累加本地页数 = Σ N<i>=<count>，i ∈ [0, nrLocalNuma)。 */
static bool ParseNumaMapLocalCount(const char *line, int nrLocalNuma, unsigned long *segStart, uint64_t *localCount)
{
    if (sscanf_s(line, "%lx", segStart) != 1) {
        return false; /* 解析段起始地址失败 */
    }
    uint64_t total = 0;
    for (int i = 0; i < nrLocalNuma; i++) {
        char pattern[NUMA_MAPS_MAX_PATTERN_LEN];
        if (snprintf_s(pattern, sizeof(pattern), sizeof(pattern) - 1, " N%d=", i) < 0) {
            continue;
        }
        const char *sub = strstr(line, pattern);
        if (sub == NULL) {
            continue;
        }
        const char *value = sub + strlen(pattern);
        char *end = NULL;
        errno = 0;
        unsigned long c = strtoull(value, &end, 10);
        if (value == end || errno != 0) {
            continue; /* 解析 N<i>=<count> 失败，跳过该 node 字段 */
        }
        total += c;
    }
    *localCount = total;
    return true;
}

/*
 * 从已打开的 numa_maps 流中收集一批候选 vaddr（≤ MAX_MOVE_PAGES_BATCH 且 ≤ maxPages）。
 * 游标 cur 跨调用保留段内位置；fp 到 EOF 或收集满即停。返回 0，*outCnt 为本批收集数（0=无更多候选）。
 *
 * 共享页边界：段级过滤无法识别共享页归属，段内混进程共享页可能被一并迁到 L2。
 * OOM 场景可放宽（首要目标是压低本地水线、避免 kill，允许共享页短暂误迁）；
 * 水线下降后由上层把 pid 重新加入 SMAP 管理，SMAP 管理态扫描会按 pidType/pageType
 * 纠正共享页归属。即紧急迁出 = 尽力腾挪、correctness 由后续 SMAP 管理态兜底。
 */
static int CollectVaddrsBatch(FILE *fp, int nrLocalNuma, uint64_t pageSize, uint64_t maxPages, NumaScanCursor *cur,
                              uint64_t *addrs, int *outCnt)
{
    *outCnt = 0;
    int cnt = 0;
    uint64_t cap = (maxPages > MAX_MOVE_PAGES_BATCH) ? MAX_MOVE_PAGES_BATCH : maxPages;
    bool huge = IsHugeMode();

    while ((uint64_t)cnt < cap) {
        /* 先把游标里上一段未枚举完的页吐出 */
        if (cur->hasLine) {
            for (; cur->segEmitted < cur->segLocal && (uint64_t)cnt < cap; cur->segEmitted++) {
                addrs[cnt++] = (uint64_t)(cur->segStart + cur->segEmitted * pageSize);
            }
            if (cur->segEmitted >= cur->segLocal) {
                cur->hasLine = false; /* 本段枚举完，读下一行 */
            }
            continue;
        }
        char *ret = fgets(cur->line, sizeof(cur->line), fp);
        if (ret == NULL) {
            break; /* EOF */
        }
        /* 真实 fgets 返回 cur->line 本址；UT mock 下返回预设行串（可能不写缓冲）。统一解析返回值。 */
        const char *line = ret;
        if (huge && !IsNumaMapLineHuge((char *)line)) {
            continue; /* 大页模式只取 huge 段 */
        }
        unsigned long segStart = 0;
        uint64_t localCount = 0;
        if (!ParseNumaMapLocalCount(line, nrLocalNuma, &segStart, &localCount)) {
            continue; /* 解析段起始失败 */
        }
        if (localCount == 0) {
            continue; /* 该段无本地页，整段跳过（不误碰纯远端段） */
        }
        cur->hasLine = true;
        cur->segStart = segStart;
        cur->segLocal = localCount;
        cur->segEmitted = 0;
    }
    *outCnt = cnt;
    return 0;
}

/*
 * 批量迁移一个进程：本地(任意本地节点) -> destNid(L2)，边扫描 numa_maps 边按 MAX_MOVE_PAGES_BATCH 分批迁移。
 * 固定分配 MAX_MOVE_PAGES_BATCH 的地址/节点/状态数组（不按 pageBudget 整体分配），迁够 pageBudget 即停。
 * 成功判定：move_pages(2) 成功时 status[i] = 页面最终所在 nid，须 == destNid 才算迁达；
 * 失败时 status[i] 为负错误码（-ENOENT/-EACCES/-EIO 等），逐页记录首个 errno。
 */
static int MigratePidFromToL2(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget)
{
    FILE *fp = OpenNumaMaps(pid);
    if (fp == NULL) {
        SMAP_LOGGER_ERROR("Open pid %d numa_maps failed.", pid);
        return -ENODEV;
    }

    /* 固定大小批数组，边扫描边迁移；不按 pageBudget 整体分配，避免极大值触发 TB 级 malloc。 */
    uint64_t *addrs = malloc(sizeof(uint64_t) * MAX_MOVE_PAGES_BATCH);
    int *nodes = malloc(sizeof(int) * MAX_MOVE_PAGES_BATCH);
    int *status = malloc(sizeof(int) * MAX_MOVE_PAGES_BATCH);
    if (addrs == NULL || nodes == NULL || status == NULL) {
        free(addrs);
        free(nodes);
        free(status);
        (void)pclose(fp);
        SMAP_LOGGER_ERROR("malloc batch arrays failed, pid %d.", pid);
        return -ENOMEM;
    }
    for (int i = 0; i < MAX_MOVE_PAGES_BATCH; i++) {
        nodes[i] = destNid; /* 全部目标 = L2 */
    }

    uint64_t movedCnt = 0;
    uint64_t failedCnt = 0;
    uint64_t attemptedCnt = 0; /* 实际收集到的候选页数，用于区分"无候选(非错误)"与"全失败(错误)" */
    int lastErr = 0; /* 保存首个失败 errno（全局 errno 或逐页 -status），避免被后续日志/free 覆写 */
    NumaScanCursor cur = {0};
    int ret = 0;
    while (*pageBudget > 0) {
        int cnt = 0;
        int r = CollectVaddrsBatch(fp, nrLocalNuma, pageSize, *pageBudget, &cur, addrs, &cnt);
        if (r != 0) {
            ret = r;
            break;
        }
        if (cnt == 0) {
            break; /* numa_maps 扫完，无更多候选 */
        }
        attemptedCnt += cnt;
        /* 复用 addrs 段作为页指针数组：LP64 下 uint64_t 与 void* 等宽，逐项即页虚拟地址。 */
        const void **pages = (const void **)addrs;
        SMAP_LOGGER_DEBUG("move_pages in: pid=%d batch=%d destNid=%d nrLocal=%d flags=0x%x", pid, cnt, destNid,
                          nrLocalNuma, (int)URGENT_MOVE_PAGES_FLAGS);
        long rc = SmapMovePages(pid, (unsigned long)cnt, pages, nodes, status, URGENT_MOVE_PAGES_FLAGS);
        if (rc < 0) {
            /* 全局失败（EACCES/EFAULT/EPERM/ENOMEM）：内核不填 status[]，整批计失败，不读未初始化内存。
               EACCES/EPERM 多为系统级权限问题，后续批同样会失败，终止本 pid。 */
            lastErr = errno;
            failedCnt += cnt;
            if (errno == EACCES || errno == EPERM) {
                ret = -lastErr;
                break;
            }
            continue;
        }
        /* 成功判定：status[i] == destNid 才代表迁达目标（非 0）。逐页失败保留首个 errno。 */
        uint64_t batchMoved = 0;
        for (int i = 0; i < cnt; i++) {
            int s = status[i];
            if (s == destNid) {
                batchMoved++;
            } else {
                failedCnt++;
                if (s < 0 && lastErr == 0) {
                    lastErr = -s; /* 逐页失败 errno（如 ENOENT/EACCES/EIO），保留首个 */
                }
            }
        }
        movedCnt += batchMoved;
        *pageBudget -= (batchMoved > *pageBudget) ? *pageBudget : batchMoved; /* 扣减预算 */
    }
    if (pclose(fp)) {
        SMAP_LOGGER_WARNING("Close numa_maps failed, pid=%d.", pid);
    }
    free(addrs);
    free(nodes);
    free(status);

    SMAP_LOGGER_INFO("pid %d local(nrLocal=%d)->L2=%d move_pages moved=%llu failed=%llu.", pid, nrLocalNuma, destNid,
                     (unsigned long long)movedCnt, (unsigned long long)failedCnt);

    if (ret != 0) {
        return ret;
    }
    if (attemptedCnt == 0) {
        return 0; /* 无候选页，非错误 */
    }
    if (movedCnt == 0) {
        /* 有候选但无一页迁达（逐页全失败或全局失败）：必须返回负错误，不能返回 0 误导调用方。 */
        if (lastErr == 0) {
            lastErr = EIO; /* 无可用 errno 时保守报 EIO（如远端节点故障逐页 -EIO） */
        }
        return -lastErr;
    }
    return 0;
}

typedef struct {
    pid_t pid;
    int l2;
} OomPidSnap;

/* 主入口：锁内只取快照，锁外做 numa_maps 读与 move_pages，避免 OOM 卡扫描线程。 */
void FindPidMigrateSize(uint64_t size)
{
    struct ProcessManager *manager = GetProcessManager();
    if (manager == NULL) {
        SMAP_LOGGER_ERROR("process manager is null.");
        return;
    }

    uint64_t pageSize = manager->tracking.pageSize;
    if (pageSize == 0) {
        SMAP_LOGGER_ERROR("pageSize is 0, smap not initialized?");
        return;
    }
    uint64_t pageBudget = size / pageSize;
    if (pageBudget == 0) {
        SMAP_LOGGER_INFO("size %llu too small (< pageSize %llu), skip.", (unsigned long long)size,
                         (unsigned long long)pageSize);
        return;
    }

    int nrLocalNuma = (int)manager->nrLocalNuma; /* 初始化后不变，锁外读取安全 */

    /* 持锁只取快照，numa_maps 读与 move_pages 在锁外执行，避免 OOM 卡扫描线程。 */
    OomPidSnap snap[MAX_4K_PROCESSES_CNT];
    int n = 0;
    EnvMutexLock(&manager->lock);
    for (ProcessAttr *cur = manager->processes; cur != NULL && n < MAX_4K_PROCESSES_CNT; cur = cur->next) {
        int l2 = GetAttrL2(cur);
        if (l2 == NUMA_NO_NODE) {
            continue; /* 未配置远端目标，跳过 */
        }
        if (cur->state == PROC_MIGRATE || cur->state == PROC_MOVE || GetL2ActcLen(cur) > 0) {
            continue; /* 协调扫描线程/逃生态/远端(L2)已有页，避免重复介入 */
        }
        snap[n].pid = cur->pid;
        snap[n].l2 = l2;
        n++;
    }
    EnvMutexUnlock(&manager->lock);

    for (int i = 0; i < n && pageBudget > 0; i++) {
        int ret = MigratePidFromToL2(snap[i].pid, nrLocalNuma, snap[i].l2, pageSize, &pageBudget);
        if (ret != 0) {
            if (ret == -EACCES || ret == -EPERM) {
                /* 系统级权限/能力故障：后续 pid 同样会失败，终止本轮避免无谓 churn。 */
                SMAP_LOGGER_ERROR("systemic migrate failure (ret=%d) at pid %d, abort urgent pass.", ret, snap[i].pid);
                break;
            }
            SMAP_LOGGER_ERROR("migrate pid %d failed: %d, try next.", snap[i].pid, ret);
        }
    }
    SMAP_LOGGER_INFO("urgent migrate done, remaining budget %llu pages.", (unsigned long long)pageBudget);
}
