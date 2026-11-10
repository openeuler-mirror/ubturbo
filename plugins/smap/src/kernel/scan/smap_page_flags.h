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

/*
 * Frequency-layout counters in the LAST_CPUPID field:
 *
 *   counter               bits        width  purpose
 *   -------------------------------------------------------------------
 *   acc counter           bit[9..13]  5      cumulative access count
 *   L3 cold-period cnt    bit[14..19] 6      cold duration in seconds
 *
 * They together occupy bit[9..19], so SMAP needs all 20 bits of the
 * field (NR_CPUS_BITS >= 12 gives the full 20-bit field). With a
 * narrower field they would spill into the spare (or KASAN/MTE tag)
 * bits above LAST_CPUPID.
 */
#if LAST_CPUPID_WIDTH < 20
#error "smap acc + cold-period counters require all 20 bits of the LAST_CPUPID field"
#endif

/* ---- Frequency field layout (relative to LAST_CPUPID_PGSHIFT) ----
 *
 * bit[0..7]  freq counter   8 bits, value 0..255
 * bit[8]     freq sentinel   1 bit,  1 = "invalid / pending-clear"
 * bit[9..13] acc counter    5 bits, cumulative access count 0..SMAP_ACC_CNT_MAX
 *            (all-1s 0x1F = invalid marker: page free resets the whole
 *            LAST_CPUPID field to all-1s, the same write the freq sentinel
 *            relies on)
 * bit[14..19] cold period   6 bits, L3 cold duration in SECONDS (one add per
 *            scan cycle, value ceil-rounded), 0..MAX_L3_COLD_THRESHOLD.
 *            all-1s 0x3F = invalid marker (page free resets the whole
 *            LAST_CPUPID field to all-1s, the same write the freq sentinel
 *            relies on): smap_page_cold_period_add() restarts from a fresh
 *            value when it reads the marker, so a freed-then-reused page
 *            starts counting over.
 *
 * Sentinel bit=1 means the freq counter is stale (e.g. page freed,
 * kernel wrote all-1s in LAST_CPUPID). get() returns 0; inc() resets
 * counter to 1 and clears sentinel.
 *
 * The acc counter accumulates one inc per scan cycle for young non
 * white-list pages of NORMAL_SCAN pids; every SMAP_ACC_DECAY_CYCLE scan
 * rounds a decay round halves all acc counters (hot pages (N+1)>>1 via
 * smap_page_acc_inc, cold pages N>>1 via smap_page_acc_decay), keeping
 * long-term hot pages distinguishable from transiently hot ones and
 * letting cooled pages fall back to 0 so they can be demoted later. It
 * is stored at 5-bit width and reported to user space unchanged in the
 * 6-bit actc prior field (prior == acc, no quantization shift).
 */

static inline bool smap_page_freq_try_get(struct page *page)
{
	if (!page)
		return false;
	return folio_try_get(page_folio(page));
}

#define SMAP_FREQ_VAL_SHIFT LAST_CPUPID_PGSHIFT
#define SMAP_FREQ_VAL_WIDTH 8
#define SMAP_FREQ_VAL_MASK ((1UL << SMAP_FREQ_VAL_WIDTH) - 1)
#define SMAP_FREQ_SENT_SHIFT (SMAP_FREQ_VAL_SHIFT + SMAP_FREQ_VAL_WIDTH)
#define SMAP_FREQ_SENT_BIT (1UL << SMAP_FREQ_SENT_SHIFT)
#define SMAP_FREQ_FIELD_MASK (SMAP_FREQ_VAL_MASK << SMAP_FREQ_VAL_SHIFT)
#define SMAP_FREQ_MAX ((1UL << SMAP_FREQ_VAL_WIDTH) - 1)
#define SMAP_COLD_PERIOD_WIDTH 6
#define SMAP_COLD_PERIOD_MASK ((1UL << SMAP_COLD_PERIOD_WIDTH) - 1)
#define SMAP_COLD_PERIOD_SHIFT (SMAP_ACC_CNT_SHIFT + SMAP_ACC_CNT_WIDTH)
#define SMAP_COLD_FIELD_MASK (SMAP_COLD_PERIOD_MASK << SMAP_COLD_PERIOD_SHIFT)

