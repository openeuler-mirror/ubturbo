/* SPDX-License-Identifier: GPL-2.0-only */
/*
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/
#include <stdlib.h>
#include <string.h>
#include <linux/device.h>

struct device *device_create(struct class_stub *cls, struct device *parent,
    dev_t devt, void *drvdata, const char *fmt, ...)
{
    struct device *dev = (struct device *)malloc(sizeof(struct device));
    if (dev == NULL) {
        return NULL;
    }
    char *name = (char *)malloc(sizeof(fmt));
    if (!strcpy(name, fmt)) {
        return NULL;
    }
    dev->init_name = name;
    return dev;
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
    return calloc(1, size);
}

void device_destroy(struct class_stub *cls, dev_t devt)
{
}

void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t flags)
{
    return calloc(n, size);
}

unsigned long _compound_head(struct page *page)
{
    return 0;
}