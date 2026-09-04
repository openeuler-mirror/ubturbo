// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 * Description: smap scan_main module
 */

#include <asm/types.h>
#include <asm/kvm_pgtable.h>

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/hugetlb.h>
#include <linux/mmzone.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/smp.h>

#include "check.h"
#include "iomem.h"
#include "scan_ioctl.h"
#include "access_tracking_wrapper.h"
#include "access_pid.h"
#include "accessed_bit.h"
#include "hist_ops.h"
#include "hist_tracking.h"
#include "scan_main.h"

#define WORKQ_FILE_PATH_LEN 64
#define WORKQ_FILE_BUF_LEN 512

struct set_scan_cpus_work {
	struct work_struct work;
	u32 cpu_start;
	u32 cpu_end;
	int result;
	struct completion done;
};

#define MAX_SCAN_TIME 100000 /* 100s */
#define MS_TO_US 1000
#define DELAY_BUFFER_MS 8

static void access_work_func(struct work_struct *work);

#define to_delay_work(n) container_of(n, struct delayed_work, work)
#define delay_work_to_ap(n) container_of(n, struct access_pid, scan_work)
#undef pr_fmt
#define pr_fmt(fmt) "scan_main: " fmt

unsigned int enable_hist = DISABLE_HIST;
module_param(enable_hist, uint, S_IRUGO);
MODULE_PARM_DESC(enable_hist, "smap hist disable: 0, smap hist enable: 1");

struct access_scan_state {
	struct workqueue_struct *scanq;
	struct rw_semaphore lock;
	char workq_name[WORKQ_NAME_SIZE];
	u8 page_size_mode;
	bool enable_on;
};

static struct access_scan_state scan_state;
static u64 node_page_count[SMAP_MAX_NUMNODES];

u32 g_pagesize_huge;
EXPORT_SYMBOL(g_pagesize_huge);

bool access_scan_enabled(void)
{
	return READ_ONCE(scan_state.enable_on);
}

int get_scan_page_size(void)
{
	return get_scan_page_mode() == PAGE_MODE_2M ? g_pagesize_huge
						    : PAGE_SIZE;
}

u8 get_scan_page_mode(void)
{
	return READ_ONCE(scan_state.page_size_mode);
}

bool is_access_hugepage(void)
{
	return get_scan_page_mode() == PAGE_MODE_2M;
}

u64 get_node_page_count(int node)
{
	if (node < 0 || node >= SMAP_MAX_NUMNODES)
		return 0;
	return READ_ONCE(node_page_count[node]);
}

void set_node_page_count(int node, u64 page_count)
{
	if (node >= 0 && node < SMAP_MAX_NUMNODES)
		WRITE_ONCE(node_page_count[node], page_count);
}

void copy_node_page_count(u64 *page_count)
{
	int node;

	for (node = 0; node < SMAP_MAX_NUMNODES; node++)
		page_count[node] = get_node_page_count(node);
}

ktime_t calc_time_us(ktime_t start_time)
{
	ktime_t cur_time, time_us;
	cur_time = ktime_get();
	time_us = ktime_to_us(ktime_sub(cur_time, start_time));
	return time_us;
}
EXPORT_SYMBOL(calc_time_us);

void cancel_ap_scan_work(struct access_pid *ap)
{
	if (ap && ap->scan_work.work.func) {
		/* cancel_delayed_work_sync returns true if it deactivated a
		 * pending timer; that pending instance's work-ref (taken in
		 * submit_one_work) is now orphaned and must be dropped here.
		 * A running instance observes AP_SLOT_REMOVING and drops its
		 * own work-ref.
		 */
		if (cancel_delayed_work_sync(&ap->scan_work) && ap->slot)
			ap_put_slot(ap->slot);
	}
}

