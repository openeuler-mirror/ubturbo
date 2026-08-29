/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap access pid module
 */

#ifndef _SRC_ACCESS_PID_H
#define _SRC_ACCESS_PID_H

#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/refcount.h>
#include <linux/mutex.h>

#include "check.h"
#include "access_ioctl.h"
#include "bus.h"
#include "drv_common.h"

#define MAX_PATH_LENGTH 64
#define AP_PROCFS_DIR_LEN 32
#define SEC_TO_MS 1000
#define NON_EXIST_PID (-1)
#define DUPLICATE_PID (-2) /* completely duplicate, skip processing */
extern int nr_local_numa;

/* Upper bound of tracked pids, inherits MAX_4K_PROCESSES_CNT. */
#ifndef AP_MAX_SLOTS
#define AP_MAX_SLOTS MAX_4K_PROCESSES_CNT
#endif

enum ap_state {
	AP_STATE_WALK = (1UL << 0),
	AP_STATE_READ = (1UL << 1),
	AP_STATE_FREQ = (1UL << 2),
	AP_STATE_MIG = (1UL << 3),
};

/*
 * Slot lifecycle: FREE -> RESERVED -> INUSE -> REMOVING -> FREE.
 * - FREE:     idle, can be CAS-claimed by ap_slot_add.
 * - RESERVED: ap_slot_add in-progress, invisible to readers.
 * - INUSE:    visible, the only state external readers accept.
 * - REMOVING: ap_slot_remove set, waiting for refs to drop to 0 to reclaim.
 */
enum ap_slot_state {
	AP_SLOT_FREE = 0,
	AP_SLOT_RESERVED = 1,
	AP_SLOT_INUSE = 2,
	AP_SLOT_REMOVING = 3,
};

/*
 * Per-pid slot: replaces the ap_data.list node. Carries the access_pid plus a
 * refcount and a fine-grained rw_semaphore (ap_lock) that protects the bitmaps
 * (paddr_bm/white_list_bm/bm_len/info). Readers take a reference via
 * ap_get_slot()/ap_collect_refs() and drop it with ap_put_slot(); the last
 * releaser reclaims the access_pid. ap_lock is a rw_semaphore (not spinlock)
 * because the scan path walks page tables and may sleep, and read-side
 * concurrency (scan + migration) is desired.
 */
struct ap_slot {
	atomic_t state; /* FREE/RESERVED/INUSE/REMOVING */
	pid_t pid;
	struct access_pid *ap;
	refcount_t refs;
	struct rw_semaphore ap_lock;
} ____cacheline_aligned_in_smp;

struct access_pid_struct {
	struct ap_slot slots[AP_MAX_SLOTS]; /* replaces ap_data.list */
	spinlock_t state_lock; /* protects global scan-phase state_flag */
	unsigned long state_flag;
};
extern struct access_pid_struct ap_data;

struct va_segment {
	u64 base_gfn;
	u64 start;
	u64 end;
	u64 hugepages;
};

struct vm_mapping_info {
	u8 nr_segs;
	u32 vm_size;
	struct va_segment segs[MAX_NODE_NUM];
	u8 *priors;
};

typedef enum {
	NORMAL_MIGRATE,
	REMOTE_MIGRATE,
	MAX_MIGRATE_TYPE,
} migrate_type;

struct access_pid {
	pid_t pid;
	smap_pid_type pid_type;
	u32 numa_nodes;
	scan_type type;
	u32 scan_time;
	u32 ntimes;
	u32 cur_times;
	struct delayed_work scan_work;
	struct completion work_done;
	u32 scan_count[SMAP_MAX_NUMNODES];
	size_t page_num[SMAP_MAX_NUMNODES];
	size_t bm_len[SMAP_MAX_NUMNODES];
	unsigned long *paddr_bm[SMAP_MAX_NUMNODES];
	unsigned long *white_list_bm[SMAP_MAX_NUMNODES];
	struct list_head
		node; /* used only for staging lists during add, not for ap_data */
	struct vm_mapping_info info;
	ktime_t last_scan_end;
	unsigned long last_scan_delay_ms;
	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_freq;
	struct ap_slot
		*slot; /* back-pointer to owning slot, set by ap_slot_add */
};

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

