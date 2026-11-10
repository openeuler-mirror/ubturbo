// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP per-pid cold-page swap staging plus the dedicated swap
 *              workqueue. Cold anon pages staged per pid are drained in
 *              batches and swapped out to the swap device through vmscan's
 *              reclaim_pages() (exported) plus kprobe-resolved isolate/lru
 *              helpers.
 */

#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/workqueue.h>

#include "check.h"
#include "access_pid.h"
#include "scan_main.h"
#include "smap_swap.h"
#include "smap_migrate_wrapper.h"

#define SWAP_WQ_NAME "smap_swap_workq"

#undef pr_fmt
#define pr_fmt(fmt) "smap_swap: " fmt

#define SMAP_SWAP_BATCH 512

/* System must keep at least this much free swap space, otherwise swap-out
 * is refused (checked before staging consumption on each window end).
 */
#define SMAP_SWAP_HEADROOM_BYTES (256ULL * 1024 * 1024)

static bool smap_swap_has_room(void)
{
	return (u64)get_nr_swap_pages() * PAGE_SIZE > SMAP_SWAP_HEADROOM_BYTES;
}

/* Release every staged folio (drop the staging references) and empty the
 * buffer; used when the system headroom forbids swap-out so pinned cold
 * pages do not pile up waiting forever.
 */
static void smap_swap_buf_purge(struct access_pid *ap)
{
	struct smap_swap_buf *buf = ap->swap_buf;
	unsigned int i;

	if (!buf)
		return;

	/* release the enqueue-time references held per staged pfn */
	spin_lock(&buf->lock);
	for (i = 0; i < buf->cnt; i++) {
		struct page *page = pfn_to_online_page(buf->pfns[i]);
		if (page)
			folio_put(page_folio(page));
	}
	buf->cnt = 0;
	spin_unlock(&buf->lock);
}

static struct workqueue_struct *smap_swap_wq;
/* hugetlb swap capability (HugePages_Swap in /proc/meminfo), delivered by
 * the swap-config ioctl; default off so hugetlb folios are never staged */
static bool hugetlb_swap_enabled = false;

void smap_swap_set_hugetlb(bool enable)
{
	WRITE_ONCE(hugetlb_swap_enabled, enable);
}

bool smap_swap_hugetlb_enabled(void)
{
	return READ_ONCE(hugetlb_swap_enabled);
}

int smap_swap_init(void)
{
	/* Dedicated unbounded wq for swap-out, same shape as the scan wq. */
	smap_swap_wq = alloc_workqueue(SWAP_WQ_NAME, WQ_UNBOUND | WQ_SYSFS,
				       WQ_MAX_THREADS);
	if (!smap_swap_wq) {
		pr_err("unable to init swap workqueue\n");
		return -ENOMEM;
	}
	return 0;
}

void smap_swap_exit(void)
{
	if (smap_swap_wq) {
		flush_workqueue(smap_swap_wq);
		destroy_workqueue(smap_swap_wq);
		smap_swap_wq = NULL;
	}
}

struct smap_swap_buf *smap_swap_buf_create(void)
{
	struct smap_swap_buf *buf = kzalloc(sizeof(*buf), GFP_KERNEL);

	if (!buf)
		return NULL;

	buf->pfns =
		kvzalloc(SMAP_SWAP_BUF_MAX * sizeof(*buf->pfns), GFP_KERNEL);
	if (!buf->pfns) {
		kfree(buf);
		return NULL;
	}
	spin_lock_init(&buf->lock);

	return buf;
}

void smap_swap_buf_free(struct smap_swap_buf *buf)
{
	unsigned int i;

	if (!buf)
		return;

	/* release the enqueue-time references held per staged pfn */
	spin_lock(&buf->lock);
	for (i = 0; i < buf->cnt; i++) {
		struct page *page = pfn_to_online_page(buf->pfns[i]);
		if (page)
			folio_put(page_folio(page));
	}
	buf->cnt = 0;
	spin_unlock(&buf->lock);

	kvfree(buf->pfns);
	kfree(buf);
}

/*
 * smap_swap_free_all - swap disabled: wait for in-flight workers, then
 * release the staged pins and free the per-pid buffers themselves.
 */
void smap_swap_free_all(void)
{
	int i;

	/* flush_workqueue() waits for the in-flight workers (the disable
	 * gate makes them self-purge and exit; no new work gets staged while
	 * the switch is off), so the buffers can be freed right after. */
	flush_workqueue(smap_swap_wq);

	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);

		if (!s)
			continue;
		/* smap_swap_buf_free() also drops the staged pins and empties
		 * the batch (same code as smap_swap_buf_purge), then frees
		 * the buffer itself. */
		if (s->ap) {
			smap_swap_buf_free(s->ap->swap_buf);
			s->ap->swap_buf = NULL;
		}
		ap_put_slot(s);
	}
}

