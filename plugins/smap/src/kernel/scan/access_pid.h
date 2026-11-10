/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: smap access pid module
 */

#ifndef _SRC_ACCESS_PID_H
#define _SRC_ACCESS_PID_H

#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/refcount.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/ktime.h>

#include "check.h"
#include "scan_ioctl.h"
#include "common.h"
#include "drv_common.h"
#include "kernel_common.h"
#include "smap_page_flags.h"
#include "smap_swap.h"

#define MAX_PATH_LENGTH 64
#define AP_PROCFS_DIR_LEN 32
#define SEC_TO_MS 1000
extern int nr_local_numa;

/* Upper bound of tracked pids. */
#ifndef AP_MAX_SLOTS
#define AP_MAX_SLOTS MAX_4K_PROCESSES_CNT
#endif

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
 * (paddr_bm/bm_len/info). Readers take a reference via
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

struct remote_ram_snapshot;

struct access_pid {
	pid_t pid;
	smap_pid_type pid_type;
	scan_type type;
	u32 scan_time;
	u32 ntimes;
	u32 cur_times;
	u32 acc_times; /* scan rounds since last cumulative-freq decay */
	bool prior_decay; /* current round ages cumulative freq (see smap_page_flags.h) */
	struct delayed_work scan_work;
	struct completion work_done;
	/* Notify readers only after scanning and bitmap construction finish. */
	wait_queue_head_t wq;
	bool freq_ready; /* window data ready for user-space consumption */
	struct remote_ram_snapshot *remote_snapshot;
	/* ---- cold-page swap-out staging (VM pids only) ----
	 * Cold pages parked with a reference and drained to the swap device
	 * by swap_work on the dedicated swap workqueue.
	 */
	struct smap_swap_buf *swap_buf;
	struct work_struct swap_work;
	/* Start of the current scan window: origin of the cold duration in
	 * seconds accumulated into SMAP_COLD_PERIOD.
	 */
	ktime_t cycle_start;
	u32 scan_count[MAX_NUMNODES];
	size_t page_num[MAX_NUMNODES];
	size_t bm_len[MAX_NUMNODES];
	unsigned long *paddr_bm[MAX_NUMNODES];
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

extern struct list_head ham_pid_list;
extern struct list_head statistic_pid_list;
extern spinlock_t ham_lock;
extern struct rw_semaphore statistic_lock;

void access_remove_ham_pid(int len, struct access_remove_pid_payload *payload);
void access_remove_statistic_pid(int len,
				 struct access_remove_pid_payload *payload);
void destroy_access_pid(struct access_pid *elem);
int init_access_pid(struct access_add_pid_payload *payload,
		    struct access_pid **elem);
int access_add_ham_pid(int len, struct access_add_pid_payload *payload,
		       u32 stride);
int access_add_statistic_pid(int len, struct access_add_pid_payload *payload,
			     u32 stride, int page_size);
int access_add_pid(int len, struct access_add_pid_payload *payload, u32 stride);
void access_remove_pid(int len, struct access_remove_pid_payload *payload);
void access_remove_all_pid(void);
void change_ap_type(pid_t pid);
void clean_last_ap_data(struct access_pid *ap);
int convert_pos_to_paddr_sorted(pid_t pid, int nid, u64 len, u64 *addr);
int init_ap_bm(int node_len, u64 *node_page_count, struct access_pid *ap);
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

/*
 * Seconds elapsed since the start of the current scan window (ceil-rounded):
 * the per-cycle increment written into the SMAP_COLD_PERIOD field, so cold
 * time accumulates in seconds. Defaults to 1 when the window origin is not
 * recorded yet.
 */
static inline u8 ap_cycle_cold_secs(struct access_pid *ap)
{
	u64 ms, secs;

	if (!ap->cycle_start)
		return 1;
	ms = ktime_to_ms(ktime_sub(ktime_get(), ap->cycle_start));
	secs = (ms + 999) / 1000; /* ceil to seconds */
	return (u8)max_t(u64, 1, min_t(u64, secs, MAX_L3_COLD_THRESHOLD));
}

/*
 * Cumulative-freq decay round: every SMAP_ACC_DECAY_CYCLE scan rounds the
 * 5-bit acc counters stored in page->flags are aged (halved) on increment,
 * so that long-term hot pages stay distinguishable from transiently hot
 * ones within the 5-bit range.
 */
static inline bool access_pid_cur_prior_decay(struct access_pid *ap)
{
	return ap->type == NORMAL_SCAN && ap->acc_times >= SMAP_ACC_DECAY_CYCLE;
}

static inline void clear_vm_mapping(u8 *priors, u32 len)
{
	if (priors)
		memset(priors, 0xff, len * sizeof(u8));
}

#endif /* _SRC_ACCESS_PID_H */
