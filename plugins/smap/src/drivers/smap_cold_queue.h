/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: Per-node PFN queue for cold page swap-out
 *
 * Lock-free MPSC ring buffer.  During the scan phase (producers only),
 * enqueue claims a slot via atomic_fetch_add(&tail) and commits via
 * atomic_inc(&count) after the PFN is written.  During the drain phase
 * (single consumer, after all producers have quiesced), the consumer
 * reads count and walks head..head+count.  No global lock — per-node
 * tail and count are the only shared state.
 *
 * IMPORTANT: The consumer MUST NOT run concurrently with producers.
 * In the current architecture this is guaranteed by userspace: the
 * drain ioctl (SMAP_MIG_DRAIN_COLD_QUEUE) is issued only after all
 * scan ioctls have returned.
 */

#ifndef _SRC_SMAP_COLD_QUEUE_H
#define _SRC_SMAP_COLD_QUEUE_H

#include <linux/types.h>
#include <linux/atomic.h>

#define SMAP_COLD_QUEUE_MAX_SIZE (262144 * 16) /* 2^22 */
#define SMAP_COLD_QUEUE_MAX_MASK (SMAP_COLD_QUEUE_MAX_SIZE - 1)

#ifndef SMAP_MAX_NUMNODES
#define SMAP_MAX_NUMNODES 22
#endif

struct smap_cold_queue {
	u64 *pfn;
	unsigned int head;	/* consumer only — no concurrency with drain */
	atomic_t tail;		/* producers: fetch_add to claim a slot */
	atomic_t count;		/* committed items: producers inc, consumer dec */
};

/*
 * One queue per NUMA node (local + remote).
 * Defined in smap_cold_queue.c (drivers module).
 */
extern struct smap_cold_queue smap_cold_numa_queue[SMAP_MAX_NUMNODES];

void smap_cold_queue_free(void);
int smap_cold_queue_init(void);

/*
 * Enqueue a PFN for swap-out. Returns 0 on success, -1 if queue is full.
 * Must be called only during the scan phase (producers only).
 */
int smap_cold_queue_enqueue(int nid, u64 pfn);

#endif /* _SRC_SMAP_COLD_QUEUE_H */
