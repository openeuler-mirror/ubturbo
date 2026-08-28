// SPDX-License-Identifier: GPL-2.0-only
/*
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/dev_printk.h>
#include <linux/kobject.h>
#include <linux/types.h>
#include <linux/dma-map-ops.h>
#include <linux/device/bus.h>
#include <linux/device/driver.h>
#include <linux/device/class.h>
#ifdef __cplusplus
extern "C" {
#endif

struct device {
    struct kobject kobj;
    struct device *parent;
    int        numa_node; // NUMA node this device is close to
    struct  device_driver *driver;
    struct  bus_type *bus;
    void *driver_data;
    dev_t            devt; // dev_t, creates the sysfs "dev"
    struct class_stub *class_stub;
    u32 scan_time;
    void    (*release)(struct device *dev);
    const char      *init_name;
    const struct dma_map_ops *dma_ops;
    u64 *dma_mask;
};


void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t flags);

struct device *device_create(struct class_stub *cls, struct device *parent,
    dev_t devt, void *drvdata, const char *fmt, ...);
void device_destroy(struct class_stub *cls, dev_t devt);

static inline const char *kobject_name(const struct kobject *kobj)
{
    return kobj->name;
}

static inline const char *dev_name(const struct device *dev)
{
    if (dev->init_name) {
        return dev->init_name;
    }
    return kobject_name(&dev->kobj);
}

#ifdef __cplusplus
}
#endif

#endif // _DEVICE_H_
