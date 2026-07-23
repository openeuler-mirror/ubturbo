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
#ifndef __OOM_MIGRATE_H__
#define __OOM_MIGRATE_H__

#include <stdint.h>
#include <sys/types.h>

#define MAX_MOVE_PAGES_BATCH 512 /* 单次 move_pages(2) 批量上限 */

/* 紧急迁出主入口：把 size 字节、落在受管进程 L1 的内存迁到其 L2。 */
void FindPidMigrateSize(uint64_t size);

/* move_pages(2) 薄封装（mockable seam）：UT 拦截此处，不触达真实 syscall。 */
long SmapMovePages(int pid, unsigned long count, const void **pages, const int *nodes, int *status, int flags);

/*
 * 读 /proc/<pid>/numa_maps，段级本地过滤（本地 nid ∈ [0, nrLocalNuma)），收集含本地节点页的段
 * 的候选 vaddr，支持多本地节点进程。
 */
int CollectVaddrsFromNumaMaps(pid_t pid, int nrLocalNuma, uint64_t pageSize, uint64_t maxPages, uint64_t **outAddrs,
                              int *outCnt);

/* 批量迁移一个进程：本地(任意本地节点) -> destNid(L2)，迁够 pageBudget 即停。 */
int MigratePidFromToL2(pid_t pid, int nrLocalNuma, int destNid, uint64_t pageSize, uint64_t *pageBudget);

#endif /* __OOM_MIGRATE_H__ */