/*
 * smap_swap_alloc_all - swap enabled: give every tracked VM pid its
 * staging buffer. Pids whose add-time gate skipped the allocation
 * (swap was off at pid-add time) come alive without a remove/re-add
 * cycle; non-VM pids and already-buffered pids are skipped.
 */
void smap_swap_alloc_all(void)
{
	int i;

	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);
		struct access_pid *ap;

		if (!s)
			continue;
		ap = s->ap;
		if (ap && ap->pid_type == SMAP_PID_VM && !ap->swap_buf)
			ap->swap_buf = smap_swap_buf_create();
		ap_put_slot(s);
	}
}

bool smap_swap_stage(struct access_pid *ap, struct page *page)
{
#ifndef KERNEL_OPENEULER
	/* reclaim_pages() is not resolvable on non-openEuler kernels and is
	 * compiled out of the worker; without it batch list folios would
	 * never be reclaimed nor put back, so nothing is ever staged and
	 * the worker self-drains (batch pops stay empty). */
	return false;
#endif
	struct folio *folio = page_folio(page);
	struct smap_swap_buf *buf = ap->swap_buf;

	if (!buf || !smap_swap_wq || !smap_swap_symbols_ready())
		return false;

	if (PageHuge(page)) {
		if (!smap_swap_hugetlb_enabled() || !PageHead(page))
			return false;
	} else {
		if (!folio_test_anon(folio) || page_mapcount(page) > 1 ||
		    PageTransHuge(page))
			return false;
	}

	spin_lock(&buf->lock);
	if (buf->cnt >= SMAP_SWAP_BUF_MAX) {
		spin_unlock(&buf->lock);
		return false;
	}
	if (!folio_try_get(folio)) {
		spin_unlock(&buf->lock);
		return false;
	}

	buf->pfns[buf->cnt++] = page_to_pfn(page);
	spin_unlock(&buf->lock);
	return true;
}

/*
 * smap_swap_notify - trigger the swap-out worker for staged pages.
 * Called once at the end of each pid scan window; a no-op when the buffer
 * is empty. Takes a work-reference on the owning slot (mirroring the scan
 * work) so the worker can safely access it even when the pid is being
 * removed concurrently.
 */
void smap_swap_notify(struct access_pid *ap)
{
	struct smap_swap_buf *buf = ap->swap_buf;
	bool has_pages;

	if (!buf || !smap_swap_wq || !smap_swap_symbols_ready() || !ap->slot)
		return;

	spin_lock(&buf->lock);
	has_pages = buf->cnt > 0;
	spin_unlock(&buf->lock);
	pr_info("notify pid %d has_pages %d\n", ap->pid, has_pages);
	if (!has_pages)
		return;

	/* Pre-swap headroom check: with <= 256MB of free swap the window's
	 * staged pages are released instead of queued for swap-out; they
	 * re-stage (and re-queue) on a later window when the system swaps
	 * back in.
	 */
	if (!smap_swap_has_room()) {
		pr_warn_ratelimited(
			"pid[%d] swap headroom <= %lluMB, skip swap-out this window\n",
			ap->pid, SMAP_SWAP_HEADROOM_BYTES >> 20);
		smap_swap_buf_purge(ap);
		return;
	}

	refcount_inc(&ap->slot->refs);
	/* The work function drops this work-reference on its only actual
	 * run; when the worker is still queued (queue_work() returns
	 * false, no extra run is scheduled) the just-taken reference has
	 * no consumer and must be returned here, otherwise the slot can
	 * never be reclaimed (refs never reach 0).
	 */
	if (!queue_work(smap_swap_wq, &ap->swap_work))
		ap_put_slot(ap->slot);
}

