// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap scan ioctl module
 */

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/list.h>

#include "acpi_mem.h"
#include "iomem.h"
#include "check.h"
#include "scan_main.h"
#include "access_pid.h"
#include "scan_ioctl.h"
#include "hist_ops.h"
#include "hist_tracking.h"

#undef pr_fmt
#define pr_fmt(fmt) "scan_ioctl: " fmt
#define MAX_NR_MIGOUT 40
#define MAX_NR_REMOVE MAX_NR_MIGOUT
#define SCHEDULE_INTERVAL (100000)

static dev_t ioctl_scan_dev;
static struct class *scan_class;
static struct cdev scan_cdev;
static struct device *scan_device;
static struct user_info ubturbo_ui = { 0 };
kuid_t procfs_kuid;
kgid_t procfs_kgid;
struct proc_dir_entry *smap_procfs_root = NULL;

static char *smap_bitmap_buf = NULL;
static size_t smap_buf_len = 0;

static int check_msg_validity(struct access_add_pid_msg *msg)
{
	if (!msg) {
		pr_err("null pid message passed to access tracking\n");
		return -EINVAL;
	}
	int max_count = is_access_hugepage() ? MAX_2M_PROCESSES_CNT
					     : MAX_4K_PROCESSES_CNT;
	if (msg->count <= 0 || msg->count > max_count) {
		pr_err("invalid message count: %d passed to access tracking\n",
		       msg->count);
		return -EINVAL;
	}
	if (!msg->payload) {
		pr_err("null payload passed to access tracking\n");
		return -EINVAL;
	}
	return 0;
}

static int add_payload(int len, struct access_add_pid_payload *payload,
		       int page_size)
{
	int ret;
	ret = access_add_ham_pid(len, payload);
	if (ret) {
		pr_err("failed to add HAM pid tracking task, ret: %d\n", ret);
		return ret;
	}
	ret = access_add_statistic_pid(len, payload, page_size);
	if (ret) {
		pr_err("failed to add statistic pid tracking task, ret: %d\n",
		       ret);
		return ret;
	}
	ret = access_add_pid(len, payload);
	return ret;
}

static long ioctl_add_pid(void __user *argp)
{
	int ret = 0, i = 0;
	struct access_add_pid_msg msg;
	struct access_add_pid_payload *payload;
	int page_size = get_scan_page_size();

	if (copy_from_user(&msg, argp, sizeof(msg)))
		return -EFAULT;
	if (check_msg_validity(&msg))
		return -EINVAL;

	payload = vzalloc(sizeof(struct access_add_pid_payload) * msg.count);
	if (!payload) {
		pr_err("unable to allocate memory for access pid payload\n");
		return -ENOMEM;
	}
	if (copy_from_user(payload, msg.payload,
			   sizeof(struct access_add_pid_payload) * msg.count)) {
		ret = -EFAULT;
		goto out_free_payload;
	}

	pr_info("adding pid payload:\n");
	for (i = 0; i < msg.count; i++) {
		if (payload[i].pid == NON_EXIST_PID)
			continue;
		pr_info("[%d] pid %d, numa_nodes %#x, scan_time %u, ntimes %u, duration %u, type %d\n",
			i, payload[i].pid, payload[i].numa_nodes,
			payload[i].scan_time, payload[i].ntimes,
			payload[i].duration, payload[i].type);
		if (payload[i].type >= MAX_SCAN_TYPE || payload[i].type < 0) {
			pr_err("invalid scan type %d of message payload[%d]\n",
			       payload[i].type, i);
			ret = -EINVAL;
			goto out_free_payload;
		}
		if (payload[i].ntimes == 0) {
			pr_err("invalid scan times %d of message payload[%d]\n",
			       payload[i].ntimes, i);
			ret = -EINVAL;
			goto out_free_payload;
		}
	}
	ret = add_payload(msg.count, payload, page_size);
#ifdef DEBUG
	print_access_pid_list();
	print_access_ham_pid_list();
	print_access_statistic_pid_list();
#endif
out_free_payload:
	vfree(payload);
	return ret;
}

