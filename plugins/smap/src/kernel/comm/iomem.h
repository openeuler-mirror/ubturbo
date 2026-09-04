/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP 远端内存地址段管理模块
 */

#ifndef SMAP_IOMEM_H
#define SMAP_IOMEM_H

#include <linux/spinlock.h>

#include "kernel_common.h"

struct ram_segment {
	struct list_head node;
	int numa_node;
	u64 start;
	u64 end;
};

struct pa_range {
	u64 pa_start;
	u64 pa_end;
};

extern struct list_head remote_ram_list;
extern int nr_local_numa;
extern rwlock_t rem_ram_list_lock;
void release_remote_ram(void);
int refresh_remote_ram(void);
int get_numa_by_pfn(unsigned long pfn);
u64 get_node_page_cnt_iomem(int nid, int page_size);
int calc_paddr_acidx_iomem(u64 pa, int *nid, u64 *index, int page_size);
int calc_paddr_acidx_iomem_known_nid(u64 pa, int nid, u64 *index,
				     int page_size);
int calc_acidx_paddr_iomem(int nid, u64 acidx, u64 *paddr, int page_size);

#endif /* SMAP_IOMEM_H */