static void set_scan_cpus_work_fn(struct work_struct *work)
{
	struct set_scan_cpus_work *sw =
		container_of(work, struct set_scan_cpus_work, work);
	u32 cpu_index;
	struct file *filp;
	char path[WORKQ_FILE_PATH_LEN];
	char buf[WORKQ_FILE_BUF_LEN];
	ssize_t ret;
	loff_t pos = 0;
	struct cpumask mask;

	if (!scan_state.scanq) {
		sw->result = -EINVAL;
		goto out;
	}

	for (cpu_index = sw->cpu_start; cpu_index <= sw->cpu_end; cpu_index++) {
		if (!cpu_online(cpu_index)) {
			pr_err("cpu %d is not online, cannot be used for scan\n",
			       cpu_index);
			sw->result = -EINVAL;
			goto out;
		}
	}

	scnprintf(path, sizeof(path),
		  "/sys/devices/virtual/workqueue/%s/cpumask",
		  scan_state.workq_name);

	cpumask_clear(&mask);
	for (cpu_index = sw->cpu_start; cpu_index <= sw->cpu_end; cpu_index++)
		cpumask_set_cpu(cpu_index, &mask);

	scnprintf(buf, sizeof(buf), "%*pb\n", cpumask_pr_args(&mask));

	filp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("failed to open workqueue cpumask sysfs: %s\n", path);
		sw->result = PTR_ERR(filp);
		goto out;
	}

	ret = kernel_write(filp, buf, strlen(buf), &pos);
	filp_close(filp, NULL);

	if (ret < 0) {
		pr_err("failed to write workqueue cpumask: %zd\n", ret);
		sw->result = ret;
		goto out;
	}

	sw->result = 0;
out:
	complete(&sw->done);
}

int set_scan_cpus(u32 cpu_start, u32 cpu_end)
{
	struct set_scan_cpus_work sw;

	INIT_WORK(&sw.work, set_scan_cpus_work_fn);
	sw.cpu_start = cpu_start;
	sw.cpu_end = cpu_end;
	sw.result = 0;
	init_completion(&sw.done);

	queue_work(system_unbound_wq, &sw.work);
	wait_for_completion(&sw.done);

	pr_info("set scan cpus from %u to %u, result: %d\n", cpu_start, cpu_end,
		sw.result);

	return sw.result;
}

void submit_one_work(struct access_pid *ap)
{
	pr_debug("submit_one_work: pid=%d, delay=%dms\n", ap->pid,
		 ap->scan_time);
	/* check if work was already initialized */
	cancel_ap_scan_work(ap);
	init_completion(&ap->work_done);
	INIT_DELAYED_WORK(&ap->scan_work, access_work_func);
	/* take a work-reference: held across all rounds of this scan cycle,
	 * dropped at the last round (or on cancel / when the slot is removed).
	 */
	if (ap->slot)
		refcount_inc(&ap->slot->refs);
	queue_delayed_work(scan_state.scanq, &ap->scan_work,
			   msecs_to_jiffies(ap->scan_time));
}

static void submit_scan_works(void)
{
	int i;

	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);
		struct access_pid *ap;

		if (!s)
			continue;
		ap = s->ap;
		/* re-arm the whole scan cycle (ntimes rounds). Hold the
		 * transient ref across submit_one_work so the ap cannot be
		 * reclaimed between; submit_one_work takes its own work-ref.
		 */
		down_write(&s->ap_lock);
		ap->cur_times = 0;
		up_write(&s->ap_lock);
		submit_one_work(ap);
		ap_put_slot(s);
	}
}

static int create_scan_workqueue(void)
{
	scnprintf(scan_state.workq_name, sizeof(scan_state.workq_name),
		  "accessbit_workq");
	scan_state.scanq = alloc_workqueue(
		scan_state.workq_name, WQ_UNBOUND | WQ_SYSFS, WQ_MAX_THREADS);
	if (!scan_state.scanq) {
		pr_err("unable to init access bit workqueue\n");
		return -ENOMEM;
	}
	return 0;
}

static void destroy_scan_workqueue(void)
{
	int i;

	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);

		if (!s)
			continue;
		cancel_ap_scan_work(s->ap);
		ap_put_slot(s);
	}
	if (scan_state.scanq) {
		flush_workqueue(scan_state.scanq);
		destroy_workqueue(scan_state.scanq);
		scan_state.scanq = NULL;
	}
}

static void access_print_acpi_mem(void)
{
#ifdef DEBUG
	struct acpi_mem_segment *mem;
	list_for_each_entry(mem, &acpi_mem.mem, segment) {
		pr_debug("[%d] %#llx-%#llx\n", mem->node, mem->start, mem->end);
	}
#endif
}

static u64 calc_node_page_count(int node)
{
	int page_size = get_scan_page_size();
	u64 page_count;

	if (node >= nr_local_numa) {
		page_count = get_node_page_cnt_iomem(node, page_size);
	} else {
		page_count = get_node_actc_len(node, page_size);
	}
	pr_debug("node: %d, page amount: %llu\n", node, page_count);

	return page_count;
}

