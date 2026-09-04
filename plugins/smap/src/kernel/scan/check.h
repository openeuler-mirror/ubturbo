/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 * Description: SMAP3.0 Tiering Memory Solution: Self-check common configuration
 */

#ifndef DRIVERS_CHECK_H
#define DRIVERS_CHECK_H

#include "kernel_common.h"

#define GB_TO_NORMAL_PAGE_SHIFT (30 - PAGE_SHIFT)
#define HUGE_TO_NORMAL_PAGE_SHIFT (__builtin_ctz(g_pagesize_huge) - PAGE_SHIFT)

#define PAGE_SIZE_64K (1UL << 16)

extern u32 g_pagesize_huge;

#endif /* DRIVERS_CHECK_H */
