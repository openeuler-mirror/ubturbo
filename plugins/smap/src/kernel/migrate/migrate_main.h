/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP Tiering Memory Solution: SMAP MIGRATE_MAIN
 */

#ifndef _MIGRATE_MAIN_H
#define _MIGRATE_MAIN_H

#include "numa.h"
#include "smap_migrate_pages.h"
#include "smap_debugfs.h"
#include "smap_migrate_wrapper.h"
#include "migrate_dev.h"

int migrate_init(void);
void migrate_exit(void);

#endif /* _MIGRATE_MAIN_H */