static void refresh_node_page_count(void)
{
	int node;

	access_print_acpi_mem();
	for (node = 0; node < SMAP_MAX_NUMNODES; node++) {
		u64 page_count;

		page_count = calc_node_page_count(node);
		if (get_node_page_count(node) != page_count)
			pr_debug(
				"page amount of node %d changed from %llu to %llu\n",
				node, get_node_page_count(node), page_count);
		set_node_page_count(node, page_count);
	}
}

static void access_tracking_enable_scan(void)
{
	down_write(&scan_state.lock);
	refresh_node_page_count();
	WRITE_ONCE(scan_state.enable_on, true);
	up_write(&scan_state.lock);
	submit_scan_works();
}

static int access_tracking_disable_scan(void)
{
	bool all_complete = true;
	int i;

	/*
	 * No global write lock: walk the INUSE slots and check each one's
	 * work_done. add_pid publishes a slot via ap_slot_add before calling
	 * complete(work_done) in the disable window, so a brand-new pid appears
	 * complete and does not force -EBUSY. If any pid still has an
	 * in-flight/pending scan, return -EBUSY and let the upper layer retry;
	 * once all are complete, flip enable_on off.
	 */
	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);

		if (!s)
			continue;
		if (!completion_done(&s->ap->work_done)) {
			all_complete = false;
			ap_put_slot(s);
			break;
		}
		ap_put_slot(s);
	}
	if (all_complete)
		WRITE_ONCE(scan_state.enable_on, false);

	return all_complete ? 0 : -EBUSY;
}

static void access_tracking_set_page_size_scan(u8 page_size_index)
{
	down_write(&scan_state.lock);
	WRITE_ONCE(scan_state.page_size_mode, page_size_index);
	refresh_node_page_count();
	up_write(&scan_state.lock);
	pr_info("set tracking page size to %u\n", page_size_index);
}

void access_tracking_enable(void)
{
	access_tracking_enable_scan();
	if (enable_hist)
		hist_tracking_enable();
}

int access_tracking_disable(void)
{
	int ret;

	ret = access_tracking_disable_scan();
	if (ret)
		return ret;
	if (enable_hist)
		return hist_tracking_disable();
	return 0;
}

int access_tracking_set_page_size(u8 page_size_index)
{
	if (page_size_index != PAGE_MODE_2M && page_size_index != PAGE_MODE_4K)
		return -EINVAL;

	access_tracking_set_page_size_scan(page_size_index);
	return enable_hist ? hist_tracking_set_page_size(page_size_index) : 0;
}

static void scan_state_init(void)
{
	memset(node_page_count, 0, sizeof(node_page_count));
	WRITE_ONCE(scan_state.enable_on, false);
	WRITE_ONCE(scan_state.page_size_mode, PAGE_MODE_2M);
	init_rwsem(&scan_state.lock);
	refresh_node_page_count();
}

static void handle_statistic_scan(struct access_pid *ap, ktime_t start_time,
				  s64 scan_time, unsigned long *scan_delay_ms)
{
	unsigned long delay_buffer_ms;
	if (ap->cur_times == 1) {
		delay_buffer_ms = DELAY_BUFFER_MS;
	} else {
		delay_buffer_ms =
			ktime_to_ms(ktime_sub(start_time, ap->last_scan_end)) -
			ap->last_scan_delay_ms;
	}

	if (*scan_delay_ms < ((scan_time / MS_TO_US) + delay_buffer_ms)) {
		pr_err("pid[%d] scan cost %lums exceeded expected scan time:%lums\n",
		       ap->pid,
		       (unsigned long)((scan_time / MS_TO_US) +
				       delay_buffer_ms),
		       *scan_delay_ms);
		*scan_delay_ms = 0;
	} else {
		*scan_delay_ms -= (scan_time / MS_TO_US + delay_buffer_ms);
	}
	pr_debug(
		"pid[%d] statistic scan delay_buffer_ms :%ldms, scan_delay_ms: %ldms \n",
		ap->pid, delay_buffer_ms, *scan_delay_ms);
}

