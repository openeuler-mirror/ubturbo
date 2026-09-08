// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP migrate-back ioctl module
 */

#include "numa.h"
#include "work.h"
#include "obmm_monitor.h"
#include "migrate_back_ioctl.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_migrate_back: " fmt

static int dup_task(unsigned long long task_id, struct list_head *head)
{
	struct migrate_back_task *t;

	list_for_each_entry(t, head, task_node) {
		if (t->task_id == task_id) {
			if (t->status == MB_TASK_ERR) {
				t->status = MB_TASK_WAITING;
				return RETRY_ID;
			}
			return DUP_ID;
		}
	}
	return NORMAL_ID;
}

static int check_duplicate_task(struct migrate_back_task *task)
{
	int task_ret;

	spin_lock(&migrate_back_task_lock);
	task_ret = dup_task(task->task_id, &migrate_back_task_list);
	if (task_ret == DUP_ID) {
		spin_unlock(&migrate_back_task_lock);
		pr_err("duplicated migrate back task id: %llu\n",
		       task->task_id);
		return DUP_ID;
	}
	if (task_ret == RETRY_ID) {
		spin_unlock(&migrate_back_task_lock);
		return RETRY_ID;
	}
	list_add(&task->task_node, &migrate_back_task_list);
	spin_unlock(&migrate_back_task_lock);

	return 0;
}

int smap_ioctl_migrate_back(struct migrate_back_inner_msg *msg)
{
	int i, ret, task_ret;
	struct migrate_back_task *task;
	struct migrate_back_subtask *subtask, *tmp;

	ret = 0;
	if (msg->count == 0) {
		pr_err("null message passed to migrate back\n");
		goto err_param;
	}

	ret = -ENOMEM;
	task = init_migrate_back_task(msg->task_id);
	if (!task) {
		pr_err("failed to init migrate back task\n");
		goto err_param;
	}

	/* Deduplicate */
	ret = -EINVAL;
	task_ret = check_duplicate_task(task);
	if (task_ret == DUP_ID) {
		goto err_dup_task;
	}
	if (task_ret == RETRY_ID) {
		kfree(task);
		return 0;
	}

	for (i = 0; i < msg->count; i++) {
		ret = init_migrate_back_subtask(task, &msg->payload[i],
						&subtask);
		if (ret < 0) {
			pr_err("failed to init migrate back subtask, source node: %d, destination node: %d\n",
			       msg->payload[i].src_nid,
			       msg->payload[i].dest_nid);
			goto err_subtask;
		}
		list_add(&subtask->task_list, &task->subtask);
	}
	task->subtask_cnt = msg->count;
	clear_migrate_back_task();
	return start_migrate_back_work();

err_subtask:
	list_for_each_entry_safe(subtask, tmp, &task->subtask, task_list) {
		list_del(&subtask->task_list);
		kfree(subtask);
	}
	spin_lock(&migrate_back_task_lock);
	list_del(&task->task_node);
	spin_unlock(&migrate_back_task_lock);
err_dup_task:
	kfree(task);
err_param:
	return ret;
}

static int check_migrate_back_msg(struct migrate_back_msg *mb_msg)
{
	int i;
	if (mb_msg->count <= 0 || mb_msg->count > MAX_NR_MIGBACK) {
		pr_err("invalid message count passed to migrate back\n");
		return -EINVAL;
	}

	for (i = 0; i < mb_msg->count; i++) {
		struct migrate_back_payload *p = &(mb_msg->payload[i]);
		if (is_node_invalid(p->src_nid)) {
			pr_err("invalid source node: %d of %dth message\n", i,
			       p->src_nid);
			return -EINVAL;
		}
		if (p->dest_nid != NUMA_NO_NODE &&
		    is_node_invalid(p->dest_nid)) {
			pr_err("invalid destination node: %d of %dth message\n",
			       i, p->dest_nid);
			return -EINVAL;
		}
	}

	return 0;
}

static int __ioctl_migrate_back(void __user *argp)
{
	int i, ret;
	struct migrate_back_msg mb_msg;
	struct migrate_back_inner_msg *mb_imsg;

	if (copy_from_user(&mb_msg, argp, sizeof(mb_msg))) {
		pr_err("failed to copy migrate back message from user space\n");
		return -EFAULT;
	}

	ret = check_migrate_back_msg(&mb_msg);
	if (ret)
		return ret;

	mb_imsg = kmalloc(sizeof(*mb_imsg), GFP_KERNEL);
	if (!mb_imsg) {
		pr_err("failed to malloc migrate back inner msg\n");
		return -ENOMEM;
	}
	mb_imsg->task_id = mb_msg.task_id;
	mb_imsg->count = mb_msg.count;

	for (i = 0; i < mb_msg.count; i++) {
		struct migrate_back_payload *p = &(mb_msg.payload[i]);
		struct migrate_back_inner_payload *i_p = &(mb_imsg->payload[i]);

		ret = find_range_by_memid(p->memid, &i_p->pa_start,
					  &i_p->pa_end);
		if (ret) {
			pr_err("unable to find range of memid: %llu, ret: %d\n",
			       p->memid, ret);
			goto free_imsg;
		}

		if (smap_is_remote_addr_valid(p->src_nid, i_p->pa_start,
					      i_p->pa_end)) {
			pr_err("memory range mismatch with node: %d\n",
			       p->src_nid);
			ret = -EINVAL;
			goto free_imsg;
		}

		i_p->src_nid = p->src_nid;
		i_p->dest_nid = p->dest_nid;
	}

	ret = smap_ioctl_migrate_back(mb_imsg);

free_imsg:
	kfree(mb_imsg);
	return ret;
}

long smap_migrate_back_ioctl(void __user *argp)
{
	return __ioctl_migrate_back(argp);
}
