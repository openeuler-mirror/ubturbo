/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdarg.h>
#include <stdio.h>
#include <linux/kobject.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

int sysfs_create_group(struct kobject *kobj,
		       const struct attribute_group *grp)
{
	return 0;
}

void sysfs_remove_group(struct kobject *kobj,
			const struct attribute_group *grp)
{}

void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr)
{}

int sysfs_create_file(struct kobject *kobj, const struct attribute *attr)
{
	return 0;
}

ssize_t sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);
	return ret;
}