static void access_work_func(struct work_struct *work)
{
	int ret = 0;
	int page_size;
	struct access_pid *ap;
	struct ap_slot *slot;
	struct delayed_work *scan_work;
	ktime_t start_time, end_time;
	s64 scan_time;
	unsigned long scan_delay_ms;

	start_time = ktime_get();
	scan_work = to_delay_work(work);
	ap = delay_work_to_ap(scan_work);
	slot = ap->slot;

	/* slot may have been removed (access_remove_pid set REMOVING): drop the
	 * work-ref and signal completion without scanning.
	 */
	if (!slot || atomic_read(&slot->state) != AP_SLOT_INUSE) {
		complete(&ap->work_done);
		if (slot)
			ap_put_slot(slot);
		return;
	}

	/*
	 * 当本轮扫描为最后一轮时，需要重新分配 bitmap 给下一轮使用。
	 * 必须在 slot->ap_lock 写锁保护下执行位图的重分配，否则会与
	 * convert_pos_to_paddr_sorted（持读锁访问 bitmap）产生 use-after-free
	 * 竞态。per-slot 写锁仅阻塞该 pid 的读侧，不影响其他 pid 的扫描。
	 */
	if (access_pid_cur_last_scanning(ap)) {
		down_write(&slot->ap_lock);
		access_walk_pagemap_prepare(ap);
		up_write(&slot->ap_lock);
	}

	down_read(&scan_state.lock);
	down_read(&slot->ap_lock);
	page_size = get_scan_page_size();
	if (ap->pid_type == SMAP_PID_VM) {
		ret = scan_accessed_bit_forward_vm(ap, page_size);
	} else {
		ret = scan_accessed_bit_forward_mm(ap, page_size);
	}
	up_read(&slot->ap_lock);
	up_read(&scan_state.lock);
	end_time = ktime_get();
	scan_time = ktime_to_us(ktime_sub(end_time, start_time));
	if (ret < 0) {
		pr_err("unable to scan access-flag, page size: %d\n",
		       page_size);
	}
	ap->cur_times++;
	pr_debug("pid[%d] cpu[%d], scan took %lldus for %dth time\n", ap->pid,
		 raw_smp_processor_id(), scan_time, ap->cur_times);

	scan_delay_ms = ap->scan_time;
	if (ap->type == STATISTIC_SCAN) {
		handle_statistic_scan(ap, start_time, scan_time,
				      &scan_delay_ms);
	}
	ap->last_scan_delay_ms = scan_delay_ms;
	if (ap->cur_times < ap->ntimes &&
	    atomic_read(&slot->state) == AP_SLOT_INUSE) {
		queue_delayed_work(scan_state.scanq, &ap->scan_work,
				   msecs_to_jiffies(scan_delay_ms));
		ap->last_scan_end = ktime_get();
		/* keep the work-ref for the next round */
	} else {
		/* last round, or slot removed mid-cycle: drop the work-ref */
		complete(&ap->work_done);
		ap_put_slot(slot);
	}
}

static int remote_ram_init(void)
{
	int ret;
	ret = refresh_remote_ram();
	if (ret) {
		pr_err("unable to refresh remote ram info, ret: %d\n", ret);
		return ret;
	}
	return 0;
}

int scan_init(void)
{
	int ret = 0;

	g_pagesize_huge = PMD_SIZE;
	ret = init_acpi_mem();
	if (ret) {
		pr_err("unable to init local memory info by ACPI table, ret: %d\n",
		       ret);
		return ret;
	}
	spin_lock_init(&ham_lock);
	init_rwsem(&statistic_lock);
	ret = remote_ram_init();
	if (ret) {
		goto err_remote_ram;
	}

	ret = scan_ioctl_init();
	if (ret) {
		pr_err("unable to init scan ioctl operations\n");
		goto err_ioctl;
	}

	scan_state_init();

	if (enable_hist) {
		ret = hist_module_init();
		if (ret) {
			pr_err("unable to init hist tracking\n");
			goto err_scan_ioctl;
		}
	}
	ret = create_scan_workqueue();
	if (ret) {
		goto err_hist;
	}

	access_print_acpi_mem();

	pr_info("scan subsystem initialized successfully\n");
	return ret;

err_hist:
	if (enable_hist)
		hist_deinit();
err_scan_ioctl:
	scan_ioctl_exit();
err_ioctl:
	release_remote_ram();
err_remote_ram:
	reset_acpi_mem();
	return ret;
}

void scan_exit(void)
{
	scan_ioctl_exit();
	destroy_scan_workqueue();
	if (enable_hist)
		hist_deinit();
	memset(node_page_count, 0, sizeof(node_page_count));
	release_remote_ram();
	reset_acpi_mem();
	pr_info("scan exit successfully\n");
}
