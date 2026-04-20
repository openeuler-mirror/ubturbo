/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP : hist_ops
 */

#ifndef _SRC_HIST_OPS_H
#define _SRC_HIST_OPS_H

#include <linux/device.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include "ub_hist.h"

#define HIST_ADDR_SHIFT_4K (12)
#define HIST_ADDR_SHIFT_2M (21)
#define HIST_ADDR_SHIFT_32M (25)
#define HIST_ADDR_SHIFT_16G (34)
#define HIST_ADDR_SHIFT_128M (27)
#define HIST_ADDR_SHIFT_64G (36)

#define HIST_STS_BA_CNT (2)
#define HIST_STS_DIE_CNT (2)
#define HIST_STS_DEV_CNT (HIST_STS_BA_CNT * HIST_STS_DIE_CNT)
#define WORD_BYTE_NUM (0x4)
#define HIST_STS_VALUE_WORD_NUM (16384)
#define STS_PER_WORD (2)
#define HIST_STS_VALUE_NUM (HIST_STS_VALUE_WORD_NUM * STS_PER_WORD)
#define HIST_THREAD_PERIOD (512)
#define HIST_SCAN_DURATION_PER_WIN (64)
#define PAGE_SIZE_64K_DIV_4K 16

#define THREAD_SLEEP (50)

struct addr_seg {
	u64 start;
	u64 size;
	u16 max;
	u64 ba_tag;
};

struct segs_info {
	u32 cnt;
	struct addr_seg *segs;
};

union smap_hist_status {
	struct status {
		u32 new_pgsize : 22;
		u32 should_update_iomem : 1;
		u32 unused : 9;
	} flag;
	u32 status_all;
};

typedef void (*flush_actc)(void);

struct smap_hist_dev {
	struct task_struct *kthread;
	struct segs_info info;
	struct ub_hist_ba_info *ba_info;
	u32 freq_register_cnt;
	u32 scan_wins_num_per_ba;
	u32 ba_cnt;
	u32 period;
	u16 *buf;
	u32 pgcount;
	u32 pgsize;
	u8 thread_enable;
	u8 abort_flag;
	flush_actc flush_actc;
	union smap_hist_status status;
	ub_hist_smap_type hw_type;
};

struct hist_ops {
	void (*read)(u16 *dst_buf, struct addr_seg *seg);
	void (*update_pgsize)(u32 pgsize);
};

struct smap_hist_dev *get_hist_dev(void);
int hist_init(u32 pgsize);
void hist_deinit(void);
void fetch_hist_actc_buf(u16 *dst_buf, struct addr_seg *seg);
void hist_update_pgsize(u32 pgsize);
void hist_set_iomem(void);
void hist_thread_pause(void);
void hist_thread_resume(void);
void hist_set_flush_actc_cb(flush_actc cb);

#endif