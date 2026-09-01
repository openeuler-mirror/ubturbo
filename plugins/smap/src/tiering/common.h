/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP common header
 */

#ifndef _SRC_TIERING_COMMON_H
#define _SRC_TIERING_COMMON_H

#include <linux/types.h>
#include <asm/page.h>
#include <linux/list.h>

#ifndef MAX
#define MAX(a, b) ((a) >= (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif

#define HUNDRED 100
#define HALF_HUNDRED (HUNDRED / 2)
#define TWO_MEGA_SHIFT 21
#define TWO_MEGA_SIZE (1UL << TWO_MEGA_SHIFT)
#define TWO_MEGA_MASK (~(TWO_MEGA_SIZE - 1))
#define MAX_4K_PROCESSES_CNT 300
#define MAX_2M_PROCESSES_CNT 100
#define MAX_PER_PID_MIG_LIST_COUNT 8
#define MAX_2M_MIGMSG_CNT (MAX_2M_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT)
#define MAX_4K_MIGMSG_CNT (MAX_4K_PROCESSES_CNT * MAX_PER_PID_MIG_LIST_COUNT)
#define NR_BATCHED_MIGRATION 512

#define SMAP_MAX_LOCAL_NUMNODES 4
#define SMAP_MAX_NUMNODES 22

#define SIZE_KB (1 << 10)

/* 分批同步迁移的批次间隔时间（单位us）及区间抖动 */
#define MIGRATE_PAGES_BATCH_SLEEP_US_MIN 100
#define MIGRATE_PAGES_BATCH_SLEEP_US_MAX 1000000

extern u32 g_pagesize_huge;

enum node_level {
	L1,
	L2,
	NR_LEVEL,
};

struct mig_list {
	bool success_to_user;
	u64 nr;
	u64 failed_mig_nr;
	u64 failed_pre_migrated_nr;
	pid_t pid;
	int from;
	int to;
	u64 *addr;
};

struct mig_pra {
	int page_size;
	int nr_thread;
	bool is_mul_thread;
};

struct migrate_msg {
	int cnt;
	struct mig_pra mul_mig;
	struct mig_list *mig_list;
};

#define MAX_NR_MIGNUMA 50

struct pa_range {
	u64 pa_start;
	u64 pa_end;
};

struct migrate_numa_inner_msg {
	int src_nid;
	int dest_nid;
	int count;
	struct pa_range range[MAX_NR_MIGNUMA];
};

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

typedef enum {
	NORMAL_MIGRATE,
	REMOTE_MIGRATE,
	SYNC_MIGRATE,
	MAX_MIGRATE_TYPE,
} migrate_type;

typedef enum {
	SYNC_MIGRATE_MODE_TOTAL, /* 一次性迁移全部 */
	SYNC_MIGRATE_MODE_BATCH, /* 分批迁移 */
} sync_migrate_mode;

struct migrate_pages_msg {
	sync_migrate_mode mode; /* SYNC_MIGRATE_MODE_TOTAL / SYNC_MIGRATE_MODE_BATCH */
	int src_nid;            /* 源NUMA节点 */
	int dest_nid;           /* 目的NUMA节点 */
	pid_t pid;              /* 目标进程 */
	uint64_t total_size;    /* 迁移总大小(KB) */
	uint64_t batch_size;    /* 每次批量大小(KB) */
	uint64_t sleep_time_us; /* 批次间休眠时间(us) */
};

typedef enum {
	SMAP_MIGRATE_DISABLE = 0,
	SMAP_MIGRATE_ENABLE,
} smap_migrate_status;

struct migrate_status_set_msg {
	smap_migrate_status status;
	pid_t pid;
};

static inline u64 calc_huge_count(u64 range)
{
	return (range & ~TWO_MEGA_MASK) == 0
		       ? (u64)(range >> TWO_MEGA_SHIFT)
		       : (u64)((range >> TWO_MEGA_SHIFT) + 1);
}

static inline u64 calc_normal_count(u64 range)
{
	return (range & ~PAGE_MASK) == 0 ? (u64)(range >> PAGE_SHIFT)
					 : (u64)((range >> PAGE_SHIFT) + 1);
}

static inline bool is_node_invalid(int node)
{
	return node < 0 || node >= SMAP_MAX_NUMNODES;
}

#endif /* _SRC_TIERING_COMMON_H */
