/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP monitor for OBMM device discovery and address range query
 */

#ifndef _SRC_MIGRATE_OBMM_MONITOR_H
#define _SRC_MIGRATE_OBMM_MONITOR_H

#include <linux/list.h>

#include "../comm/iomem.h"
#include "kernel_common.h"

#define MAX_MEMID_STRLEN 32

extern struct list_head remote_ram_list;
extern int nr_local_numa;

struct memid_range {
	u64 memid;
	u64 start;
	u64 end;
	u64 seq;
	struct list_head node;
};

int iterate_obmm_dev(void);
void free_obmm_dev(void);
int find_range_by_memid(u64 memid, u64 *start, u64 *end);

static inline bool is_numa_remote(int nid)
{
	return nid >= nr_local_numa && nid < SMAP_MAX_NUMNODES;
}

#endif /* _SRC_MIGRATE_OBMM_MONITOR_H */