#define MIN_L3_COLD_THRESHOLD 1UL
#define MAX_L3_COLD_THRESHOLD (SMAP_COLD_PERIOD_MASK - 1)
#define DEFAULT_L3_COLD_THRESHOLD 30

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
			v = (u8)((old >> SMAP_FREQ_VAL_SHIFT) &
				 SMAP_FREQ_VAL_MASK);
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
			v = (u8)((old >> SMAP_FREQ_VAL_SHIFT) &
				 SMAP_FREQ_VAL_MASK);
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
			u16 sum = (u16)((old >> SMAP_FREQ_VAL_SHIFT) &
					SMAP_FREQ_VAL_MASK) +
				  val;
			v = (u8)min_t(u16, sum, SMAP_FREQ_MAX);
			nv = old & ~SMAP_FREQ_FIELD_MASK;
			nv |= (unsigned long)v << SMAP_FREQ_VAL_SHIFT;
		}
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
	return v;
}

/* ---- Cumulative access count (acc) ----
 *
 * Lives in bit[9..13] of the LAST_CPUPID field. Unlike the freq counter
 * it is NOT read-cleared on report; it keeps accumulating across scan
 * cycles and is only aged by the periodic decay cycle. Stored at 5-bit
 * width, so the reported 6-bit actc prior is the acc value itself.
 */

#define SMAP_ACC_CNT_SHIFT (SMAP_FREQ_SENT_SHIFT + 1)
#define SMAP_ACC_CNT_WIDTH 5
#define SMAP_ACC_CNT_MASK ((1UL << SMAP_ACC_CNT_WIDTH) - 1)
#define SMAP_ACC_CNT_FIELD_MASK (SMAP_ACC_CNT_MASK << SMAP_ACC_CNT_SHIFT)
#define SMAP_ACC_CNT_MAX (SMAP_ACC_CNT_MASK - 1) /* 30 */
#define SMAP_ACC_CNT_INVALID SMAP_ACC_CNT_MASK /* 0x1F, freed-page marker */
#define SMAP_ACC_DECAY_CYCLE 4 /* decay cadence, in scan rounds */

/* ---- Decay cadence ----
 *
 * acc counters are halved every SMAP_ACC_DECAY_CYCLE scan rounds,
 * decoupled from the 5-bit saturating counter width.
 *
 * Decay halves instead of resetting, so a continuously-hot page's value
 * spans about 2*SMAP_ACC_DECAY_CYCLE: one full cycle of increments piled
 * on top of the half-history kept by the previous halving. Keeping that
 * span inside the 5-bit range (SMAP_ACC_CNT_MAX = 30) requires D <= 15.
 *
 * D = 4 keeps the cumulative window short so acc tracks recent access
 * tightly; the freed-page restart value DECAY_CYCLE/2 = 2 starts
 * transient pages just above the cold threshold.
 */

/* Read current acc value (non-destructive); returns 0 when invalid. */
static inline u8 smap_page_acc_get(struct page *page)
{
	unsigned long flags;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return 0;

	flags = READ_ONCE(page->flags);
	v = (u8)((flags >> SMAP_ACC_CNT_SHIFT) & SMAP_ACC_CNT_MASK);
	if (v == SMAP_ACC_CNT_INVALID)
		v = 0;
	else
		v = min_t(u8, v, SMAP_ACC_CNT_MAX);
	folio_put(page_folio(page));
	return v;
}

/*
 * smap_page_acc_inc - cumulative access count +1
 * @page: target page
 * @decay: current scan cycle is a decay cycle
 *
 * decay: stored N -> (N+1)>>1 (count this round's access first, then
 * halve the history, so a long-idle page can decay back to 0);
 * otherwise N -> N+1. Saturates at SMAP_ACC_CNT_MAX; an invalid marker
 * (freed page) restarts at SMAP_ACC_DECAY_CYCLE/2 so that transient
 * briefly-accessed pages (temporary allocations) start above the cold
 * threshold and are freed before they can be falsely demoted.
 * Never touches the freq counter / sentinel bits.
 */
