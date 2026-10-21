// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP migrate back debugfs module
 */

#define DEBUGFS_RO_MODE 0444
#define BUF_MAX_SIZE 256
#define NAME_MAX_LEN 50
#define PREFIX "mb"

#include <linux/debugfs.h>

#include "migrate_task.h"
#include "migrate_back_debugfs.h"

ssize_t smap_migrate_back_debugfs_read(struct file *filp, char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	char buf[BUF_MAX_SIZE];
	ssize_t ret;
	struct migrate_back_task *task = filp->private_data;

	ret = scnprintf(buf, BUF_MAX_SIZE, "%d\n%u\n", task->status,
			task->progress);
	return simple_read_from_buffer(user_buf, count, ppos, buf, ret);
}

/* open 与 clear/free_all 并发时任务可能已脱离列表并被释放，
 * 须持锁确认任务仍在 migrate_back_task_list 中才允许取引用 */
static bool migrate_back_task_is_listed(struct migrate_back_task *task)
{
	struct migrate_back_task *t;

	list_for_each_entry(t, &migrate_back_task_list, task_node) {
		if (t == task)
			return true;
	}
	return false;
}

int smap_migrate_back_debugfs_open(struct inode *inode, struct file *filp)
{
	struct migrate_back_task *task = inode->i_private;

	spin_lock(&migrate_back_task_lock);
	if (!migrate_back_task_is_listed(task)) {
		spin_unlock(&migrate_back_task_lock);
		return -ENODEV;
	}
	kref_get(&task->ref);
	spin_unlock(&migrate_back_task_lock);
	filp->private_data = task;
	return 0;
}

int smap_migrate_back_debugfs_release(struct inode *inode, struct file *filp)
{
	struct migrate_back_task *task = filp->private_data;

	kref_put(&task->ref, migrate_back_task_release);
	return 0;
}

struct file_operations smap_migrate_back_debugfs_ops = {
	.owner = THIS_MODULE,
	.open = smap_migrate_back_debugfs_open,
	.read = smap_migrate_back_debugfs_read,
	.release = smap_migrate_back_debugfs_release,
	.llseek = default_llseek,
};

void create_migrate_back_debugfs(struct migrate_back_task *task)
{
	char file_name[NAME_MAX_LEN];
	struct dentry *dentry;

	int rc = scnprintf(file_name, NAME_MAX_LEN, "%s_%llu", PREFIX,
			   task->task_id);
	if (!rc)
		return;
	dentry = debugfs_lookup(file_name, dbg_root);
	if (dentry) {
		debugfs_remove(dentry);
		dput(dentry);
	}
	debugfs_create_file(file_name, DEBUGFS_RO_MODE, dbg_root, task,
			    &smap_migrate_back_debugfs_ops);
}

void remove_migrate_back_debugfs(struct migrate_back_task *task)
{
	char file_name[NAME_MAX_LEN];
	struct dentry *dentry;

	int rc = scnprintf(file_name, NAME_MAX_LEN, "%s_%llu", PREFIX,
			   task->task_id);
	if (!rc)
		return;
	dentry = debugfs_lookup(file_name, dbg_root);
	if (dentry) {
		debugfs_remove(dentry);
		dput(dentry);
	}
}
