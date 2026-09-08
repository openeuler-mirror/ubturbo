/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMAP_KERNEL_COMMON_H
#define SMAP_KERNEL_COMMON_H

#include <linux/mm.h>

#include "common.h"

#define MAX_NODE_NUM 32
#define DEVICE_BASE_MINOR 0
#define DEVICE_MINOR_COUNT 1
#define INVALID_PADDR 0

#define SMAP_MAX_LOCAL_NUMNODES SMAP_MAX_LOCAL_NUMA_NODES
#define SMAP_MAX_REMOTE_NUMNODES SMAP_MAX_REMOTE_NUMA_NODES
#define SMAP_MAX_NUMNODES SMAP_MAX_NUMA_NODES

#ifndef MIN
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif

struct access_pid;

typedef enum {
	NORMAL_MIGRATE,
	REMOTE_MIGRATE,
	MAX_MIGRATE_TYPE,
} migrate_type;

typedef struct {
	u64 pme;
} pagemap_entry_t;

struct remote_migrate_info {
	pid_t pid;
	u64 page_cnt;
	int remote_nid;
	unsigned int mig_cnt;
	u64 folios_len;
	struct folio **folios;
};

struct pagemapread {
	int pos, len; /* units: PM_ENTRY_BYTES, not bytes */
	migrate_type mig_type;
	struct remote_migrate_info mig_info;
	struct access_pid *ap;
};

struct freq_info {
	u64 hpa;
	u16 freq;
};

enum node_level { L1, L2, NR_LEVEL };

extern u32 g_pagesize_huge;

static inline u64 calc_huge_count(u64 range)
{
	return (range & ~PMD_MASK) == 0 ? range >> PMD_SHIFT
					: (range >> PMD_SHIFT) + 1;
}

static inline u64 calc_normal_count(u64 range)
{
	return (range & ~PAGE_MASK) == 0 ? range >> PAGE_SHIFT
					 : (range >> PAGE_SHIFT) + 1;
}

#endif /* SMAP_KERNEL_COMMON_H */
