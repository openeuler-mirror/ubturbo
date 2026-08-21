/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SMAP page->flags helpers for cold-period counting and init tracking
 *
 * On aarch64 with CONFIG_NUMA_BALANCING=y, the LAST_CPUPID field
 * (LAST__PID_SHIFT + NR_CPUS_BITS bits; 16 with NR_CPUS=256, 20 with
 * the default arm64 NR_CPUS=4096) lives in page->flags just below the
 * zone/node fields, which occupy the top of the word. The kernel
 * writes all-1s into it when a page is freed, so both 0xF (cold) and
 * bit-0 (init) act as "never touched by SMAP" sentinels.  We reuse its
 * low 5 bits:
 *
 *   bit  [0]     (1 bit)   init flag  (1 = kernel-uninitialised, 0 = SMAP-seen)
 *   bits [1..4]  (4 bits)  consecutive-cold-period counter  (0..14, 0xF = sentinel)
 *
 * REQUIRES CONFIG_NUMA_BALANCING=y: with it disabled LAST_CPUPID_WIDTH
 * becomes 0 and LAST_CPUPID_PGSHIFT collapses to 0, so SMAP's bits
 * would land in the FLAGS region (PG_locked & friends) at the bottom
 * of the word. (Same if the field ever fails to fit and moves
 * out-of-line.)
 */

#ifndef _SMAP_PAGE_FLAGS_H
#define _SMAP_PAGE_FLAGS_H

#include <linux/compiler.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <asm/cmpxchg.h>

#if !defined(CONFIG_NUMA_BALANCING) || LAST_CPUPID_WIDTH == 0
#error "smap page flags require CONFIG_NUMA_BALANCING and LAST_CPUPID in page->flags"
#endif

/* Init flag: bit [0]. 1 = kernel-uninitialised, 0 = SMAP-seen. */
#define SMAP_INIT_FLAG_WIDTH 1
#define SMAP_INIT_FLAG_SHIFT LAST_CPUPID_PGSHIFT
#define SMAP_INIT_FLAG_MASK (1UL << SMAP_INIT_FLAG_SHIFT)

/*
 * Cold-period counter: bits [1..4]. 0xF is sentinel (kernel all-1s);
 * effective range 0..14, saturates at 14.
 */
#define SMAP_COLD_PERIOD_WIDTH 4
#define SMAP_COLD_PERIOD_MASK ((1UL << SMAP_COLD_PERIOD_WIDTH) - 1)
#define SMAP_COLD_PERIOD_SHIFT (LAST_CPUPID_PGSHIFT + SMAP_INIT_FLAG_WIDTH)
#define SMAP_COLD_FIELD_MASK (SMAP_COLD_PERIOD_MASK << SMAP_COLD_PERIOD_SHIFT)

/* Sentinel → 0 (0xF means kernel all-1s, not a real cold count). */
static inline u8 smap_page_cold_periods_get(struct page *page)
{
	u8 cnt = (page->flags >> SMAP_COLD_PERIOD_SHIFT) &
		 SMAP_COLD_PERIOD_MASK;
	return (cnt == SMAP_COLD_PERIOD_MASK) ? 0 : cnt;
}

/* Clear cold-period to 0; init flag preserved. */
static inline void smap_page_cold_periods_reset(struct page *page)
{
	unsigned long old, new;

	do {
		old = READ_ONCE(page->flags);
		new = old & ~SMAP_COLD_FIELD_MASK;
	} while (unlikely(cmpxchg(&page->flags, old, new) != old));
}

/*
 * Saturating increment: 0xF→1 (sentinel→first cold), 0..13→+1, 14→stay.
 * Saturates at 14 so cold_period_threshold (1..14) always triggers.
 */
static inline u8 smap_page_cold_periods_inc(struct page *page)
{
	unsigned long old, new;
	u8 cnt;

	do {
		old = READ_ONCE(page->flags);
		cnt = (old >> SMAP_COLD_PERIOD_SHIFT) & SMAP_COLD_PERIOD_MASK;
		if (cnt == SMAP_COLD_PERIOD_MASK)
			cnt = 1;
		else if (cnt < SMAP_COLD_PERIOD_MASK - 1)
			cnt++;
		new = old & ~SMAP_COLD_FIELD_MASK;
		new |= (unsigned long)cnt << SMAP_COLD_PERIOD_SHIFT;
	} while (unlikely(cmpxchg(&page->flags, old, new) != old));

	return cnt;
}

/* Init flag: true if page never seen by SMAP (bit 0 = 1). */
static inline bool smap_get_page_init(struct page *page)
{
	return test_bit(SMAP_INIT_FLAG_SHIFT, &page->flags);
}

/* Clear init flag (bit 0 → 0); no other bits touched. */
static inline void smap_clear_page_init(struct page *page)
{
	clear_bit(SMAP_INIT_FLAG_SHIFT, &page->flags);
}

#endif /* _SMAP_PAGE_FLAGS_H */