extern struct list_head ham_pid_list;
extern struct list_head statistic_pid_list;
extern spinlock_t ham_lock;
extern struct rw_semaphore statistic_lock;

void print_access_ham_pid_list(void);
void print_access_statistic_pid_list(void);
void access_remove_ham_pid(int len, struct access_remove_pid_payload *payload);
void access_remove_statistic_pid(int len,
				 struct access_remove_pid_payload *payload);
void destroy_access_pid(struct access_pid *elem);
int init_access_pid(struct access_add_pid_payload *payload,
		    struct access_pid **elem);
void print_access_pid_list(void);
int access_add_ham_pid(int len, struct access_add_pid_payload *payload);
int access_add_statistic_pid(int len, struct access_add_pid_payload *payload,
			     int page_size);
int access_add_pid(int len, struct access_add_pid_payload *payload);
void access_remove_pid(int len, struct access_remove_pid_payload *payload);
void access_remove_all_pid(void);
void change_ap_type(pid_t pid);
void clean_last_ap_data(struct access_pid *ap);
int convert_pos_to_paddr_sorted(pid_t pid, int nid, u64 len, u64 *addr);
int init_ap_bm_white_list(int node_len, u64 *node_page_count,
			  struct access_pid *ap);
int init_vm_mapping(struct vm_mapping_info *info);
int access_walk_pagemap_prepare(struct access_pid *ap);

/*
 * Slot-table + reference-count API (replaces global ap_data.lock).
 * Contract: every ap_get_slot/ap_get_ref/ap_collect_refs success must be paired
 * with ap_put_slot/ap_release_refs on ALL return paths (incl. early-return).
 */
int ap_slot_add(struct access_pid *ap);
void ap_slot_remove(pid_t pid);
struct ap_slot *ap_get_slot(pid_t pid);
struct ap_slot *ap_get_slot_at(int i);
struct access_pid *ap_get_ref(pid_t pid);
size_t ap_collect_refs(struct ap_slot *arr[], size_t cap);
void ap_release_refs(struct ap_slot *arr[], size_t n);
void ap_put_slot(struct ap_slot *s);
bool ap_slot_empty(void);
int ap_slot_used_count(void);

static inline bool access_pid_is_scanning(pid_t pid)
{
	struct ap_slot *slot = ap_get_slot(pid);
	bool scanning;

	if (!slot)
		return false;
	down_read(&slot->ap_lock);
	scanning = slot->ap->type != NO_SCAN;
	up_read(&slot->ap_lock);
	ap_put_slot(slot);
	return scanning;
}

static inline bool access_pid_cur_last_scanning(struct access_pid *ap)
{
	return ap->type == NORMAL_SCAN && ap->cur_times + 1 >= ap->ntimes;
}

static inline void clear_vm_mapping(u8 *priors, u32 len)
{
	if (priors)
		memset(priors, 0xff, len * sizeof(u8));
}

static inline void set_ap_whole_state(struct access_pid_struct *aps,
				      unsigned long state)
{
	spin_lock(&aps->state_lock);
	aps->state_flag = state;
	spin_unlock(&aps->state_lock);
}

static inline bool check_and_clear_ap_state(struct access_pid_struct *aps,
					    enum ap_state state)
{
	spin_lock(&aps->state_lock);
	if (!(aps->state_flag & state)) {
		spin_unlock(&aps->state_lock);
		return false;
	}
	aps->state_flag = 0;
	spin_unlock(&aps->state_lock);
	return true;
}

#endif /* _SRC_ACCESS_PID_H */
