/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP3.0 Tiering Memory Solution: tracking_dev
 */

#ifndef __TRACKING_PRIVATE_H__
#define __TRACKING_PRIVATE_H__

#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/hrtimer.h>

int tracking_bus_init(void);
void tracking_bus_exit(void);

struct tracking_node_dev {
	struct list_head list;
	struct list_head dev_list;
	struct device *device;
	struct device cdev_device;
	struct cdev cdev;
	struct file *file;
	int target_node;
};

#endif /* __TRACKING_PRIVATE_H__ */
