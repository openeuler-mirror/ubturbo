/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP per-pid cold-page swap staging buffer and prototypes of
 *              the swap-out worker (cold anon pages -> swap).
 */

#ifndef _SRC_SMAP_SWAP_H
#define _SRC_SMAP_SWAP_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>

/* Per-pid staging budget: 128KB of folio pointers, see SMAP_SWAP_BUF_MAX. */
#define SMAP_SWAP_BUF_SUM (128 * 1024)
#define SMAP_SWAP_BUF_MAX (SMAP_SWAP_BUF_SUM / sizeof(struct folio *))

/*
 * Staging buffer of one pid: pages kept cold for longer than the configured
 * swap period are parked here as PFNs, each holding the folio reference
 * taken at stage time (removed after the isolate attempt) so the page
 * survives until the swap window; the swap workqueue worker validates each
 * pfn at drain time (pfn_to_online_page), isolates, and swaps out. The
 * buffer is allocated only for VM pids and further stages are dropped
 * while full.
 */
struct smap_swap_buf {
	spinlock_t lock;
	u32 cnt;
	unsigned long *pfns; /* SMAP_SWAP_BUF_MAX slots */
};

/* Per-worker-invocation cumulative statistics for the swap-out drain. */
struct smap_swap_stats {
	unsigned long staged;
	unsigned long lru_skip;
	unsigned long unevictable;
	unsigned long isolated;
	unsigned long reclaimed;
};

struct access_pid;

int smap_swap_init(void);
void smap_swap_exit(void);

struct smap_swap_buf *smap_swap_buf_create(void);
void smap_swap_buf_free(struct smap_swap_buf *buf);

/* swap disabled: release every pid's staged buffer (drop staging refs) */
void smap_swap_free_all(void);
void smap_swap_alloc_all(void);

/*
 * hugetlb swap capability: only set by the swap-config ioctl when
 * /proc/meminfo exposes HugePages_Swap (kernel supports hugetlb swap
 * out); default off. Without it hugetlb folios are never staged.
 */
void smap_swap_set_hugetlb(bool enable);
bool smap_swap_hugetlb_enabled(void);

/*
 * Stage one cold folio (takes a folio reference; pure buffer append).
 * Returns true when the folio was actually staged; false means the buffer
 * is full / the folio is not swappable and no reference was retained.
 */
bool smap_swap_stage(struct access_pid *ap, struct page *page);

/*
 * Trigger the swap-out worker once at scan-window end when the staging
 * buffer is non-empty (takes a work-reference on the owning slot).
 */
void smap_swap_notify(struct access_pid *ap);

void smap_swap_work_fn(struct work_struct *work);

#endif /* _SRC_SMAP_SWAP_H */