static long ioctl_remove_pid(void __user *argp)
{
	int i;
	struct access_remove_pid_msg msg;
	struct access_remove_pid_payload *payload;
	if (copy_from_user(&msg, argp, sizeof(msg)))
		return -EFAULT;
	if (msg.count <= 0 || msg.count > MAX_NR_REMOVE)
		return -EINVAL;
	if (!msg.payload) {
		pr_err("null payload passed to access remove pid\n");
		return -EINVAL;
	}
	payload = vzalloc(sizeof(struct access_remove_pid_payload) * msg.count);
	if (!payload) {
		pr_err("unable to allocate memory for access pid payload\n");
		return -ENOMEM;
	}
	if (copy_from_user(payload, msg.payload,
			   sizeof(struct access_remove_pid_payload) *
				   msg.count)) {
		vfree(payload);
		return -EFAULT;
	}
	pr_info("remove pid payload\n");
	for (i = 0; i < msg.count; i++)
		pr_info("[%d] pid %d\n", i, payload[i].pid);

	access_remove_pid(msg.count, payload);
	access_remove_ham_pid(msg.count, payload);
	access_remove_statistic_pid(msg.count, payload);
#ifdef DEBUG
	print_access_pid_list();
#endif
	vfree(payload);
	return 0;
}

static long ioctl_remove_all_pid(void __user *argp)
{
	access_remove_all_pid();
#ifdef DEBUG
	print_access_pid_list();
#endif
	return 0;
}

static void remove_procfs_root(void)
{
	proc_remove(smap_procfs_root);
	smap_procfs_root = NULL;
}

static int create_procfs_root(struct user_info *ui)
{
	smap_procfs_root = proc_mkdir(SMAP_PROC_ROOT, NULL);
	if (!smap_procfs_root) {
		pr_err("failed to create /proc/%s\n", SMAP_PROC_ROOT);
		return -ENOMEM;
	}

	procfs_kuid = make_kuid(&init_user_ns, ui->uid);
	procfs_kgid = make_kgid(&init_user_ns, ui->gid);
	proc_set_user(smap_procfs_root, procfs_kuid, procfs_kgid);
	return 0;
}

static inline bool is_user_unchanged(struct user_info *ui)
{
	return ui && ui->uid == ubturbo_ui.uid && ui->gid == ubturbo_ui.gid;
}

static long ioctl_create_smap_procfs(void __user *argp)
{
	int ret;
	struct user_info temp_ui;

	if (copy_from_user(&temp_ui, argp, sizeof(temp_ui)))
		return -EFAULT;

	if (smap_procfs_root && is_user_unchanged(&temp_ui)) {
		pr_info("procfs root directory unchanged\n");
		return 0;
	}

	remove_procfs_root();

	ret = create_procfs_root(&temp_ui);
	if (ret) {
		remove_procfs_root();
		return ret;
	}

	pr_info("procfs root directory create\n");
	return 0;
}

#ifndef BYTES_PER_LONG
#define BYTES_PER_LONG 8
#endif

static size_t calc_bitmap_len(void)
{
	size_t buf_len = 0;
	int i;

	/*
	 * Each process's information layout is as follows:
	 * +----------+------------------------------------------------------+
	 * | PID (4B) |        NR_NODE0_PAGE-NR_NODEn_PAGE (n * 8B)          |
	 * +----------+------------------------------------------------------+
	 *
	 * Note: bitmap/mapping data is not transmitted here since mem_freq_read
	 * already assembles complete actc_data including freq, prior, and white_list.
	 */
	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);
		bool normal;

		if (!s)
			continue;
		down_read(&s->ap_lock);
		normal = s->ap->type == NORMAL_SCAN;
		up_read(&s->ap_lock);
		if (normal) {
			buf_len += sizeof(pid_t);
			buf_len += sizeof(size_t) * SMAP_MAX_NUMNODES;
		}
		ap_put_slot(s);
	}

	return buf_len;
}

static long ioctl_walk_pagemap(void __user *argp)
{
	size_t buf_len;
	if (!check_and_clear_ap_state(&ap_data, AP_STATE_WALK)) {
		pr_err("walk pagemap of access pid is not allowed\n");
		return -EAGAIN;
	}
	buf_len = calc_bitmap_len();
	if (!buf_len || copy_to_user(argp, &buf_len, sizeof(buf_len))) {
		set_ap_whole_state(&ap_data, AP_STATE_WALK);
		return -EFAULT;
	} else {
		set_ap_whole_state(&ap_data, AP_STATE_WALK | AP_STATE_READ);
		vfree(smap_bitmap_buf);
		smap_bitmap_buf = NULL;
		smap_buf_len = 0;
	}
	return 0;
}