static inline void smap_page_acc_inc(struct page *page, bool decay)
{
	unsigned long old, nv;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return;

	do {
		old = READ_ONCE(page->flags);
		v = (u8)((old >> SMAP_ACC_CNT_SHIFT) & SMAP_ACC_CNT_MASK);
		if (v == SMAP_ACC_CNT_INVALID) {
			v = SMAP_ACC_DECAY_CYCLE / 2;
		} else {
			if (decay)
				v = (u8)((v + 1) >> 1);
			else if (v >= SMAP_ACC_CNT_MAX)
				break; /* saturated, leave as is */
			else
				v++;
		}
		nv = old & ~SMAP_ACC_CNT_FIELD_MASK;
		nv |= (unsigned long)v << SMAP_ACC_CNT_SHIFT;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
}

/*
 * smap_page_acc_decay - halve the cumulative access count of a cold page
 * @page: target page (not accessed in this scan cycle)
 *
 * Decay cycle: stored N -> N>>1. Cold pages are never passed to
 * smap_page_acc_inc, so without this helper a once-hot page would keep
 * its historical acc forever, squatting on a local slot and never
 * becoming demotable. An invalid marker (freed page) is left as is.
 * Never touches the freq counter / sentinel bits.
 */
static inline void smap_page_acc_decay(struct page *page)
{
	unsigned long old, nv;
	u8 v;

	if (!smap_page_freq_try_get(page))
		return;

	do {
		old = READ_ONCE(page->flags);
		v = (u8)((old >> SMAP_ACC_CNT_SHIFT) & SMAP_ACC_CNT_MASK);
		if (v == SMAP_ACC_CNT_INVALID || v == 0)
			break; /* freed-page marker, or already zero */
		v >>= 1;
		nv = old & ~SMAP_ACC_CNT_FIELD_MASK;
		nv |= (unsigned long)v << SMAP_ACC_CNT_SHIFT;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
}

/*
 * migrate_folio_actc - inherit the cumulative access count across migration.
 * @folio_from: source folio being migrated out
 * @folio_to:   newly allocated destination folio
 *
 * A migrated page lands on a freshly allocated folio whose acc is 0, so the
 * long-term heat is lost and the page may be misjudged cold again (ping-pong
 * demote/promote). Copy the source acc into the destination so the history
 * travels with the page, in both demote and promote directions.
 *
 * acc only exists for normal 4K pages of NORMAL_SCAN pids; hugetlb folios
 * carry no acc (VM prior comes from the allocation-order mapping) and are
 * skipped. A NULL destination (allocation failure) is ignored. The source is
 * left untouched: it keeps reporting its own acc until it is freed, and the
 * freed-page marker takes care of reuse.
 */
static inline void migrate_folio_actc(struct folio *folio_from,
				      struct folio *folio_to)
{
	struct page *page;
	unsigned long old, nv;
	u8 acc;

	if (!folio_from || !folio_to)
		return;
	if (folio_test_hugetlb(folio_from) || folio_test_hugetlb(folio_to))
		return;

	acc = smap_page_acc_get(&folio_from->page);
	if (acc == 0)
		return;

	page = &folio_to->page;
	if (!smap_page_freq_try_get(page))
		return;

	do {
		old = READ_ONCE(page->flags);
		nv = old & ~SMAP_ACC_CNT_FIELD_MASK;
		nv |= (unsigned long)acc << SMAP_ACC_CNT_SHIFT;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
	folio_put(page_folio(page));
}

/* Clear cold-period to 0. */
static inline void smap_page_cold_periods_reset(struct page *page)
{
	unsigned long old, nv;

	do {
		old = READ_ONCE(page->flags);
		nv = old & ~SMAP_COLD_FIELD_MASK;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));
}

/* Saturates at MAX_L3_COLD_THRESHOLD. */
static inline u8 smap_page_cold_period_add(struct page *page, unsigned long val)
{
	unsigned long old, nv;
	unsigned long period;

	if (val > MAX_L3_COLD_THRESHOLD)
		val = MAX_L3_COLD_THRESHOLD;

	do {
		old = READ_ONCE(page->flags);
		period = (old >> SMAP_COLD_PERIOD_SHIFT) &
			 SMAP_COLD_PERIOD_MASK;
		if (period == SMAP_COLD_PERIOD_MASK)
			period = val;
		else if (period + val > MAX_L3_COLD_THRESHOLD)
			period = MAX_L3_COLD_THRESHOLD;
		else
			period += val;
		nv = old & ~SMAP_COLD_FIELD_MASK;
		nv |= period << SMAP_COLD_PERIOD_SHIFT;
	} while (unlikely(cmpxchg(&page->flags, old, nv) != old));

	return (u8)period;
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
