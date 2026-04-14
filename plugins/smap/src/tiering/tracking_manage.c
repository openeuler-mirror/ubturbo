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

#include "migrate_task.h"
#include "smap_migrate_wrapper.h"
#include "dump_info.h"
#include "acpi_mem.h"
#include "iomem.h"
#include "ham_migration.h"
#include "pid_ioctl.h"
#include "trouble_numa_meta.h"
#include "ubus_notify.h"
#include "tracking_manage.h"

#define SMAP_WATCH_NAME "smap_migrate_result"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_track_manage: " fmt

int node_modes[SMAP_MAX_NUMNODES] = {
	[0 ... SMAP_MAX_NUMNODES - 1] = INVALID_DATA_MODE,
};
module_param_array(node_modes, int, NULL, S_IRUGO);
MODULE_PARM_DESC(
	node_modes,
	"SMAP data mode selection: SKIP = -1, HIST_MODE = 0, CPI_MODE_AND = 1, "
	"CPI_MODE_SUM = 2, CPI_MODE_OR = 3, ACCESS_MODE_AND = 4, ACCESS_MODE_SUM = 5, ACCESS_MODE_OR = 6,"
	"MEBS_MODE = 7, MEBS_MODE_4B = 8, MEBS_MODE_6B = 9, PLDA_HDT_MODE = 10, PLDA_HDT_DECAY_MODE = 11, default skip");
EXPORT_SYMBOL_GPL(node_modes);

unsigned int smap_pgsize = HUGE_PAGE;
module_param(smap_pgsize, uint, S_IRUGO);
MODULE_PARM_DESC(smap_pgsize, "SMAP migration page size: 0 for 4K, 1 for 2M, "
			      "default 2M");
EXPORT_SYMBOL_GPL(smap_pgsize);

unsigned int smap_mode = VM_MODE;
module_param(smap_mode, uint, S_IRUGO);
MODULE_PARM_DESC(smap_mode, "SMAP migrate mode: 0 for baremetal, 1 for VM, "
			    "2 for process, default VM");
EXPORT_SYMBOL_GPL(smap_mode);

unsigned int smap_scene = NORMAL_SCENE;
module_param(smap_scene, uint, S_IRUGO);
MODULE_PARM_DESC(smap_scene, "SMAP usage scenarios: 0 for HCCS, 1 for UB_QEMU");

char *qemu_name = "qemu-kvm";
module_param(qemu_name, charp, S_IRUGO);
MODULE_PARM_DESC(qemu_name, "qemu_name string to match");
EXPORT_SYMBOL_GPL(qemu_name);
#define MB_INTV 1000
#define SUBTASK_RETRY_TIME 1000
#define SUBTASK_SLEEP_TIME 5

struct workqueue_struct *migrate_back_wq = NULL;
struct delayed_work migrate_back_work;

bool is_smap_pg_huge(void)
{
	return smap_pgsize == HUGE_PAGE;
}
EXPORT_SYMBOL(is_smap_pg_huge);

static void resource(void)
{
	cancel_delayed_work_sync(&migrate_back_work);
	release_remote_ram();
	reset_acpi_mem();
	destroy_workqueue(migrate_back_wq);
	exit_migrate();
	smap_dev_exit();
	smap_debugfs_mod_exit();
	ham_exit();
}

static int is_qemu_name_valid(void)
{
	if (!qemu_name || strlen(qemu_name) == 0 ||
	    strlen(qemu_name) > QEMU_NAME_LEN) {
		pr_err("invalid qemu name\n");
		return -EINVAL;
	}
	return 0;
}

int is_smap_args_valid(void)
{
	int i;

	if (smap_pgsize >= NR_PGSIZE_ARGS) {
		return -EINVAL;
	}
	if (smap_mode >= NR_MODE_ARGS) {
		return -EINVAL;
	}
	for (i = 0; i < SMAP_MAX_NUMNODES; i++) {
		if (node_modes[i] != INVALID_DATA_MODE &&
		    is_data_mode_invalid(node_modes[i])) {
			return -EINVAL;
		}
	}
	return is_qemu_name_valid();
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
		if (is_trouble_numa(subtask->src_nid)) {
			pr_err("migrate back is trouble numa(%d), stop migrate.\n", subtask->src_nid);
			subtask->status = MB_SUBTASK_ERR;
		} else {
			for (i = 0; i < SUBTASK_RETRY_TIME; i++) {
				if (is_smap_pg_huge())
					smap_handle_migrate_back_subtask(subtask);
				else
					smap_handle_migrate_back_subtask_4k(subtask);
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

	ret = is_smap_args_valid();
	if (ret < 0) {
		pr_err("invalid module arguments\n");
		return ret;
	}
	ret = smap_process_symbols();
	if (ret) {
		pr_err("smap process symbols failed\n");
		return ret;
	}
	ret = hisi_ubus_register_link_down_notifier();
	if (ret) {
		pr_err("failed to register vendor record notifier, ret: %d\n", ret);
		return ret;
	}
	init_trouble_numa_manager();
	ret = init_acpi_mem();
	if (ret < 0) {
		pr_err("failed to init ACPI memory, ret: %d\n", ret);
		return ret;
	}
	(void)refresh_remote_ram();
	if (smap_scene != UB_QEMU_SCENE_ADVANCED) {
		if (list_empty(&remote_ram_list)) {
			ret = -EINVAL;
			pr_err("remote NUMA is not detected\n");
			goto out_smap_node_sysfs;
		}
		pr_info("remote NUMA has been detected\n");
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
	ret = ham_init();
	if (ret < 0) {
		pr_err("failed to init HAM, ret: %d\n", ret);
		goto out_migrate_int;
	}
	queue_delayed_work(migrate_back_wq, &migrate_back_work,
			   msecs_to_jiffies(MB_INTV));
	pr_info("SMAP init successfully\n");
	return 0;
out_migrate_int:
	exit_migrate();
out_dev_int:
	smap_dev_exit();
out_debugfs:
	smap_debugfs_mod_exit();
out_workqueue:
	if (migrate_back_wq)
		destroy_workqueue(migrate_back_wq);
out_smap_node_sysfs:
	release_remote_ram();
	reset_acpi_mem();
	return ret;
}

static void __exit tracking_exit(void)
{
	resource();
	cleanup_trouble_numa_manager();
	hisi_ubus_unregister_link_down_notifier();
	pr_info("SMAP exit successfully\n");
}

MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL v2");
module_init(tracking_init);
module_exit(tracking_exit);