static inline void write_bitmap_pid(char **buffer, struct access_pid *ap)
{
	if (unlikely(!buffer || !(*buffer) || !ap)) {
		pr_err("invalid buffer or access pid passed to write bitmap\n");
		return;
	}
	memcpy(*buffer, &ap->pid, sizeof(ap->pid));
	*buffer += sizeof(ap->pid);
}

static inline void write_bitmap_nrpage(char **buffer, struct access_pid *ap)
{
	int i;
	if (unlikely(!buffer || !(*buffer) || !ap)) {
		pr_err("invalid buffer or access pid passed to write page number\n");
		return;
	}
	for (i = 0; i < SMAP_MAX_NUMNODES; i++) {
		memcpy(*buffer, &ap->page_num[i], sizeof(ap->page_num[i]));
		*buffer += sizeof(ap->page_num[i]);
	}
}

static void write_bitmap_buffer(char **buffer)
{
	int i;

	if (unlikely(!buffer || !(*buffer))) {
		pr_err("invalid buffer passed to write bitmap buffer\n");
		return;
	}
	for (i = 0; i < AP_MAX_SLOTS; i++) {
		struct ap_slot *s = ap_get_slot_at(i);
		struct access_pid *ap;
		bool normal;

		if (!s)
			continue;
		down_read(&s->ap_lock);
		ap = s->ap;
		normal = ap->type == NORMAL_SCAN;
		if (normal) {
			write_bitmap_pid(buffer, ap);
			write_bitmap_nrpage(buffer, ap);
		}
		up_read(&s->ap_lock);
		ap_put_slot(s);
	}
}

static ssize_t read_bitmap(char __user *buf, size_t cnt, loff_t *loff,
			   bool *completed)
{
	char *tmp_buf;
	ssize_t len;

	*completed = false;
	pr_debug("reading bitmap, smap_buf_len %zu, loff %lld, cnt %zu\n",
		 smap_buf_len, *loff, cnt);
	if (cnt == 0) {
		*completed = true;
		return 0;
	}
	if (*loff > 0)
		goto copy_data;

	smap_buf_len = calc_bitmap_len();
	if (smap_buf_len == 0) {
		*completed = true;
		return 0;
	}

	vfree(smap_bitmap_buf);
	smap_bitmap_buf = vmalloc(smap_buf_len);
	if (!smap_bitmap_buf) {
		pr_err("failed to alloc memory in read_bitmap\n");
		return -ENOMEM;
	}

	tmp_buf = smap_bitmap_buf;
	write_bitmap_buffer(&smap_bitmap_buf);
	smap_bitmap_buf = tmp_buf;

copy_data:
	if (unlikely(*loff >= smap_buf_len)) {
		len = -ERANGE;
		goto free_buf;
	}
	if (*loff + cnt > smap_buf_len)
		len = smap_buf_len - *loff;
	else
		len = cnt;
	if (copy_to_user(buf, smap_bitmap_buf + (*loff), len)) {
		len = -EFAULT;
		goto free_buf;
	}
	if (*loff + len == smap_buf_len) {
		*completed = true;
		goto free_buf;
	}
	*loff += len;
	return len;

free_buf:
	vfree(smap_bitmap_buf);
	smap_bitmap_buf = NULL;
	smap_buf_len = 0;
	*loff = 0;
	return len;
}

static int smap_scan_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int smap_scan_release(struct inode *inode, struct file *file)
{
	return 0;
}

static void update_tracking_data(u16 *tracking_data,
				 struct statistics_tracking_info *stat_info,
				 struct tracking_info_payload *payload_info)
{
	u64 j;
	u32 i, idx;
	payload_info->length =
		payload_info->length > (stat_info->page_num[L1] +
					stat_info->page_num[L2])
			? (stat_info->page_num[L1] + stat_info->page_num[L2])
			: payload_info->length;

	for (idx = 0; idx + SCHEDULE_INTERVAL <= payload_info->length;
	     idx += SCHEDULE_INTERVAL) {
		for (i = 0; i < SCHEDULE_INTERVAL; i++) {
			for (j = 0; j < stat_info->window_num; j++)
				tracking_data[idx + i] +=
					stat_info->sliding_windows[j][idx + i];
		}
		cond_resched();
	}

	for (; idx < payload_info->length; idx++) {
		for (j = 0; j < stat_info->window_num; j++)
			tracking_data[idx] +=
				stat_info->sliding_windows[j][idx];
	}
}

