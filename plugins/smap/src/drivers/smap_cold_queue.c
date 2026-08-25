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

#include "smap_cold_queue.h"

/* Per-node queues (local + remote) */
struct smap_cold_queue smap_cold_numa_queue[SMAP_MAX_NUMNODES];
EXPORT_SYMBOL(smap_cold_numa_queue);

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
 * Return: 0 on success, -1 if the per-node ring is full.
 */
int smap_cold_queue_enqueue(int nid, u64 pfn)
{
	struct smap_cold_queue *q;
	u32 slot;

	if (nid < 0 || nid >= SMAP_MAX_NUMNODES)
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
