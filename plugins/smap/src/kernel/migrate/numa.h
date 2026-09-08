/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP NUMA module
 */

#ifndef _SRC_TIERING_NUMA_H
#define _SRC_TIERING_NUMA_H

#include "kernel_common.h"

unsigned long get_node_nr_free_pages(int nid);

static inline bool is_node_invalid(int node)
{
	return node < 0 || node >= SMAP_MAX_NUMNODES;
}

#endif /* _SRC_TIERING_NUMA_H */
