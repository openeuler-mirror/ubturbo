/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: SMAP Tiering Memory Solution: tracking_dev
 */

#ifndef __TRACKING_IOCTL_H__
#define __TRACKING_IOCTL_H__

#include <linux/types.h>

#include "drv_common.h"

#define SMAP_IOCTL_TRACKING_CMD _IOW('N', 0, unsigned long)

#define SMAP_IOCTL_MODE_SET_CMD _IOW('N', 1, unsigned long)

#define SMAP_IOCTL_MAP_MODE_CMD _IOW('N', 2, unsigned long)

#define SMAP_IOCTL_GET_SIZE_CMD _IOR('N', 3, unsigned long)

#define SMAP_IOCTL_PAGE_SIZE_SET_CMD _IOW('N', 4, unsigned long)

enum node_tracking_cmd {
	TRACKING_DISABLED,
	TRACKING_ENABLED,
};

#endif
