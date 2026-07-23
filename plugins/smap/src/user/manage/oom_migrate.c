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
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "smap_user_log.h"
#include "manage.h"
#include "securec.h"
#include "oom_migrate.h"

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

/* move_pages(2) 薄封装，UT 拦截此处不触达真实 syscall。 */
long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status, int flags)
{
    return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}

/*
 * 读 /proc/<pid>/numa_maps，段级本地过滤：取含本地节点页的段并枚举候选 vaddr。
 * numa_maps 只给段内各 node 页数、无页偏移，枚举地址可能含段内非本地页，
 * 由 move_pages 按页粒度处理（已在 L2 的页幂等，未映射页返回 -ENOENT）。
 */
int CollectVaddrsFromNumaMaps(pid_t pid, int nrLocalNuma, uint64_t pageSize, uint64_t maxPages, uint64_t **outAddrs,
                              int *outCnt)
{
    *outAddrs = NULL;
    *outCnt = 0;
    if (nrLocalNuma <= 0) {
        return 0; /* 无本地节点，无可迁本地页 */
    }
    if (maxPages == 0 || maxPages > SIZE_MAX / sizeof(uint64_t)) {
        SMAP_LOGGER_ERROR("invalid maxPages %llu.", (unsigned long long)maxPages);
        return -EINVAL; /* 申请大小非法，避免 malloc(0) 或整数溢出 */
    }

    FILE *fp = OpenNumaMaps(pid);
    if (fp == NULL) {
        SMAP_LOGGER_ERROR("Open pid %d numa_maps failed.", pid);
        return -ENODEV;
    }

    uint64_t *addrs = malloc(sizeof(uint64_t) * maxPages);
    if (addrs == NULL) {
        (void)pclose(fp);
        SMAP_LOGGER_ERROR("malloc vaddrs failed, pid %d.", pid);
        return -ENOMEM;
    }

    int cnt = 0;
    char line[MAX_LINE_LENGTH];
    bool huge = IsHugeMode();
    while (fgets(line, sizeof(line), fp) != NULL && (uint64_t)cnt < maxPages) {
        if (huge && !IsNumaMapLineHuge(line)) {
            continue; /* 大页模式只取 huge 段 */
        }
        unsigned long segStart = 0;
        if (sscanf_s(line, "%lx", &segStart) != 1) {
            continue; /* 解析段起始地址失败 */
        }
        /* 段内落在本地节点的总页数 = Σ N<i>=<count>，i ∈ [0, nrLocalNuma) */
        unsigned long localCount = 0;
        for (int i = 0; i < nrLocalNuma; i++) {
            char pattern[NUMA_MAPS_MAX_PATTERN_LEN];
            if (snprintf_s(pattern, sizeof(pattern), sizeof(pattern) - 1, " N%d=", i) < 0) {
                continue;
            }
            char *sub = strstr(line, pattern);
            if (sub == NULL) {
                continue;
            }
            char *value = sub + strlen(pattern);
            char *end = NULL;
            errno = 0;
            unsigned long c = strtoull(value, &end, 10);
            if (value == end || errno != 0) {
                continue; /* 解析 N<i>=<count> 失败，跳过该 node 字段 */
            }
            localCount += c;
        }
        if (localCount == 0) {
            continue; /* 该段无本地页，整段跳过（不误碰纯远端段） */
        }
        for (unsigned long i = 0; i < localCount && (uint64_t)cnt < maxPages; i++) {
            addrs[cnt] = (uint64_t)(segStart + i * pageSize);
            cnt++;
        }
    }
    if (pclose(fp)) {
        SMAP_LOGGER_WARNING("Close numa_maps failed, pid=%d.", pid);
    }
    if (cnt == 0) {
        free(addrs);
        addrs = NULL;
    }
    *outAddrs = addrs;
    *outCnt = cnt;
    return 0;
}

/*
 * 批量迁移一个进程：本地(任意本地节点) -> destNid(L2)，按 MAX_MOVE_PAGES_BATCH 分批。
 * nodes[] 全填 L2，迁够 pageBudget 即停。
 */
int MigratePidFromToL2(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget)
{
    if (pageBudget == NULL || *pageBudget == 0) {
        return 0;
    }

    uint64_t *addrs = NULL;
    int addrCnt = 0;
    int ret = CollectVaddrsFromNumaMaps(pid, nrLocalNuma, pageSize, *pageBudget, &addrs, &addrCnt);
    if (ret != 0) {
        SMAP_LOGGER_ERROR("Collect vaddrs for pid %d failed: %d.", pid, ret);
        return ret;
    }
    if (addrCnt == 0) {
        free(addrs);
        return 0;
    }

    int *nodes = malloc(sizeof(int) * addrCnt);
    int *status = malloc(sizeof(int) * addrCnt);
    if (nodes == NULL || status == NULL) {
        free(addrs);
        free(nodes);
        free(status);
        SMAP_LOGGER_ERROR("malloc nodes/status failed, pid %d.", pid);
        return -ENOMEM;
    }
    for (int i = 0; i < addrCnt; i++) {
        nodes[i] = destNid; /* 全部目标 = L2 */
    }

    uint64_t ok = 0;
    uint64_t fail = 0;
    int lastErr = 0; /* 保存 move_pages 全局失败 errno，避免被后续日志/free 覆写 */
    for (int base = 0; base < addrCnt; base += MAX_MOVE_PAGES_BATCH) {
        int batch = addrCnt - base;
        if (batch > MAX_MOVE_PAGES_BATCH) {
            batch = MAX_MOVE_PAGES_BATCH;
        }
        /* 复用 addrs 段作为页指针数组：LP64 下 uint64_t 与 void* 等宽，逐项即页虚拟地址。 */
        const void **pages = (const void **)&addrs[base];
        SMAP_LOGGER_DEBUG(
            "move_pages in: pid=%d batch=%d addrCnt=%d destNid=%d nrLocal=%d first=0x%llx last=0x%llx flags=0x%x", pid,
            batch, addrCnt, destNid, nrLocalNuma, (unsigned long long)addrs[base],
            (unsigned long long)addrs[base + batch - 1], (int)URGENT_MOVE_PAGES_FLAGS);
        long rc = SmapMovePages(pid, (unsigned long)batch, pages, &nodes[base], &status[base], URGENT_MOVE_PAGES_FLAGS);
        if (rc < 0) {
            /* 全局失败（EACCES/EFAULT/EPERM/ENOMEM）：内核不填 status[]，整批计失败，不读未初始化内存 */
            lastErr = errno;
            fail += batch;
            continue;
        }
        for (int i = 0; i < batch; i++) {
            if (status[base + i] == 0) {
                ok++;
            } else {
                fail++;
            }
        }
    }
    SMAP_LOGGER_INFO("pid %d local(nrLocal=%d)->L2=%d move_pages moved=%llu failed=%llu.", pid, nrLocalNuma, destNid,
                     (unsigned long long)ok, (unsigned long long)fail);

    *pageBudget -= (ok > *pageBudget) ? *pageBudget : ok; /* 扣减预算 */
    free(addrs);
    free(nodes);
    free(status);
    return (ok == 0) ? -lastErr : 0;
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
            SMAP_LOGGER_ERROR("migrate pid %d failed: %d, try next.", snap[i].pid, ret);
        }
    }
    SMAP_LOGGER_INFO("urgent migrate done, remaining budget %llu pages.", (unsigned long long)pageBudget);
}
