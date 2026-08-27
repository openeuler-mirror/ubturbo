// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: Per-node PFN queue for cold page swap-out
 *
 * Lock-free MPSC ring buffer.  The scan path (accessed_bit.c) enqueues
 * cold remote PFNs under the owning node via atomic_fetch_add on tail;
 * the drain function (mig_init.c) iterates the array and reclaims pages.
 *
 * Correctness constraint: producers (scan) and consumer (drain) must not
 * run concurrently.  See smap_cold_queue.h for details.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "smap_cold_queue.h"
#include "accessed_bit.h"

#undef pr_fmt
#define pr_fmt(fmt) "smap_cold_queue: " fmt

/* Per-node queues (local + remote) */
struct smap_cold_queue smap_cold_numa_queue[SMAP_MAX_NUMNODES];
EXPORT_SYMBOL(smap_cold_numa_queue);

#define SWAP_DISABLE 0
#define SWAP_ENABLE  1

/*
 * Runtime kill-switch for the cold-page swap-out path, exposed as a sysfs
 * attribute at /sys/kernel/smap/swap_enable.  1 = enabled (default), 0 =
 * disabled.  When disabled the scan path stops enqueueing cold PFNs and the
 * drain path refuses to reclaim, so no page is swapped out and cold pages
 * fall back to normal bitmap-based migration.  No lock needed: a single
 * aligned store/load is atomic on all supported architectures.
 */
unsigned int swap_out_enable = SWAP_ENABLE;
EXPORT_SYMBOL(swap_out_enable);

static ssize_t swap_enable_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(swap_out_enable));
}

static ssize_t swap_enable_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;
	if (val != SWAP_DISABLE && val != SWAP_ENABLE) {
		pr_err("swap_enable %u invalid, must be %d or %d\n", val,
		       SWAP_DISABLE, SWAP_ENABLE);
		return -EINVAL;
	}
	WRITE_ONCE(swap_out_enable, val);
	pr_info("swap-out %s\n", val ? "enabled" : "disabled");
	return count;
}

static struct kobj_attribute swap_enable_attr = __ATTR_RW(swap_enable);

/*
 * Attach the swap_enable attribute to the shared /sys/kernel/smap kobject
 * (created by smap_cold_threshold_sysfs_init). Must run after that. Failure
 * is non-fatal: swap-out keeps its default enabled state, only runtime
 * toggling is unavailable.
 */
int smap_swap_enable_sysfs_init(void)
{
	struct kobject *kobj = smap_get_kobject();

	if (!kobj) {
		pr_err("smap kobject not ready, skip swap_enable\n");
		return -ENXIO;
	}
	return sysfs_create_file(kobj, &swap_enable_attr.attr);
}

void smap_swap_enable_sysfs_exit(void)
{
	struct kobject *kobj = smap_get_kobject();

	if (kobj)
		sysfs_remove_file(kobj, &swap_enable_attr.attr);
}

int smap_cold_queue_init(void)
{
	int i;

	for (i = 0; i < SMAP_MAX_NUMNODES; i++) {
		smap_cold_numa_queue[i].pfn =
			vmalloc(SMAP_COLD_QUEUE_MAX_SIZE * sizeof(u64));
		if (!smap_cold_numa_queue[i].pfn)
			return -1;
		smap_cold_numa_queue[i].head = 0;
		atomic_set(&smap_cold_numa_queue[i].tail, 0);
		atomic_set(&smap_cold_numa_queue[i].count, 0);
	}
	return 0;
}

void smap_cold_queue_free(void)
{
	int i;

	for (i = 0; i < SMAP_MAX_NUMNODES; i++) {
		vfree(smap_cold_numa_queue[i].pfn);
		smap_cold_numa_queue[i].pfn = NULL;
		smap_cold_numa_queue[i].head = 0;
		atomic_set(&smap_cold_numa_queue[i].tail, 0);
		atomic_set(&smap_cold_numa_queue[i].count, 0);
	}
}

/*
 * smap_cold_queue_enqueue - Enqueue a cold PFN for swap-out (lock-free).
 * @nid: NUMA node id
 * @pfn:  page frame number
 *
 * Uses atomic_fetch_add on tail to claim a ring slot without a spinlock.
 * The consumer must not run concurrently with this function (guaranteed
 * by userspace sequencing: drain ioctl only arrives after scan is
 * disabled), so no memory barrier is needed between the PFN store and
 * the atomic_inc(&count) commit.
 *
 * Return: 0 on success, -1 if the per-node ring is full or swap-out is
 * disabled via swap_enable; in both cases the caller records the page
 * in the bitmap (normal migration path) instead of swapping it out.
 */
int smap_cold_queue_enqueue(int nid, u64 pfn)
{
	struct smap_cold_queue *q;
	u32 slot;

	if (nid < 0 || nid >= SMAP_MAX_NUMNODES)
		return -1;

	/*
	 * Swap-out disabled via /sys/kernel/smap/swap_enable: report full so
	 * the caller records the page in the bitmap (normal migration path).
	 */
	if (!READ_ONCE(swap_out_enable))
		return -1;

	q = &smap_cold_numa_queue[nid];

	/*
	 * Fullness check: use tail - head (claimed slots) rather than count
	 * (committed slots) because a claimed slot may not yet be committed
	 * (producer preempted between claim and write).  head is consumer-only
	 * and stable during the scan phase.
	 */
	if ((u32)(atomic_read(&q->tail) - q->head) >=
	    SMAP_COLD_QUEUE_MAX_SIZE)
		return -1;

	/* Claim a slot. */
	slot = (u32)(atomic_fetch_add(1, &q->tail) &
		     SMAP_COLD_QUEUE_MAX_MASK);

	/*
	 * Write the PFN and commit.  No barrier needed between
	 * the store and atomic_inc(): (a) the drain consumer runs
	 * only after all producers have quiesced, so cache-coherence
	 * propagation has long since completed by the time any
	 * consumer reads this slot; (b) on ARM64 atomic_inc()
	 * already carries store-release semantics (STLR).
	 */
	q->pfn[slot] = pfn;
	atomic_inc(&q->count);

	return 0;
}
EXPORT_SYMBOL(smap_cold_queue_enqueue);
