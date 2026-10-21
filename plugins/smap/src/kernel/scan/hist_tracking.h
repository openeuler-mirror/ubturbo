/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP : hist_dev
 */

#ifndef _SRC_HIST_TRACKING_H
#define _SRC_HIST_TRACKING_H

#include <linux/types.h>
int hist_module_init(void);
void hist_tracking_enable(void);
int hist_tracking_disable(void);
int hist_tracking_set_page_size(u8 page_size_mode);
int hist_tracking_ub_watch(void *result);
int hist_tracking_ub_watch_config(u32 duration_ms);
bool is_hist_tracking_node(int node);

#endif
