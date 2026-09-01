/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SMAP page flags helpers — freq counter & sentinel in LAST_CPUPID field
 */

#ifndef _SRC_SMAP_PAGE_FLAGS_H
#define _SRC_SMAP_PAGE_FLAGS_H

#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/page-flags-layout.h>
#include <asm/cmpxchg.h>

/*
 * Guard: LAST_CPUPID must reside in page->flags.
 * REQUIRES CONFIG_NUMA_BALANCING=y: with it disabled LAST_CPUPID_WIDTH
 * becomes 0 and LAST_CPUPID_PGSHIFT collapses to 0, so SMAP's bits
 * would land in the FLAGS region (PG_locked & friends) at the bottom
 * of the word. (Same if the field ever fails to fit and moves
 * out-of-line, i.e. LAST_CPUPID_NOT_IN_PAGE_FLAGS.)
 */
#if !defined(CONFIG_NUMA_BALANCING) || LAST_CPUPID_WIDTH == 0
#error "smap page flags require CONFIG_NUMA_BALANCING and LAST_CPUPID in page->flags"
#endif

/* ---- Frequency field layout (relative to LAST_CPUPID_PGSHIFT) ----
 *
 * bit[0..7]  freq counter   8 bits, value 0..255
 * bit[8]     freq sentinel   1 bit,  1 = "invalid / pending-clear"
 * bit[9..19] reserved       11 bits (for future cumulative freq)
 *
 * Sentinel bit=1 means the freq counter is stale (e.g. page freed,
 * kernel wrote all-1s in LAST_CPUPID). get() returns 0; inc() resets
 * counter to 1 and clears sentinel.
 */

static inline bool smap_page_freq_try_get(struct page *page)
{
	if (!page)
		return false;
	return folio_try_get(page_folio(page));
}

#define SMAP_FREQ_VAL_SHIFT   LAST_CPUPID_PGSHIFT
#define SMAP_FREQ_VAL_WIDTH   8
#define SMAP_FREQ_VAL_MASK    ((1UL << SMAP_FREQ_VAL_WIDTH) - 1)
#define SMAP_FREQ_SENT_SHIFT  (SMAP_FREQ_VAL_SHIFT + SMAP_FREQ_VAL_WIDTH)
#define SMAP_FREQ_SENT_BIT    (1UL << SMAP_FREQ_SENT_SHIFT)
#define SMAP_FREQ_FIELD_MASK  (SMAP_FREQ_VAL_MASK << SMAP_FREQ_VAL_SHIFT)
#define SMAP_FREQ_MAX         ((1UL << SMAP_FREQ_VAL_WIDTH) - 1)

/* Read current freq value; returns 0 when sentinel is set. */
static inline u8 smap_page_freq_get(struct page *page)
{
	unsigned long flags;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return 0;

	flags = READ_ONCE(page->flags);
	if (flags & SMAP_FREQ_SENT_BIT)
		v = 0;
	else
		v = (u8)((flags >> SMAP_FREQ_VAL_SHIFT) & SMAP_FREQ_VAL_MASK);
	folio_put(page_folio(page));
	return v;
}

/* Set the sentinel bit to mark freq as stale. */
static inline void smap_page_freq_set_sentinel(struct page *page)
{
	unsigned long old, nv;

	if (!smap_page_freq_try_get(page))
		return;

	do {
		old = READ_ONCE(page->flags);
		nv = old | SMAP_FREQ_SENT_BIT;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
}

/* Atomically read freq and set sentinel; returns 0 when sentinel is already set. */
static inline u8 smap_page_freq_read_clear(struct page *page)
{
	unsigned long old, nv;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return 0;

	do {
		old = READ_ONCE(page->flags);
		if (old & SMAP_FREQ_SENT_BIT) {
			v = 0;
			nv = old; /* sentinel already set, no change */
		} else {
			v = (u8)((old >> SMAP_FREQ_VAL_SHIFT) & SMAP_FREQ_VAL_MASK);
			nv = old | SMAP_FREQ_SENT_BIT;
		}
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
	return v;
}

/* Increment freq by 1; resets to 1 when sentinel is set. */
static inline u8 smap_page_freq_inc(struct page *page)
{
	unsigned long old, nv;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return 0;

	do {
		old = READ_ONCE(page->flags);
		if (old & SMAP_FREQ_SENT_BIT) {
			v = 1;
			nv = old & ~(SMAP_FREQ_FIELD_MASK | SMAP_FREQ_SENT_BIT);
			nv |= 1UL << SMAP_FREQ_VAL_SHIFT;
		} else {
			v = (u8)((old >> SMAP_FREQ_VAL_SHIFT) & SMAP_FREQ_VAL_MASK);
			if (v < SMAP_FREQ_MAX)
				v++;
			nv = old & ~SMAP_FREQ_FIELD_MASK;
			nv |= (unsigned long)v << SMAP_FREQ_VAL_SHIFT;
		}
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
	return v;
}

/* Add val to freq; caps at SMAP_FREQ_MAX. */
static inline u8 smap_page_freq_add(struct page *page, u8 val)
{
	unsigned long old, nv;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return 0;

	do {
		old = READ_ONCE(page->flags);
		if (old & SMAP_FREQ_SENT_BIT) {
			v = min_t(u8, val, SMAP_FREQ_MAX);
			nv = old & ~(SMAP_FREQ_FIELD_MASK | SMAP_FREQ_SENT_BIT);
			nv |= (unsigned long)v << SMAP_FREQ_VAL_SHIFT;
		} else {
			u16 sum = (u16)((old >> SMAP_FREQ_VAL_SHIFT) & SMAP_FREQ_VAL_MASK) + val;
			v = (u8)min_t(u16, sum, SMAP_FREQ_MAX);
			nv = old & ~SMAP_FREQ_FIELD_MASK;
			nv |= (unsigned long)v << SMAP_FREQ_VAL_SHIFT;
		}
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
	return v;
}

/* ---- Page classification helpers ---- */

static inline bool is_file_or_shared_page(struct page *page)
{
	struct folio *folio = page_folio(page);

	return !folio_test_anon(folio) || folio_test_ksm(folio) ||
	       page_mapcount(page) > 1 || folio_test_swapcache(folio);
}

static inline bool is_shared_file_page(struct page *page)
{
	struct folio *folio = page_folio(page);

	return !folio_test_anon(folio) && page_mapcount(page) > 1;
}

#endif /* _SRC_SMAP_PAGE_FLAGS_H */
