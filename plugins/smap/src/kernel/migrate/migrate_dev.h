/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP: SMAP MIGRATE_DEV
 */

#ifndef _MIGRATE_DEV_H
#define _MIGRATE_DEV_H

#include <linux/types.h>

#include "common.h"
#include "dump_info.h"
#include "kernel_common.h"
#include "smap_migrate_pages.h"
#include "smap_migrate_wrapper.h"

#define KB_TO_2M 11
#define KB_TO_4K 2
#define SMAP_MIG_DEV "smap_migrate_dev"
#define SMAP_MIG_CLASS "smap_migrate_class"
#define HUNDRED 100
#define HALF_HUNDRED (HUNDRED / 2)
#define MAX_2M_MIGMSG_CNT \
	(MAX_2M_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT)
#define MAX_4K_MIGMSG_CNT \
	(MAX_4K_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT)

struct migrate_numa_msg {
	int src_nid;
	int dest_nid;
	int count;
	u64 memids[MAX_NR_MIGNUMA];
};

struct mig_payload {
	pid_t pid;
	int src_nid;
	int dest_nid;
	int ratio;
	int keep_ratio;
	u64 mem_size;
	bool is_ratio_mode;
	u64 success_cnt;
};

struct migrate_pid_remote_numa_msg {
	int pid_cnt;
	struct mig_payload *payloads;
	int *mig_res_array; // 迁移结果
};

#define SMAP_MIG_MIGRATE _IOW(SMAP_MIGRATE_MAGIC, 0, struct migrate_msg)
#define SMAP_SET_PAGETYPE _IOW(SMAP_MIGRATE_MAGIC, 1, uint32_t)
#define SMAP_MIG_MIGRATE_NUMA \
	_IOW(SMAP_MIGRATE_MAGIC, 2, struct migrate_numa_msg)
#define SMAP_MIG_PID_REMOTE_NUMA \
	_IOW(SMAP_MIGRATE_MAGIC, 3, struct migrate_pid_remote_numa_msg)
#define SMAP_SET_UB_DMA_AVAIL _IOW(SMAP_MIGRATE_MAGIC, 4, unsigned int)

static inline int get_max_pid_cnt(void)
{
	return smap_pgtype == HUGE_PAGE ? MAX_2M_PROCESSES_CNT
					: MAX_4K_PROCESSES_CNT;
}

extern void walk_pid_pagemap(struct pagemapread *pm);
extern int convert_pos_to_paddr_sorted(pid_t pid, int nid, u64 len, u64 *addr);
extern int smap_is_remote_addr_valid(int nid, u64 pa_start, u64 pa_end);

extern u64 get_node_page_cnt_iomem(int nid, int page_size);
int migrate_dev_init(void);
void migrate_dev_exit(void);

#endif /* _MIGRATE_DEV_H */
