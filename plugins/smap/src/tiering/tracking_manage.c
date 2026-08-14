// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP Tiering Memory Solution: SMAP TRACKING_MANAGE
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/nodemask.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>

#include "migrate_task.h"
#include "smap_migrate_wrapper.h"
#include "dump_info.h"
#include "iomem.h"
#ifdef HAM_ENABLED
#include "ham_migration.h"
#endif
#include "pid_ioctl.h"
#ifdef CRITICAL_OFF
#include "critical.h"
#endif
#include "tracking_manage.h"

#define SMAP_WATCH_NAME "smap_migrate_result"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_track_manage: " fmt

unsigned int smap_pgtype = HUGE_PAGE;
EXPORT_SYMBOL_GPL(smap_pgtype);
#define MB_INTV 1000
#define SUBTASK_RETRY_TIME 1000
#define SUBTASK_SLEEP_TIME 5

struct workqueue_struct *migrate_back_wq = NULL;
struct delayed_work migrate_back_work;

bool is_smap_pg_huge(void)
{
	return smap_pgtype == HUGE_PAGE;
}
EXPORT_SYMBOL(is_smap_pg_huge);

static void resource(void)
{
	cancel_delayed_work_sync(&migrate_back_work);
	free_obmm_dev();
	destroy_workqueue(migrate_back_wq);
	exit_migrate();
	smap_dev_exit();
	smap_debugfs_mod_exit();
#ifdef HAM_ENABLED
	ham_exit();
#endif
}

static void migrate_back_work_func(struct work_struct *work)
{
	struct migrate_back_subtask *subtask;
	struct migrate_back_task *task;
	struct migrate_back_task *prev_task = NULL;
	bool found;
	int i, ret;

	ret = iterate_obmm_dev();
	if (ret) {
		pr_err("failed to iterate obmm_dev in migrate back schedule, ret: %d\n",
		       ret);
	}

next:
	found = false;
	spin_lock(&migrate_back_task_lock);
	list_for_each_entry(task, &migrate_back_task_list, task_node) {
		if (task->status == MB_TASK_WAITING) {
			found = true;
			prev_task = task;
			break;
		}
	}
	spin_unlock(&migrate_back_task_lock);

	if (!found) {
		queue_delayed_work(migrate_back_wq, &migrate_back_work,
				   msecs_to_jiffies(MB_INTV));
		return;
	}

	task = prev_task;
	list_for_each_entry(subtask, &task->subtask, task_list) {
		if (node_is_critical_err(subtask->src_nid)) {
			pr_err_ratelimited("critical error on node %d\n",
					   subtask->src_nid);
			subtask->status = MB_SUBTASK_ERR;
		} else {
			for (i = 0; i < SUBTASK_RETRY_TIME; i++) {
				if (is_smap_pg_huge())
					smap_handle_migrate_back_subtask(
						subtask);
				else
					smap_handle_migrate_back_subtask_4k(
						subtask);
				if (subtask->status != MB_SUBTASK_ERR) {
					break;
				}
				msleep(SUBTASK_SLEEP_TIME);
				pr_debug("migrate back retry time %d\n", i);
			}
		}
		if (subtask->status == MB_SUBTASK_ERR) {
			task->status = MB_TASK_ERR;
		}
		task->progress += HUNDRED / task->subtask_cnt;
	}
	if (task->status != MB_TASK_ERR) {
		task->status = MB_TASK_DONE;
		task->progress = HUNDRED;
	}
	goto next;
}

static int __init tracking_init(void)
{
	int ret = 0;

	ret = smap_process_symbols();
	if (ret) {
		pr_err("smap process symbols failed\n");
		return ret;
	}
	ret = iterate_obmm_dev();
	if (ret) {
		pr_err("failed to iterate obmm_dev, ret: %d\n", ret);
		goto out_smap_node_sysfs;
	}
	migrate_back_wq = create_workqueue("smap_migrate_back_wq");
	if (!migrate_back_wq) {
		pr_err("failed to create migrate back workqueue\n");
		ret = -EAGAIN;
		goto out_smap_node_sysfs;
	}
	INIT_DELAYED_WORK(&migrate_back_work, migrate_back_work_func);
	ret = smap_debugfs_migrate_init();
	if (ret < 0) {
		pr_err("failed to init debugfs, ret: %d\n", ret);
		goto out_workqueue;
	}
	ret = smap_dev_init();
	if (ret < 0) {
		pr_err("failed to init SMAP device, ret: %d\n", ret);
		goto out_debugfs;
	}
	ret = init_migrate();
	if (ret < 0) {
		pr_err("failed to init migrate, ret: %d\n", ret);
		goto out_dev_int;
	}
#ifdef HAM_ENABLED
	ret = ham_init();
	if (ret < 0) {
		pr_err("failed to init HAM, ret: %d\n", ret);
		goto out_migrate_int;
	}
#endif
	queue_delayed_work(migrate_back_wq, &migrate_back_work,
			   msecs_to_jiffies(MB_INTV));
	pr_info("SMAP init successfully\n");
	return 0;
#ifdef HAM_ENABLED
out_migrate_int:
	exit_migrate();
#endif
out_dev_int:
	smap_dev_exit();
out_debugfs:
	smap_debugfs_mod_exit();
out_workqueue:
	if (migrate_back_wq)
		destroy_workqueue(migrate_back_wq);
out_smap_node_sysfs:
	free_obmm_dev();
	return ret;
}

static void __exit tracking_exit(void)
{
	resource();
	pr_info("SMAP exit successfully\n");
}

MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL v2");
module_init(tracking_init);
module_exit(tracking_exit);
