/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP migrate-back ioctl module
 */

#ifndef _SRC_MIGRATE_BACK_IOCTL_H
#define _SRC_MIGRATE_BACK_IOCTL_H

#include <linux/types.h>

#include "common.h"
#include "migrate_task.h"

extern int nr_local_numa;

struct migrate_back_inner_msg {
	unsigned long long task_id;
	int count;
	struct migrate_back_inner_payload payload[MAX_NR_MIGBACK];
};

struct migrate_back_payload {
	int src_nid;
	int dest_nid;
	u64 memid;
};

enum task_id_mode {
	NORMAL_ID,
	DUP_ID,
	RETRY_ID,
};

struct migrate_back_msg {
	unsigned long long task_id;
	int count;
	struct migrate_back_payload payload[MAX_NR_MIGBACK];
};

#define SMAP_MIGRATE_BACK _IOW(SMAP_MIGRATE_MAGIC, 5, struct migrate_back_msg)
extern int smap_is_remote_addr_valid(int nid, u64 pa_start, u64 pa_end);
long smap_migrate_back_ioctl(void __user *argp);

#endif /* _SRC_MIGRATE_BACK_IOCTL_H */
