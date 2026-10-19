/* SPDX-License-Identifier: GPL-2.0-only */
/*
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/

#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/errno.h>
#include <linux/major.h>
#include <linux/module.h>

#include <linux/cdev.h>
void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
}

int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)
{
    return 0;
}

void unregister_chrdev_region(dev_t from, unsigned count)
{
}

int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
    return 0;
}

void cdev_del(struct cdev *p)
{
}