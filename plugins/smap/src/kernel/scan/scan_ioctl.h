/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap scan ioctl module
 */

#ifndef _SRC_SCAN_IOCTL_H
#define _SRC_SCAN_IOCTL_H

#include <linux/proc_fs.h>
#include <linux/types.h>

#include "check.h"
#include "common.h"
#include "drv_common.h"
#include "ub_hist.h"

#define SCAN_DEV "smap_scan_dev"
#define SCAN_CLASS "smap_scan_class"
#define SCAN_DEVICE "smap_scan_dev"
#define SMAP_PROC_ROOT "smap"

struct actc_data {
	actc_t freq; /* 访问频次 */
	u8 flags; /* 位域:bit0=白名单页, bit1=已选中, bits2-7=优先级 */
} __attribute__((packed));

#define ACTC_WHITE_LIST_BIT BIT(0)
#define ACTC_SELECT_BIT BIT(1)
#define ACTC_PRIOR_GET(f) (((f) >> 2) & 0x3F)
#define ACTC_PRIOR_SET(p) (((p) & 0x3F) << 2)

typedef enum {
	NO_SCAN = -1,
	HAM_SCAN,
	NORMAL_SCAN,
	STATISTIC_SCAN,
	MAX_SCAN_TYPE,
} scan_type;

/* per-pid 身份，镜像用户态 PidType（PROCESS_TYPE=0 / VM_TYPE=1），数值须一致 */
typedef enum {
	SMAP_PID_PROCESS = 0,
	SMAP_PID_VM,
} smap_pid_type;

struct access_add_pid_payload {
	pid_t pid;
	u32 numa_nodes;
	u32 scan_time;
	u32 duration;
	scan_type type;
	u32 ntimes;
	smap_pid_type pid_type;
};

struct access_add_pid_msg {
	int count;
	struct access_add_pid_payload *payload;
};

struct access_remove_pid_payload {
	pid_t pid;
};

struct access_remove_pid_msg {
	int count;
	struct access_remove_pid_payload *payload;
};

struct tracking_info_payload {
	pid_t pid;
	u32 length;
	u16 *data; /* DFX 统计扫描频次，保留原始 u16 真值，不压缩 */
};

struct access_pid_freq_msg {
	pid_t pid;
	size_t len[SMAP_MAX_NUMNODES];
	actc_t *freq[SMAP_MAX_NUMNODES];
};

struct user_info {
	uid_t uid;
	gid_t gid;
};

struct smap_scan_cpu_range {
	u32 cpu_min;
	u32 cpu_max;
};

extern kuid_t procfs_kuid;
extern kgid_t procfs_kgid;
extern struct proc_dir_entry *smap_procfs_root;

#define SMAP_ACCESS_ADD_PID \
	_IOW(SMAP_ACCESS_MAGIC, 1, struct access_add_pid_msg)
#define SMAP_ACCESS_REMOVE_PID \
	_IOW(SMAP_ACCESS_MAGIC, 2, struct access_remove_pid_msg)
#define SMAP_ACCESS_REMOVE_ALL_PID _IOW(SMAP_ACCESS_MAGIC, 3, int)
#define SMAP_ACCESS_WALK_PAGEMAP _IOW(SMAP_ACCESS_MAGIC, 4, size_t)
#define SMAP_ACCESS_GET_TRACKING \
	_IOW(SMAP_ACCESS_MAGIC, 5, struct tracking_info_payload)
#define SMAP_ACCESS_CREATE_PROCFS _IOW(SMAP_ACCESS_MAGIC, 6, struct user_info)
#define SMAP_ACCESS_GET_NR_LOCAL_NUMA _IOR(SMAP_ACCESS_MAGIC, 7, int)
#define SMAP_ACCESS_REFRESH_REMOTE_RAM _IO(SMAP_ACCESS_MAGIC, 8)
#define SMAP_ACCESS_SET_SCAN_CPU \
	_IOW(SMAP_ACCESS_MAGIC, 9, struct smap_scan_cpu_range)

#define SMAP_IOCTL_TRACKING_CMD _IOW(SMAP_ACCESS_MAGIC, 0, unsigned long)
#define SMAP_IOCTL_PAGE_SIZE_SET_CMD _IOW(SMAP_ACCESS_MAGIC, 10, unsigned long)
#define SMAP_IOCTL_UB_WATCH_CMD \
	_IOR(SMAP_ACCESS_MAGIC, 11, struct ub_flux_mb_statistic)

struct ub_watch_config {
	uint32_t duration_ms;
};

#define SMAP_IOCTL_UB_WATCH_CONFIG_CMD \
	_IOW(SMAP_ACCESS_MAGIC, 12, struct ub_watch_config)

enum node_tracking_cmd {
	TRACKING_DISABLED,
	TRACKING_ENABLED,
};

void scan_ioctl_exit(void);
int scan_ioctl_init(void);

#endif /* _SRC_SCAN_IOCTL_H */
