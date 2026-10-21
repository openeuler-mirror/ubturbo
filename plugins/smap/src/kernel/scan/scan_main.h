/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP Tiering Memory Solution: scan_main模块
 */

#ifndef _SRC_SCAN_MAIN_H
#define _SRC_SCAN_MAIN_H

#include <linux/types.h>
#include <linux/mutex.h>

#include "accessed_bit.h"
#include "access_pid.h"
#include "drv_common.h"

extern u32 g_pagesize_huge;

enum access_page_mode {
	PAGE_MODE_4K = 0,
	PAGE_MODE_2M = 9, // 考虑上层提供的其他常见页面粒度，这里对此做预留
};

/* 远端节点扫描模式，由用户态配置文件经 ioctl 下发，缺省为软硬结合 */
enum remote_scan_mode {
	SMAP_REMOTE_SCAN_SOFT = 0, /* 纯软：远端仅软件扫描 */
	SMAP_REMOTE_SCAN_COMBINED, /* 软硬结合：远端软硬件频次叠加 */
	SMAP_REMOTE_SCAN_HARD, /* 纯硬：远端频次仅由硬件提供 */
	NR_REMOTE_SCAN_MODE,
};

#define AB_ACTC_ELEM_SIZE 16
#define WORKQ_NAME_SIZE 32
#define WORKQ_NAME_MAX_LEN (WORKQ_NAME_SIZE - 1)
#define WQ_MAX_THREADS 8

void cancel_ap_scan_work(struct access_pid *ap);
int scan_init(void);
void scan_exit(void);
int access_tracking_restart_pid(pid_t pid);
int access_tracking_set_page_size(u8 page_size_index);
int set_remote_scan_mode(u8 mode);
bool is_remote_scan_hist_active(void);
bool is_freq_hist_only_node(int node);
int set_scan_cpus(u32 cpu_start, u32 cpu_end);
void submit_one_work(struct access_pid *ap);
ktime_t calc_time_us(ktime_t start_time);
u8 get_scan_page_mode(void);
int get_scan_page_size(void);
bool is_access_hugepage(void);
u64 get_node_page_count(int node);
void set_node_page_count(int node, u64 page_count);
void copy_node_page_count(u64 *page_count);
#endif /* _SRC_SCAN_MAIN_H */
