/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef _SMAP_NETLINK_H
#define _SMAP_NETLINK_H

int smap_netlink_init(void);

void smap_netlink_exit(void);

int smap_send_info_to_user(const void *data, size_t data_len);

#endif