/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef _TROUBLR_NUMA_META_H
#define _TROUBLR_NUMA_META_H

enum numa_list_flags {
    NUMA_AVAILABLE = 0,
    NUMA_UNAVAILABLE
};

struct numa_entry {
    u16 numa_id;
    u16 status;
};

struct numa_status_list {
    u16 cnt;
    struct numa_entry entries[];
};

void init_trouble_numa_manager(void);

void cleanup_trouble_numa_manager(void);

int trouble_numa_list_add(u16 numa_id);

int trouble_numa_list_del(u16 numa_id);

void trouble_numa_list_del_all(void);

int is_trouble_numa_in_list(u16 numa_id);

int trouble_numa_list_get_all(u16 *buffer, size_t buf_size);

int is_trouble_numa_list_empty(void);

void deal_trouble_numa_info(void *msg);

#endif