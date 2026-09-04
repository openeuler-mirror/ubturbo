// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP : hist_dev
 */
#include <asm/types.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>
#include <linux/mmzone.h>
#include <linux/pfn.h>
#include <linux/workqueue.h>
#include <linux/hugetlb.h>
#include <linux/ktime.h>
#include <linux/version.h>
#include <linux/spinlock.h>

#include "check.h"
#include "iomem.h"
#include "acpi_mem.h"
#include "scan_main.h"
#include "ub_hist.h"
#include "hist_ops.h"
#include "hist_tracking.h"

#undef pr_fmt
#define pr_fmt(fmt) "hist: " fmt

bool is_hist_tracking_node(int node)
{
	return enable_hist && node >= nr_local_numa && node < SMAP_MAX_NUMNODES;
}

int hist_tracking_disable(void)
{
	hist_thread_pause();
	return 0;
}

static void hist_dev_pgsize_update(u8 page_size_mode)
{
	u32 pgsize = page_size_mode == PAGE_MODE_2M ? SIZE_2M : SIZE_4K;
	hist_update_pgsize(pgsize);
}

void hist_tracking_enable(void)
{
	hist_thread_resume();
}

int hist_tracking_set_page_size(u8 pgsize)
{
	if (pgsize != PAGE_MODE_4K && pgsize != PAGE_MODE_2M) {
		pr_err("invalid page size\n");
		return -EINVAL;
	}
	hist_dev_pgsize_update(pgsize);
	return 0;
}

static inline bool is_numa_flux_updated(struct ub_flux_mb_statistic *stc,
					int numa_id)
{
	int i;

	for (i = 0; i < stc->len; i++) {
		if (stc->flux[i].numa_id == numa_id)
			return true;
	}
	return false;
}

int hist_tracking_ub_watch(void *result)
{
	struct ub_flux_mb flux_mb;
	struct ub_flux_mb_statistic *stc =
		(struct ub_flux_mb_statistic *)result;
	struct ram_segment *seg, *tmp;
	int idx = 0, ret;

	if (!stc)
		return -EINVAL;

	ret = ub_watch(&flux_mb);
	if (ret)
		return ret;

	stc->len = 0;
	read_lock(&rem_ram_list_lock);
	list_for_each_entry_safe(seg, tmp, &remote_ram_list, node) {
		if (stc->len >= SMAP_MAX_REMOTE_NUMNODES) {
			pr_err("too many remote NUMA nodes\n");
			read_unlock(&rem_ram_list_lock);
			return -EINVAL;
		}

		if (is_numa_flux_updated(stc, seg->numa_node))
			continue;

		ret = get_path_idx_by_addr(seg->start, &idx);
		if (ret) {
			pr_err("get path index failed for seg %#llx\n",
			       seg->start);
			read_unlock(&rem_ram_list_lock);
			return ret;
		}

		stc->flux[stc->len].numa_id = seg->numa_node;
		stc->flux[stc->len].read_mb = flux_mb.read[idx];
		stc->flux[stc->len].write_mb = flux_mb.write[idx];
		stc->len++;
	}

	read_unlock(&rem_ram_list_lock);

	return 0;
}

int hist_tracking_ub_watch_config(u32 duration_ms)
{
	return ub_watch_config(duration_ms);
}

int hist_module_init(void)
{
	int ret;

	ret = hist_init(SIZE_2M);
	if (ret) {
		pr_err("init SMAP histogram device failed, ret: %d\n", ret);
		return ret;
	}
	pr_info("smap hist tracking init success.\n");
	return 0;
}
