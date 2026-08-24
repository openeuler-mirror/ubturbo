/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP NUMA module
 */

#ifndef _SRC_TIERING_NUMA_H
#define _SRC_TIERING_NUMA_H

#ifdef __cplusplus
extern "C" {
#endif
unsigned long get_node_nr_free_pages(int nid);

#ifdef __cplusplus
}
#endif
#endif /* _SRC_TIERING_NUMA_H */