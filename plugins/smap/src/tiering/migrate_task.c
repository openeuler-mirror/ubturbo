// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP migrate task module
 */
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include "migrate_back_debugfs.h"
#include "migrate_task.h"

#define MAX_MIGRATE_BACK_TASK_COUNT 100

LIST_HEAD(migrate_back_task_list);
DEFINE_SPINLOCK(migrate_back_task_lock);

struct migrate_back_task *init_migrate_back_task(unsigned long long task_id)
{
	struct migrate_back_task *task;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return NULL;
	kref_init(&task->ref);
	task->task_id = task_id;
	INIT_LIST_HEAD(&task->subtask);
	return task;
}

/* kref 归零回调：清理 subtask、移除 debugfs 文件并释放任务内存。
 * 可能在持锁调用方（clear/free_all）解锁后执行，函数本身不加锁；
 * remove_migrate_back_debugfs 幂等，文件不存在时为 no-op */
void migrate_back_task_release(struct kref *ref)
{
	struct migrate_back_task *task =
		container_of(ref, struct migrate_back_task, ref);
	struct migrate_back_subtask *s, *stmp;

	list_for_each_entry_safe(s, stmp, &task->subtask, task_list) {
		list_del(&s->task_list);
		kfree(s);
	}
	remove_migrate_back_debugfs(task);
	kfree(task);
}

void free_all_migrate_back_task(void)
{
	struct migrate_back_task *t, *ttmp;
	LIST_HEAD(dead);

	/* 锁内只做摘链：release 中 debugfs 操作可能睡眠，不能持 spinlock 执行 */
	spin_lock(&migrate_back_task_lock);
	list_for_each_entry_safe(t, ttmp, &migrate_back_task_list, task_node) {
		list_del(&t->task_node);
		list_add(&t->task_node, &dead);
	}
	spin_unlock(&migrate_back_task_lock);

	list_for_each_entry_safe(t, ttmp, &dead, task_node)
		kref_put(&t->ref, migrate_back_task_release);
}

int init_migrate_back_subtask(struct migrate_back_task *task,
			      struct migrate_back_inner_payload *p,
			      struct migrate_back_subtask **result)
{
	struct migrate_back_subtask *subtask;

	subtask = kzalloc(sizeof(*subtask), GFP_KERNEL);
	if (!subtask) {
		return -ENOMEM;
	}

	subtask->src_nid = p->src_nid;
	subtask->dest_nid = p->dest_nid;
	subtask->pa_start = p->pa_start;
	subtask->pa_end = p->pa_end;
	subtask->task = task;
	subtask->isolated_flag = true;
	*result = subtask;

	return 0;
}

void clear_migrate_back_task(void)
{
	int count = 0;
	struct migrate_back_task *t, *ttmp;
	LIST_HEAD(dead);

	/* 锁内只做摘链：release 中 debugfs 操作可能睡眠，不能持 spinlock 执行 */
	spin_lock(&migrate_back_task_lock);
	list_for_each_entry_safe(t, ttmp, &migrate_back_task_list, task_node) {
		count++;
		if (count > MAX_MIGRATE_BACK_TASK_COUNT &&
		    t->status > MB_TASK_WAITING) {
			list_del(&t->task_node);
			list_add(&t->task_node, &dead);
		}
	}
	spin_unlock(&migrate_back_task_lock);

	list_for_each_entry_safe(t, ttmp, &dead, task_node)
		kref_put(&t->ref, migrate_back_task_release);
}