void smap_swap_work_fn(struct work_struct *work)
{
	struct access_pid *ap =
		container_of(work, struct access_pid, swap_work);
	struct smap_swap_buf *buf = ap->swap_buf;
	unsigned long *batch;
	unsigned int n, i;

	if (!buf || !smap_swap_symbols_ready())
		goto out;

	/* swap switched off between notify and execution: stop everything. */
	if (!get_swap_enable()) {
		/*
		 * swap switched off between notify and execution: release the
		 * stage pins (the ioctl path already purge_all()ed, the second
		 * purge is a no-op) and stop everything.
		 */
		smap_swap_buf_purge(ap);
		goto out;
	}

	/* guard against the headroom shrinking below the floor between the
	 * window-end check (notify) and the actual swap-out.
	 */
	if (!smap_swap_has_room()) {
		pr_warn_ratelimited(
			"pid[%d] swap headroom shrank below the floor, skip swap-out\n",
			ap->pid);
		smap_swap_buf_purge(ap);
		goto out;
	}

	batch = kmalloc_array(SMAP_SWAP_BATCH, sizeof(*batch), GFP_KERNEL);
	if (!batch) {
		/*
		 * kmalloc failure happens under memory pressure: holding the
		 * stage pins while the system is pressed makes the pressure
		 * worse (the pinned folios are still cold). Release the
		 * buffer pins so the folios go back to LRU and become
		 * reclaimable by kswapd; accumulated cold duration is kept
		 * in page flags, so they re-stage once memory frees up.
		 */
		pr_err_ratelimited(
			"pid[%d] no memory for swap batch, releasing staged pages\n",
			ap->pid);
		smap_swap_buf_purge(ap);
		goto out;
	}

	struct smap_swap_stats stats = {};

	while (1) {
		LIST_HEAD(page_list);
		unsigned int nr_lru_skip = 0, nr_unevic = 0, nr_isolated = 0;

		spin_lock(&buf->lock);
		n = min_t(u32, buf->cnt, SMAP_SWAP_BATCH);
		if (n) {
			memcpy(batch, buf->pfns, n * sizeof(*batch));
			memmove(&buf->pfns[0], &buf->pfns[n],
				(buf->cnt - n) * sizeof(*buf->pfns));
			buf->cnt -= n;
		}
		spin_unlock(&buf->lock);
		if (!n)
			break;

		for (i = 0; i < n; i++) {
			struct page *page = pfn_to_online_page(batch[i]);
			struct folio *folio;

			/*
			 * pfn_to_online_page() cannot fail while the enqueue-time
			 * pin is held (the reference blocks section offlining
			 * and buddy free, so the pfn still resolves to OUR
			 * folio); a NULL would mean the invariant broke and the
			 * queued pin cannot even be returned — warn loudly.
			 */
			if (unlikely(!page)) {
				pr_warn_ratelimited(
					"pid[%d] staged pfn %lx not online (stage-pin invariant broken!)\n",
					ap->pid, batch[i]);
				continue;
			}
			folio = page_folio(page);

			/* isolate_folio_to_list() requires a caller-held
			 * reference: one is already held since enqueue time
			 * (the stage pin). Clear the referenced flag before
			 * isolation so later reclaim cycles (this folio may
			 * go back to LRU) do not see stale young state. */
			/*
			 * PR387 drain parity: skip pages not (yet) on the LRU
			 * list (freshly faulted ones can sit on a per-cpu
			 * pagevec; others may be isolated by another consumer):
			 * without membership they cannot be swapped, so release
			 * the stage pin and move on.
			 */
			if (!folio_test_hugetlb(folio) &&
			    !folio_test_lru(folio)) {
				folio_put(folio);
				nr_lru_skip++;
				continue;
			}
			/* unevictable anon pages can never be reclaimed here;
			 * short-circuit before the isolation work instead of
			 * doing an isolate + putback round-trip afterwards */
			if (!folio_test_hugetlb(folio) &&
			    folio_test_unevictable(folio)) {
				folio_put(folio);
				nr_unevic++;
				continue;
			}
			if (!folio_test_hugetlb(folio))
				folio_clear_referenced(folio);

			/* isolate_folio_to_list() returns false when another
			 * consumer raced for this folio and it never joins
			 * the batch list — count only real isolates; either
			 * way the stage pin below is the only reference we
			 * hold, so release it unconditionally. */
			if (smap_swap_isolate_folio_to_list(folio, &page_list))
				nr_isolated++;
			folio_put(folio);
		}

		/*
		 * ignore_references=true: pages reaching the swap worker have
		 * already been verified cold for the configured period during
		 * scanning, so skip the shrink_folio_list() reference re-check
		 * (folio_check_references()) and reclaim regardless of young
		 * bits picked up in the staging-to-worker window. This keeps
		 * the per-batch reclaimed count close to the batch size.
		 */
		unsigned int nr_reclaimed = 0;

#ifdef KERNEL_OPENEULER
		/* reclaim_pages() internals put back every un-reclaimed
		 * folio (releasing the isolate references) and return with
		 * the list empty — no residual handling on this side. */
		nr_reclaimed = reclaim_pages(&page_list, true);
#endif
		stats.staged += n;
		stats.lru_skip += nr_lru_skip;
		stats.unevictable += nr_unevic;
		stats.isolated += nr_isolated;
		stats.reclaimed += nr_reclaimed;
	}

	pr_debug(
		"pid[%d] swap worker done: staged %lu, lru_skip %lu, unevictable %lu, isolated %lu, reclaimed %lu\n",
		ap->pid, stats.staged, stats.lru_skip, stats.unevictable,
		stats.isolated, stats.reclaimed);
	kfree(batch);
out:
	/* drop the work-reference taken by smap_swap_notify */
	if (ap->slot)
		ap_put_slot(ap->slot);
}
