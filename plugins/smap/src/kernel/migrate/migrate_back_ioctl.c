// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP migrate-back ioctl module
 */

#include "numa.h"
#include "work.h"
#include "obmm_monitor.h"
#include "migrate_back_ioctl.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_migrate_back: " fmt

/* 同步等待内核迁回任务完成的上限，对齐用户态 MIGRATE_BACK ready 等待时长 */
#define MIGBACK_SYNC_WAIT_MS 60000

static int dup_task(unsigned long long task_id, struct list_head *head)
{
	struct migrate_back_task *t;

	list_for_each_entry(t, head, task_node) {
		if (t->task_id == task_id) {
			if (t->status == MB_TASK_ERR) {
				t->status = MB_TASK_WAITING;
				/* 上个 ERR 生命周期已经 complete 过，重投前重置
				 * completion，否则同步等待拿到的是陈旧信号 */
				reinit_completion(&t->done);
				return RETRY_ID;
			}
			return DUP_ID;
		}
	}
	return NORMAL_ID;
}

static struct migrate_back_task *
find_task_by_id_locked(unsigned long long task_id)
{
	struct migrate_back_task *t;

	list_for_each_entry(t, &migrate_back_task_list, task_node) {
		if (t->task_id == task_id)
			return t;
	}
	return NULL;
}

/* 等待迁回任务完成，并按 task ID 查询结果；已清理的任务视为完成。 */
static int wait_migrate_back_result(unsigned long long task_id)
{
	struct migrate_back_task *t;
	unsigned long left;
	int status;

	spin_lock(&migrate_back_task_lock);
	t = find_task_by_id_locked(task_id);
	if (t)
		kref_get(&t->ref);
	spin_unlock(&migrate_back_task_lock);
	if (!t) {
		pr_err("migrate back task %llu disappeared before wait\n",
		       task_id);
		return -EAGAIN;
	}

	left = wait_for_completion_timeout(
		&t->done, msecs_to_jiffies(MIGBACK_SYNC_WAIT_MS));
	spin_lock(&migrate_back_task_lock);
	status = find_task_by_id_locked(task_id) == t ? (int)t->status
						      : MB_TASK_DONE;
	spin_unlock(&migrate_back_task_lock);
	kref_put(&t->ref, migrate_back_task_release);

	if (left == 0)
		return -ETIMEDOUT;
	if (status == MB_TASK_ERR)
		return -EAGAIN;
	return 0;
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
	struct migrate_back_subtask *subtask;

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
		kref_put(&task->ref, migrate_back_task_release);
		/* 残留 ERR 任务已重置回 WAITING，同步等待新一轮执行结果 */
		return wait_migrate_back_result(msg->task_id);
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
	ret = start_migrate_back_work();
	if (ret)
		return ret;
	/* 入队成功后等待迁回任务完成并返回状态。 */
	return wait_migrate_back_result(msg->task_id);

err_subtask:
	/* 引用释放交给 migrate_back_task_release：subtask 清理与 debugfs
	 * 移除统一在其内完成（此时尚未创建 debugfs 文件，为 no-op） */
	spin_lock(&migrate_back_task_lock);
	list_del(&task->task_node);
	spin_unlock(&migrate_back_task_lock);
	kref_put(&task->ref, migrate_back_task_release);
	return ret;
err_dup_task:
	/* 任务从未入列表，引用计数为 1，put 即释放 */
	kref_put(&task->ref, migrate_back_task_release);
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
