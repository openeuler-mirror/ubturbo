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
#include "access_iomem.h"
#include "access_acpi_mem.h"
#include "access_tracking.h"
#include "bus.h"
#include "ub_hist.h"
#include "hist_ops.h"
#include "hist_tracking.h"

#define to_access_tracking_dev(n) \
	container_of(n, struct access_tracking_dev, ldev)
#define to_delay_work(n) container_of(n, struct delayed_work, work)
#define delay_work_to_dev(n) \
	container_of(n, struct access_tracking_dev, scan_work)
#define WORK_QUEUE_NAME_LEN 32
#define HIST_TRACKING_DEFAULT_PERIOD 200
#define SCAN_TIME_MAX 100000

#undef pr_fmt
#define pr_fmt(fmt) "hist: " fmt

extern struct list_head access_dev;

static int hist_tracking_disable(struct device *ldev)
{
	struct access_tracking_dev *hdev;

	hdev = to_access_tracking_dev(ldev);
	hdev->enable_on = false;
	hist_thread_pause();
	return 0;
}

static void hist_dev_pgsize_update(u8 page_size_mode)
{
	u32 pgsize = page_size_mode == PAGE_MODE_2M ? SIZE_2M : SIZE_4K;
	hist_update_pgsize(pgsize);
}

/*
 * 频次已迁移至 page->flags，无需再维护 ACTC 缓冲，
 * 但 page_count 仍被 acidx 越界检查（fill_actc_data_by_bitmap 等）
 * 依赖，必须在使能/页粒度变更时按最新 remote_ram_list 重算。
 */
static u64 hist_calc_access_len(struct access_tracking_dev *hdev)
{
	int page_size = get_page_size(hdev);
	u64 page_count;

	if (hdev->node >= nr_local_numa)
		page_count = get_node_page_cnt_iomem(hdev->node, page_size);
	else
		page_count = get_node_actc_len(hdev->node, page_size);
	pr_debug("histogram tracking node: %d, got page count: %llu\n",
		 hdev->node, page_count);

	return page_count;
}

static void hist_pginfo_reinit(struct access_tracking_dev *hdev)
{
	u64 page_count = hist_calc_access_len(hdev);

	if (!page_count)
		pr_debug("no page found on node: %d\n", hdev->node);
	if (hdev->page_count != page_count)
		pr_debug("page amount of tracking device on node %d has been changed from %llu to %llu\n",
			 hdev->node, hdev->page_count, page_count);
	hdev->page_count = page_count;
}

static void hist_tracking_enable(struct device *ldev)
{
	struct access_tracking_dev *hdev;

	hdev = to_access_tracking_dev(ldev);
	down_write(&hdev->buffer_lock);
	hist_pginfo_reinit(hdev);
	up_write(&hdev->buffer_lock);
	hdev->enable_on = true;
	hist_thread_resume();
}

static int hist_tracking_set_page_size(struct device *ldev, u8 pgsize)
{
	struct access_tracking_dev *hdev;

	hdev = to_access_tracking_dev(ldev);

	if (pgsize != PAGE_MODE_4K && pgsize != PAGE_MODE_2M) {
		pr_err("invalid page size\n");
		return -EINVAL;
	}
	down_write(&hdev->buffer_lock);
	hdev->enable_on = false;
	hdev->page_size_mode = pgsize;
	hist_dev_pgsize_update(pgsize);
	hist_pginfo_reinit(hdev);
	up_write(&hdev->buffer_lock);
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

static int hist_tracking_ub_watch(struct device *ldev, void *result)
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

static int hist_tracking_ub_watch_config(struct device *ldev, u32 duration_ms)
{
	return ub_watch_config(duration_ms);
}

static struct tracking_operations g_hist_tracking_ops = {
	.tracking_enable = hist_tracking_enable,
	.tracking_disable = hist_tracking_disable,
	.tracking_set_page_size = hist_tracking_set_page_size,
	.tracking_ub_watch = hist_tracking_ub_watch,
	.tracking_ub_watch_config = hist_tracking_ub_watch_config,
};

static void hist_tracking_deinit(void)
{
	struct access_tracking_dev *hdev, *n;
	list_for_each_entry_safe(hdev, n, &access_dev, list) {
		if (!hdev->is_hist)
			continue;
		tracking_dev_remove(hdev->tracking_dev);
		device_unregister(&hdev->ldev);
		kfree(hdev);
	}
}

void access_tracking_dev_release(struct device *dev)
{
	pr_debug("Releasing device %s\n", dev_name(dev));
}

static int hist_tracking_init(void)
{
	int ret;
	unsigned int node;
	struct access_tracking_dev *hdev;

	if (nr_local_numa < 0)
		return -EINVAL;

	for (node = (unsigned int)nr_local_numa; node < SMAP_MAX_NUMNODES;
	     ++node) {
		hdev = kzalloc(sizeof(struct access_tracking_dev), GFP_KERNEL);
		if (!hdev) {
			pr_err("unable to alloc mem for histogram tracking device\n");
			goto put_dev;
		}

		hdev->node = node;
		hdev->page_size_mode = PAGE_MODE_2M;
		hdev->is_hist = true;

		hdev->page_count = hist_calc_access_len(hdev);
		pr_info("page count: %llu for node: %d\n", hdev->page_count,
			hdev->node);

		init_rwsem(&hdev->buffer_lock);
		device_initialize(&hdev->ldev);
		hdev->ldev.release = access_tracking_dev_release;
		ret = dev_set_name(&hdev->ldev, "hist_tracking_dev%d", node);
		if (ret) {
			pr_err("unable to set histogram tracking device name, ret: %d\n",
			       ret);
			goto put_dev_hdev;
		}

		ret = device_add(&hdev->ldev);
		if (ret) {
			pr_err("unable to add histogram tracking device\n");
			goto put_dev_hdev;
		}

		hdev->tracking_dev = tracking_dev_add(
			&hdev->ldev, &g_hist_tracking_ops, hdev->node);
		if (!hdev->tracking_dev) {
			pr_err("unable to add tracking for node: %d\n",
			       hdev->node);
			goto del_dev;
		}

		list_add_tail(&hdev->list, &access_dev);
	}

	return 0;

del_dev:
	device_del(&hdev->ldev);
put_dev_hdev:
	put_device(&hdev->ldev);
	kfree(hdev);
put_dev:
	hist_tracking_deinit();
	pr_err("smap tracking dev init failed.\n");
	return -ENODEV;
}

int hist_module_init(void)
{
	int ret;
	ret = init_acpi_mem();
	if (ret) {
		pr_err("parse ACPI table failed: %d\n", ret);
		return ret;
	}
	ret = hist_init(SIZE_2M);
	if (ret) {
		pr_err("init SMAP histogram device failed, ret: %d\n", ret);
		goto err_acpi_mem;
	}

	ret = hist_tracking_init();
	if (ret) {
		pr_err("init histogram tracking device failed, ret: %d\n", ret);
		goto err_tracking_add;
	}
	pr_info("smap hist tracking init success.\n");
	return 0;

err_tracking_add:
	hist_deinit();
err_acpi_mem:
	reset_acpi_mem();
	return ret;
}
