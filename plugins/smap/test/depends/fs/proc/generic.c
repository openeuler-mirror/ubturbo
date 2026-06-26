#include <linux/proc_fs.h>
#include <linux/uidgid.h>

void proc_set_user(struct proc_dir_entry *de, kuid_t uid, kgid_t gid)
{
	return;
}

struct proc_dir_entry *proc_create_data(const char *name, umode_t mode,
				struct proc_dir_entry *parent,
				const struct proc_ops *proc_ops, void *data)
{
	return NULL;
}