static long ioctl_get_tracking(void __user *argp)
{
	int ret = 0;
	struct tracking_info_payload msg;
	u16 *tracking_data;
	struct statistics_tracking_info *tmp;
	pr_info("Receive ioctl get tracking\n");
	if (copy_from_user(&msg, argp, sizeof(msg)))
		return -EFAULT;

	if (msg.length == 0) {
		pr_err("invalid message length passed to get tracking data\n");
		return -EINVAL;
	}

	if (!msg.data) {
		pr_err("null buffer passed to get tracking data\n");
		return -EINVAL;
	}
	tracking_data = vzalloc(sizeof(u16) * msg.length);
	if (!tracking_data) {
		pr_err("unable to allocate memory for tracking data payload\n");
		return -ENOMEM;
	}

	down_read(&statistic_lock);
	list_for_each_entry(tmp, &statistic_pid_list, node) {
		if (tmp->pid == msg.pid)
			update_tracking_data(tracking_data, tmp, &msg);
	}
	up_read(&statistic_lock);
	/* GET_TRACKING 为 DFX 统计扫描频次，保留原始 u16 真值，不做压缩 */
	if (copy_to_user(argp, &msg, sizeof(msg))) {
		pr_err("failed to copy message to user space\n");
		ret = -EFAULT;
		goto out_free;
	}
	if (copy_to_user(msg.data, tracking_data, sizeof(u16) * msg.length)) {
		pr_err("failed to copy tracking data to user space buffer\n");
		ret = -EFAULT;
	}
	pr_info("Exit ioctl get tracking, ret: %d, outlen: %d\n", ret,
		msg.length);
out_free:
	vfree(tracking_data);
	return ret;
}

static long ioctl_get_nr_local_numa(void __user *argp)
{
	if (copy_to_user(argp, &nr_local_numa, sizeof(int))) {
		pr_err("copy_to_user nr_local_numa failed\n");
		return -EFAULT;
	}
	pr_info("passed nr_local_numa %d to user space\n", nr_local_numa);
	return 0;
}

static long ioctl_set_scan_cpu(void __user *argp)
{
	struct smap_scan_cpu_range range;

	if (copy_from_user(&range, argp, sizeof(range))) {
		pr_err("copy_from_user smap_scan_cpu_range failed\n");
		return -EFAULT;
	}

	if (range.cpu_min > range.cpu_max ||
	    range.cpu_max >= num_possible_cpus()) {
		pr_err("invalid scan cpu range: %d-%d\n", range.cpu_min,
		       range.cpu_max);
		return -EINVAL;
	}

	if (set_scan_cpus(range.cpu_min, range.cpu_max)) {
		pr_err("failed to set scan cpu range: %d-%d\n", range.cpu_min,
		       range.cpu_max);
		return -EINVAL;
	}
	return 0;
}

static long ioctl_tracking_cmd(unsigned long arg)
{
	if (arg == TRACKING_DISABLED)
		return access_tracking_disable();
	access_tracking_enable();
	return 0;
}

static long ioctl_tracking_page_size(unsigned long arg)
{
	return access_tracking_set_page_size((u8)arg);
}

static long ioctl_tracking_ub_watch(void __user *argp)
{
	struct ub_flux_mb_statistic result = { 0 };
	int ret;

	if (!argp || !enable_hist)
		return -ENODEV;
	ret = hist_tracking_ub_watch(&result);
	if (ret)
		return ret;
	return copy_to_user(argp, &result, sizeof(result)) ? -EFAULT : 0;
}

static long ioctl_tracking_ub_watch_config(void __user *argp)
{
	struct ub_watch_config config;

	if (!enable_hist)
		return -ENODEV;
	if (copy_from_user(&config, argp, sizeof(config)))
		return -EFAULT;
	return hist_tracking_ub_watch_config(config.duration_ms);
}

