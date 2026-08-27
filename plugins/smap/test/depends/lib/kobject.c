/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/kobject.h>

struct kobject *kernel_kobj;

void kobject_put(struct kobject *kobj)
{}

int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
			 struct kobject *parent, const char *fmt, ...)
{
	return 0;
}

struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	(void)name;
	(void)parent;
	/* DT stub: return a stable non-NULL kobject so callers that attach
	 * sysfs attributes proceed; sysfs ops are themselves stubbed. */
	static struct kobject kobj;
	return &kobj;
}
