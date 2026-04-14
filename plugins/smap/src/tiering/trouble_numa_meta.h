/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef _TROUBLR_NUMA_META_H
#define _TROUBLR_NUMA_META_H

#define MAX_NUMA_NUM 18

enum numa_list_flags {
    NUMA_UNAVAILABLE = 0,
    NUMA_AVAILABLE
};

struct numa_entry {
    u16 numa_id;
    u16 status;
};

struct numa_status_list {
    u16 cnt;
    struct numa_entry entries[MAX_NUMA_NUM];
};

void init_trouble_numa_manager(void);

void cleanup_trouble_numa_manager(void);

int trouble_numa_list_add(u16 numa_id);

int trouble_numa_list_get_all(u16 *buffer, size_t buf_size);

int is_trouble_numa(u16 numa_id);

int deal_trouble_numa_info(struct numa_status_list *numa_info);

#endif