static long smap_scan_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int rc = 0;

	if (_IOC_TYPE(cmd) != SMAP_ACCESS_MAGIC)
		return -EINVAL;

	pr_debug("enter smap_scan_ioctl, nr %u\n", _IOC_NR(cmd));
	switch (cmd) {
	case SMAP_IOCTL_TRACKING_CMD:
		return ioctl_tracking_cmd(arg);
	case SMAP_IOCTL_PAGE_SIZE_SET_CMD:
		return ioctl_tracking_page_size(arg);
	case SMAP_IOCTL_UB_WATCH_CMD:
		return ioctl_tracking_ub_watch(argp);
	case SMAP_IOCTL_UB_WATCH_CONFIG_CMD:
		return ioctl_tracking_ub_watch_config(argp);
	case SMAP_ACCESS_ADD_PID:
		return ioctl_add_pid(argp);
	case SMAP_ACCESS_REMOVE_PID:
		return ioctl_remove_pid(argp);
	case SMAP_ACCESS_REMOVE_ALL_PID:
		return ioctl_remove_all_pid(argp);
	case SMAP_ACCESS_WALK_PAGEMAP:
		return ioctl_walk_pagemap(argp);
	case SMAP_ACCESS_GET_TRACKING:
		return ioctl_get_tracking(argp);
	case SMAP_ACCESS_CREATE_PROCFS:
		return ioctl_create_smap_procfs(argp);
	case SMAP_ACCESS_GET_NR_LOCAL_NUMA:
		return ioctl_get_nr_local_numa(argp);
	case SMAP_ACCESS_REFRESH_REMOTE_RAM:
		rc = refresh_remote_ram();
		if (!rc)
			hist_set_iomem();
		return rc;
	case SMAP_ACCESS_SET_SCAN_CPU:
		return ioctl_set_scan_cpu(argp);
	default:
		rc = -ENOTTY;
	}
	pr_debug("exit smap_scan_ioctl, rc %d\n", rc);

	return rc;
}

static ssize_t smap_scan_read(struct file *file, char __user *buf, size_t cnt,
			      loff_t *loff)
{
	ssize_t ret;
	bool completed = false;

	if (!check_and_clear_ap_state(&ap_data, AP_STATE_READ)) {
		pr_err("read bitmap of access pid is not allowed\n");
		return -EPERM;
	}

	ret = read_bitmap(buf, cnt, loff, &completed);
	if (ret < 0)
		set_ap_whole_state(&ap_data, AP_STATE_WALK);
	else if (completed)
		set_ap_whole_state(&ap_data, AP_STATE_WALK | AP_STATE_FREQ);
	else
		set_ap_whole_state(&ap_data, AP_STATE_WALK | AP_STATE_READ);
	return ret;
}

static struct file_operations smap_scan_fops = {
	.owner = THIS_MODULE,
	.open = smap_scan_open,
	.read = smap_scan_read,
	.unlocked_ioctl = smap_scan_ioctl,
	.release = smap_scan_release,
};

void scan_dev_exit(void)
{
	device_destroy(scan_class, ioctl_scan_dev);
	class_destroy(scan_class);
	cdev_del(&scan_cdev);
	unregister_chrdev_region(ioctl_scan_dev, DEVICE_MINOR_COUNT);
}

int scan_dev_init(void)
{
	int rc = alloc_chrdev_region(&ioctl_scan_dev, DEVICE_BASE_MINOR,
				     DEVICE_MINOR_COUNT, SCAN_DEV);
	if (rc < 0) {
		pr_err("unable to allocate scan character device region\n");
		return rc;
	}

	cdev_init(&scan_cdev, &smap_scan_fops);

	rc = cdev_add(&scan_cdev, ioctl_scan_dev, 1);
	if (rc) {
		pr_err("unable to add scan device to the system\n");
		goto err_cdev;
	}
	scan_class = class_create(SCAN_CLASS);
	if (IS_ERR(scan_class)) {
		pr_err("unable to create the scan class\n");
		rc = PTR_ERR(scan_class);
		goto err_class;
	}

	scan_device = device_create(scan_class, NULL, ioctl_scan_dev, NULL,
				    SCAN_DEVICE);
	if (IS_ERR(scan_device)) {
		pr_err("unable to create the scan device\n");
		rc = PTR_ERR(scan_device);
		goto err_device;
	}

	return 0;

err_device:
	class_destroy(scan_class);
err_class:
	cdev_del(&scan_cdev);
err_cdev:
	unregister_chrdev_region(ioctl_scan_dev, DEVICE_MINOR_COUNT);
	return rc;
}

void scan_ioctl_exit(void)
{
	vfree(smap_bitmap_buf);
	smap_bitmap_buf = NULL;
	smap_buf_len = 0;
	access_remove_all_pid();
	remove_procfs_root();
	scan_dev_exit();
}

int scan_ioctl_init(void)
{
	return scan_dev_init();
}
