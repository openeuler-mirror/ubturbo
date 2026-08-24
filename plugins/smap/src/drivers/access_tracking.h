/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP Tiering Memory Solution: tracking_access模块
 */

#ifndef _SRC_ACCESS_TRACKING_H
#define _SRC_ACCESS_TRACKING_H

#include <linux/device.h>
#include <linux/types.h>
#include <linux/mutex.h>

#include "accessed_bit.h"
#include "access_pid.h"
#include "drv_common.h"
#include "bus.h"
#include "hist_tracking.h"

extern u32 g_pagesize_huge;
extern unsigned int enable_hist;

enum access_page_mode {
	PAGE_MODE_4K = 0,
	PAGE_MODE_2M = 9, // 考虑上层提供的其他常见页面粒度，这里对此做预留
};

enum hist_status {
	DISABLE_HIST,
	ENABLE_HIST,
	NR_STATUS_ARGS,
};

#define AB_ACTC_ELEM_SIZE 16
#define WORKQ_NAME_SIZE 32
#define WORKQ_NAME_MAX_LEN (WORKQ_NAME_SIZE - 1)
#define WQ_MAX_THREADS 0

extern struct list_head access_dev;
extern u8 access_page_size;

static inline bool is_access_hugepage(void)
{
	return access_page_size == PAGE_MODE_2M;
}

static inline struct access_tracking_dev *get_access_tracking_dev(int node_id)
{
	struct access_tracking_dev *adev;
	list_for_each_entry(adev, &access_dev, list) {
		if (adev->node == node_id) {
			return adev;
		}
	}
	return NULL;
}

static inline struct access_tracking_dev *get_first_access_dev(void)
{
	return list_first_entry(&access_dev, struct access_tracking_dev, list);
}

/*
 * 扫描模块是否处于 enable 状态。add_pid 在非 enable（disable/migrate 期间）时
 * 不应立即提交扫描任务，pid 留在 ap_data.list 中等下次 enable 由 submit_scan_works 拉起。
 * access_dev 为空（如单测环境）按 disable 处理，避免对空表 list_first_entry 越界读 enable_on。
 */
static inline bool access_scan_enabled(void)
{
	if (list_empty(&access_dev))
		return false;
	return get_first_access_dev()->enable_on;
}

static inline int get_page_size(struct access_tracking_dev *adev)
{
	return adev->page_size_mode == PAGE_MODE_2M ? g_pagesize_huge
						    : PAGE_SIZE;
}

void cancel_ap_scan_work(struct access_pid *ap);
int set_scan_cpus(u32 cpu_start, u32 cpu_end);
bool is_access_hugepage(void);
void submit_one_work(struct access_pid *ap);
ktime_t calc_time_us(ktime_t start_time);
#endif /* _SRC_ACCESS_TRACKING_H */
