/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP page flags for accumulated access count
 */

#ifndef _SMAP_PAGE_FLAGS_H
#define _SMAP_PAGE_FLAGS_H

#include <linux/mm.h>
#include <linux/mmzone.h>
#include "access_ioctl.h"

/*
 * 使用LAST_CPUPID字段存储累计访问频次
 * SMAP插件禁用了numa_balance，因此LAST_CPUPID是闲置的
 * 页面释放时LAST_CPUPID会自动重置为全1
 * 衰减机制：每SMAP_ACC_CNT_MAX次扫描周期执行一次衰减，
 * acc_times达到SMAP_ACC_CNT_MAX时归零，当前扫描周期标记为衰减周期。
 * 衰减时先右移再递增，保证prior值在SMAP_ACC_CNT_MAX以内。
 */

#define SMAP_ACC_BITS ACTC_PRIOR_BITS
#define SMAP_ACC_MASK ((1ULL << SMAP_ACC_BITS) - 1)
#define SMAP_ACC_CNT_MAX (SMAP_ACC_MASK - 1)
#define SMAP_ACC_CNT_INIT SMAP_ACC_MASK

/*
 * 获取累计访问频次
 */
static inline u8 get_smap_acc_cnt(struct page *page)
{
	unsigned long flags = READ_ONCE(page->flags);
	u8 cnt = (flags >> LAST_CPUPID_PGSHIFT) & SMAP_ACC_MASK;
	if (cnt == SMAP_ACC_CNT_INIT)
		return 0;

	return min_t(u8, cnt, SMAP_ACC_CNT_MAX);
}

/*
 * inc_smap_acc_cnt - 累计访问频次加一
 * @page: 目标页面
 * @decay: 是否执行衰减（当前扫描周期为衰减周期时为true）
 *
 * decay为true时，先右移衰减再递增：stored=N → (N>>1)+1
 * decay为false时，直接递增：stored=N → N+1
 * 使用cmpxchg实现原子更新，失败则跳过（不重试）。
 */
static inline void inc_smap_acc_cnt(struct page *page, bool decay)
{
	unsigned long old_flags, new_flags;
	u8 cnt, new_cnt;

	old_flags = READ_ONCE(page->flags);
	cnt = (old_flags >> LAST_CPUPID_PGSHIFT) & SMAP_ACC_MASK;
	if (cnt == SMAP_ACC_CNT_INIT)
		cnt = 0;
	new_cnt = (cnt >> decay);
	if (new_cnt >= SMAP_ACC_CNT_MAX)
		return;

	new_flags = old_flags & ~(SMAP_ACC_MASK << LAST_CPUPID_PGSHIFT);
	new_flags |= ((unsigned long)(++new_cnt) << LAST_CPUPID_PGSHIFT);
	cmpxchg(&page->flags, old_flags, new_flags);
}

#endif /* _SMAP_PAGE_FLAGS_H */
