// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP Tiering Memory Solution: get accessed bit module
 */

#include <asm/pgtable.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/stage2_pgtable.h>

#include <linux/rmap.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>
#include <linux/kvm_host.h>
#include <linux/hugetlb.h>
#include <linux/mmzone.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/pid_namespace.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/rcupdate.h>
#include <linux/bits.h>
#include <linux/stat.h>
#include <linux/pagewalk.h>
#include <linux/swapops.h>

#include "kernel_common.h"
#include "access_tracking_wrapper.h"
#include "access_pid.h"
#include "access_mmu.h"
#include "scan_main.h"
#include "smap_page_flags.h"
#include "accessed_bit.h"
#include "iomem.h"

#define DEFAULT_REF_COUNT 0
#define TRUE_REF 1
#define MAX_NR_KVM 100
#undef pr_fmt
#define pr_fmt(fmt) "access-bit: " fmt

LIST_HEAD(ham_pid_list);
LIST_HEAD(statistic_pid_list);
spinlock_t ham_lock;
struct rw_semaphore statistic_lock;
static u64 freq_info_num;

static int scan_group_size_set(const char *val, const struct kernel_param *kp);

/* Custom parameter operations structure */
static const struct kernel_param_ops scan_group_size_ops = {
	.set = scan_group_size_set,
	.get = param_get_ulong,
};

/* Scan group size kernel parameter: default 64KiB */
unsigned long scan_group_size = 64UL * 1024;
module_param_cb(scan_group_size, &scan_group_size_ops, &scan_group_size, 0644);
MODULE_PARM_DESC(scan_group_size, "Scan group size in bytes (default: 64KiB)");

/**
 * scan_group_size_set - Callback for setting scan group size parameter
 * @val: String containing the new group size value
 * @kp: Pointer to kernel_param structure
 *
 * When user modifies the parameter via sysfs:
 * - Validate the value is positive and a power of 2
 * - Update the global scan group size
 */
static int scan_group_size_set(const char *val, const struct kernel_param *kp)
{
	unsigned long new_size;
	int ret;

	/* Parse parameter value */
	ret = kstrtoul(val, 10, &new_size);
	if (ret)
		return ret;

	/* Validate parameter: must be positive and power of 2 */
	if (new_size == 0 || (new_size & (new_size - 1)) != 0) {
		pr_err("invalid scan group size: %lu, must be > 0 and power of 2\n",
		       new_size);
		return -EINVAL;
	}

	pr_info("scan group size changed: %lu -> %lu\n", scan_group_size,
		new_size);

	/* Update scan group size */
	scan_group_size = new_size;

	return 0;
}

struct smap_vma_struct {
	unsigned long start_vaddr;
	unsigned long end_vaddr;
};

void free_mem(struct acpi_mem_segment *mem)
{
	if (!mem) {
		return;
	}
	kfree(mem);
}

/* Must be called with an elevated refcount on the filp */
static inline bool is_kvm_file(struct file *filp)
{
	if (strcmp(filp->f_inode->i_sb->s_type->name, "anon_inodefs") != 0)
		return false;
	return strcmp(filp->f_path.dentry->d_name.name, "kvm-vm") == 0;
}

/*
 * get_kvm_file_from_task - return struct file if task is a kvm process,
 * return NULL otherwise
 *
 * Context: fput must be called later if return value is non-NULL
 */
struct file *get_kvm_file_from_task(struct task_struct *task)
{
	struct file *filp = NULL;
	unsigned int i;

	if (!task || !task->files)
		return NULL;

	rcu_read_lock();
	task_lock(task);
	for (i = 0; i < files_fdtable(task->files)->max_fds; i++) {
		filp = files_lookup_fd_rcu(task->files, i);
		if (!filp) {
			continue;
		}
		if (!get_file_rcu(filp)) {
			continue;
		}

		if (!is_kvm_file(filp)) {
			fput(filp);
			continue;
		}
		task_unlock(task);
		rcu_read_unlock();
		return filp;
	}
	task_unlock(task);
	rcu_read_unlock();
	return NULL;
}

static int get_pid_from_tracking_file(const struct file *file)
{
	size_t len;
	int pid;
	char *number_str = NULL;
	const char *path = file->f_path.dentry->d_parent->d_name.name;
	if (!path) {
		return -EINVAL;
	}
	len = strlen(path);
	number_str = kmalloc(len, GFP_KERNEL);
	if (number_str == NULL) {
		pr_err("unable to allocate memory for tracking file path\n");
		return -ENOMEM;
	}
	memset(number_str, '\0', len);
	/* File name end with '_t', we don't need the last 2 characters */
	strncpy(number_str, path, len - 2);
	if (kstrtoint(number_str, DECIMAL, &pid) == 0) {
		kfree(number_str);
		return pid;
	}
	kfree(number_str);
	return -1;
}

static inline void clear_tracking_info(struct ham_tracking_info *info)
{
	memset(info->paddr[L1], 0, sizeof(u64) * info->len[L1]);
	memset(info->freq[L1], 0, sizeof(u16) * info->len[L1]);
	if (info->paddr[L2]) {
		memset(info->paddr[L2], 0, sizeof(u64) * info->len[L2]);
		memset(info->freq[L2], 0, sizeof(u16) * info->len[L2]);
	}
}

static struct freq_info *get_freq_info(struct ham_tracking_info *info, u64 *n)
{
	u64 i;
	u64 num = 0;
	struct freq_info *freq_info_array;
	u64 total_len;
	total_len = info->len[L1] + info->len[L2];
	freq_info_array = vzalloc(sizeof(struct freq_info) * total_len);
	if (!freq_info_array) {
		pr_err("unable to allocate memory for frequency info array\n");
		return NULL;
	}
	for (i = 0; i < info->len[L1]; i++) {
		if (info->freq[L1][i] != 0) {
			freq_info_array[num].hpa = info->paddr[L1][i];
			freq_info_array[num].freq = info->freq[L1][i];
			++num;
		}
	}
	if (info->l2_node != -1) {
		for (i = 0; i < info->len[L2]; i++) {
			if (info->freq[L2][i] != 0) {
				freq_info_array[num].hpa = info->paddr[L2][i];
				freq_info_array[num].freq = info->freq[L2][i];
				++num;
			}
		}
	}
	*n = num;
	return freq_info_array;
}

static ssize_t proc_file_read(struct file *file, char __user *buf, size_t count,
			      loff_t *pos)
{
	int ret = 0;
	struct ham_tracking_info *info = NULL;
	struct ham_tracking_info *tmp;
	u64 num = 0;
	struct freq_info *freq_info_array;
	int pid = get_pid_from_tracking_file(file);
	if (pid < 0)
		return -EINVAL;

	spin_lock(&ham_lock);
	list_for_each_entry(tmp, &ham_pid_list, node) {
		if (tmp->pid == pid) {
			info = tmp;
			break;
		}
	}
	spin_unlock(&ham_lock);

	if (!info) {
		pr_err("unable to find tracking file of pid: %d in process file system\n",
		       pid);
		return -ENXIO;
	}

	freq_info_array = get_freq_info(info, &num);
	if (!freq_info_array) {
		pr_err("unable to get frequency info array\n");
		return -ENOMEM;
	}
	if (*pos == 0) {
		if (copy_to_user(buf, &num, sizeof(u64))) {
			pr_err("unable to copy size of frequency info array to user space\n");
			ret = -EFAULT;
		}
		freq_info_num = num;
		*pos += sizeof(u64);
		count = sizeof(u64);
		goto out;
	}
	num = min(num, freq_info_num);
	if (copy_to_user(buf, freq_info_array,
			 sizeof(struct freq_info) * num)) {
		pr_err("unable to copy frequency info array to user space\n");
		ret = -EFAULT;
		goto out;
	}
	*pos += (sizeof(struct freq_info) * num);
	count += (sizeof(struct freq_info) * num);
	clear_tracking_info(info);

out:
	if (freq_info_array) {
		vfree(freq_info_array);
	}
	return ret ? ret : count;
}

static const struct proc_ops proc_file_ops = {
	.proc_read = proc_file_read,
};

int smap_create_tracking_info_file(struct ham_tracking_info *info)
{
	int ret;
	char path[64];
	struct proc_dir_entry *proc_file = NULL;
	struct proc_dir_entry *proc_dir = NULL;

	ret = scnprintf(path, sizeof(path), "%d_t", info->pid);
	if (!ret) {
		pr_err("wirte pid to path of pid: %d failed\n", info->pid);
		return ret;
	}
	proc_dir = proc_mkdir(path, NULL);
	if (!proc_dir) {
		pr_err("unable to create directory of pid: %d in process file system\n",
		       info->pid);
		return -ENOENT;
	}
	proc_file =
		proc_create("tracking_info", S_IRUGO, proc_dir, &proc_file_ops);
	if (!proc_file) {
		pr_err("unable to create tracking info file of pid: %d in process file system\n",
		       info->pid);
		remove_proc_entry(path, NULL);
		return -ENOENT;
	}
	info->pde = proc_dir;
	return 0;
}

int get_ham_pages_freqs(pid_t pid, struct freq_info **freq_info_array,
			uint64_t *freq_info_num)
{
	struct ham_tracking_info *info = NULL;
	struct ham_tracking_info *tmp;
	if (!access_pid_is_scanning(pid)) {
		pr_info("the current access pid: %d is not scanning\n", pid);
		return -EINVAL;
	}
	spin_lock(&ham_lock);
	list_for_each_entry(tmp, &ham_pid_list, node) {
		if (tmp->pid == pid) {
			info = tmp;
			break;
		}
	}
	spin_unlock(&ham_lock);

	if (!info) {
		pr_err("unable to find pid: %d in HAM managed pid list\n", pid);
		return -ENXIO;
	}

	change_ap_type(pid);
	*freq_info_array = get_freq_info(info, freq_info_num);
	if (!*freq_info_array) {
		pr_err("unable to get frequency info of pid: %d\n", pid);
		return -ENOMEM;
	}
	clear_tracking_info(info);
	return 0;
}
EXPORT_SYMBOL(get_ham_pages_freqs);

int pmd_huge(pmd_t md)
{
	return pmd_val(md) && !(pmd_val(md) & PMD_TABLE_BIT);
}

int pud_huge(pud_t ud)
{
#ifndef __PAGETABLE_PMD_FOLDED
	return pud_val(ud) && !(pud_val(ud) & PUD_TABLE_BIT);
#else
	return 0;
#endif
}

pte_t *huge_pte_offset(struct mm_struct *mm, unsigned long addr,
		       unsigned long sz)
{
	pgd_t *gdp;
	p4d_t *p4d;
	pud_t *pudp, ud;
	pmd_t *pmdp, md;

	gdp = pgd_offset(mm, addr);
	if (!pgd_present(READ_ONCE(*gdp)))
		return NULL;

	p4d = p4d_offset(gdp, addr);
	if (!p4d_present(READ_ONCE(*p4d)))
		return NULL;

	pudp = pud_offset(p4d, addr);
	ud = READ_ONCE(*pudp);
	if (sz != PUD_SIZE && pud_none(ud))
		return NULL;
	if (pud_huge(ud) || !pud_present(ud))
		return (pte_t *)pudp;

	if (sz == CONT_PMD_SIZE)
		addr &= CONT_PMD_MASK;

	pmdp = pmd_offset(pudp, addr);
	md = READ_ONCE(*pmdp);
	if (!(sz == PMD_SIZE || sz == CONT_PMD_SIZE) && pmd_none(md))
		return NULL;
	if (pmd_huge(md) || !pmd_present(md))
		return (pte_t *)pmdp;

	if (sz == CONT_PTE_SIZE)
		return pte_offset_kernel(pmdp, (addr & CONT_PTE_MASK));

	return NULL;
}

static inline int get_numa_id_by_paddr(phys_addr_t paddr)
{
	unsigned long pfn = PHYS_PFN(paddr);
	struct page *page;

	if (!pfn_valid(pfn))
		return NUMA_NO_NODE;

	page = pfn_to_online_page(pfn);
	if (!page)
		return NUMA_NO_NODE;

	return page_to_nid(page);
}

static void actc_data_add_fast(phys_addr_t paddr, struct page *page,
			       struct access_pid *ap, u32 page_size)
{
	int ret, nid;
	u64 pa_index;

	if (!page)
		return;

	nid = get_numa_id_by_paddr(paddr);
	if (unlikely(nid == NUMA_NO_NODE))
		return;

	if (nid < nr_local_numa) {
		ret = calc_paddr_acidx_acpi_known_nid(paddr, nid, &pa_index,
						      page_size);
	} else {
		ret = remote_ram_snapshot_paddr_acidx_known_nid(
			ap->remote_snapshot, paddr, nid, &pa_index, page_size);
	}

	if (unlikely(ret))
		return;

	if (unlikely(pa_index >=
		     (nid < nr_local_numa
			      ? get_node_page_count(nid)
			      : remote_ram_snapshot_page_count(
					ap->remote_snapshot, nid, page_size))))
		return;

	/* Pure HW mode: remote freq comes from hardware only; other modes add
	 * software accumulation.
	 */
	if (!is_freq_hist_only_node(nid))
		smap_page_freq_inc(page);
}

static int hva_to_hpa_hugetlb(struct kvm *kvm, u64 host_va,
			      struct access_pid *ap, bool is_young)
{
	struct hstate *h;
	struct vm_area_struct *vma;
	unsigned long hmask;
	unsigned long sz;
	pte_t *ptep;
	pte_t pte;
	phys_addr_t paddr;
	struct page *page;

	if (!kvm)
		return -EINVAL;

	vma = find_vma(kvm->mm, host_va);
	if (!vma || !is_vm_hugetlb_page(vma)) {
		pr_debug("invalid vma for hugetlb, host_va: %llu\n", host_va);
		return -EFAULT;
	}
	h = hstate_vma(vma);
	hmask = huge_page_mask(h);
	sz = huge_page_size(h);
	if (sz != g_pagesize_huge)
		return -EINVAL;

	ptep = huge_pte_offset(kvm->mm, host_va & hmask, sz);
	if (!ptep) {
		return -EFAULT;
	}

	pte = smap_huge_ptep_get(ptep);
	if (!pte_present(pte)) {
		pr_debug("unable to get PTE\n");
		return -EINVAL;
	}

	paddr = PFN_PHYS(pte_pfn(pte));
	page = pfn_to_online_page(pte_pfn(pte));
	if (!page)
		return -EINVAL;
	if (is_young)
		actc_data_add_fast(paddr, page, ap, g_pagesize_huge);

	if (access_pid_cur_last_scanning(ap))
		add_to_bm_huge(host_va, page, ap);

	return 0;
}

static pte_t *smap_pte_offset(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, addr);
	if (!pgd_present(READ_ONCE(*pgd)))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	if (!p4d_present(READ_ONCE(*p4d)))
		return NULL;

	pud = pud_offset(p4d, addr);
	if (!pud_present(READ_ONCE(*pud)))
		return NULL;

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(READ_ONCE(*pmd)))
		return NULL;

	pte = smap__pte_offset_map(pmd, addr, NULL);
	return pte;
}

static int hva_to_hpa_4k(struct kvm *kvm, u64 host_va, struct access_pid *ap,
			 bool is_young)
{
	pte_t *ptep;
	pte_t pte;
	phys_addr_t paddr;
	struct page *page;

	if (!kvm)
		return -EINVAL;

	ptep = smap_pte_offset(kvm->mm, host_va);
	if (!ptep) {
		return -EFAULT;
	}

	pte = ptep_get(ptep);
	if (!pte_present(pte)) {
		pr_debug("unable to get PTE\n");
		pte_unmap(ptep);
		return -EINVAL;
	}

	paddr = __pte_to_phys(pte);
	pte_unmap(ptep);

	page = pfn_to_online_page(pte_pfn(pte));
	if (!page)
		return -EINVAL;

	if (is_young)
		actc_data_add_fast(paddr, page, ap, PAGE_SIZE);

	if (access_pid_cur_last_scanning(ap))
		add_to_bm_page(paddr, page, ap);

	return 0;
}

static void ham_actc_data_add(int pid, phys_addr_t paddr, u32 page_size)
{
	struct ham_tracking_info *tmp;
	int ret, node_id;
	u64 pa_index;

	node_id = get_numa_id_by_paddr(paddr);
	if (unlikely(node_id == NUMA_NO_NODE))
		return;

	if (node_id < nr_local_numa) {
		ret = calc_paddr_acidx_acpi_known_nid(paddr, node_id, &pa_index,
						      page_size);
	} else {
		ret = calc_paddr_acidx_iomem_known_nid(paddr, node_id,
						       &pa_index, page_size);
	}

	if (unlikely(ret))
		return;

	if (unlikely(pa_index >= get_node_page_count(node_id)))
		return;

	spin_lock(&ham_lock);
	list_for_each_entry(tmp, &ham_pid_list, node) {
		if (tmp->pid == pid && tmp->l1_node == node_id) {
			tmp->paddr[L1][pa_index] = paddr;
			tmp->freq[L1][pa_index]++;
		}
		if (tmp->pid == pid && tmp->l2_node == node_id) {
			tmp->paddr[L2][pa_index] = paddr;
			tmp->freq[L2][pa_index]++;
		}
	}
	spin_unlock(&ham_lock);
}

static int hva_to_hpa_ham(struct kvm *kvm, u64 host_va, pid_t pid)
{
	struct hstate *h;
	struct vm_area_struct *vma;
	unsigned long hmask;
	unsigned long sz;
	pte_t *ptep;
	pte_t pte;
	phys_addr_t paddr;

	if (!kvm)
		return -EINVAL;

	vma = find_vma(kvm->mm, host_va);
	if (!vma || !is_vm_hugetlb_page(vma)) {
		pr_debug("invalid vma for hugetlb, host_va: %llu\n", host_va);
		return -EFAULT;
	}
	h = hstate_vma(vma);
	hmask = huge_page_mask(h);
	sz = huge_page_size(h);
	if (sz != g_pagesize_huge) {
		return -EINVAL;
	}

	ptep = huge_pte_offset(kvm->mm, host_va & hmask, sz);
	if (!ptep) {
		return -EFAULT;
	}

	pte = smap_huge_ptep_get(ptep);
	if (!pte_present(pte)) {
		pr_debug("unable to get PTE\n");
		return -EINVAL;
	}

	paddr = PFN_PHYS(pte_pfn(pte));
	ham_actc_data_add(pid, paddr, g_pagesize_huge);
	return 0;
}

static struct vm_area_struct *get_vma_if_huge_page(struct kvm *kvm,
						   unsigned long host_va)
{
	struct vm_area_struct *vma = NULL;
	if (!kvm->mm) {
		return NULL;
	}
	vma = find_vma(kvm->mm, host_va);
	if (!vma) {
		return NULL;
	}
	if (!is_vm_hugetlb_page(vma)) {
		return NULL;
	}
	return vma;
}

static int hva_to_hpa(struct kvm *kvm, u64 host_va, struct access_pid *ap,
		      bool is_young)
{
	int ret = 0;
	struct vm_area_struct *vma;

	if (!kvm || !kvm->mm)
		return -EINVAL;
	vma = find_vma(kvm->mm, host_va);
	if (!vma) {
		return -EFAULT;
	}

	/* The dispatch tracks the VMA backend: a hugetlbfs-backed mapping
	 * follows the hugetlb path, a pte-mapped one is skipped in 2M
	 * scans (pre-4K filter semantics) and scanned per-page in 4K.
	 */
	if (is_vm_hugetlb_page(vma)) {
		if (ap->type == HAM_SCAN) {
			hva_to_hpa_ham(kvm, host_va, ap->pid);
		} else {
			ret = hva_to_hpa_hugetlb(kvm, host_va, ap, is_young);
		}
	} else {
		/* 2M scan over a non-hugetlb backend: skipped, keeping the
		 * pre-4K hugepage-only filter semantics for compat VMs */
		if (is_access_hugepage()) {
			pr_debug("pid %d: 2M scan skipped on non-hugetlb vma\n",
				 ap->pid);
			return 0;
		}
		ret = hva_to_hpa_4k(kvm, host_va, ap, is_young);
	}
	return ret;
}

static void clear_statistic_tracking_info(pid_t pid)
{
	u32 wins_index;
	struct statistics_tracking_info *tmp;
	down_read(&statistic_lock);
	list_for_each_entry(tmp, &statistic_pid_list, node) {
		if (tmp->pid == pid) {
			wins_index = tmp->scan_num;
			if (tmp->sliding_windows[wins_index]) {
				memset(tmp->sliding_windows[wins_index],
				       DEFAULT_REF_COUNT,
				       (tmp->page_num[L1] + tmp->page_num[L2]) *
					       sizeof(u8));
			}
		}
	}
	up_read(&statistic_lock);
}

static void update_statistic_scan_freq_mm(u64 *vaddr, u64 page_cnt, int pid)
{
	u32 wins_index;
	u64 vaddr_idx, l1_idx, l2_idx;
	struct statistics_tracking_info *tmp;
	down_read(&statistic_lock);
	list_for_each_entry(tmp, &statistic_pid_list, node) {
		if (tmp->pid == pid) {
			l1_idx = 0;
			vaddr_idx = 0;
			while (vaddr_idx < page_cnt &&
			       l1_idx < tmp->page_num[L1]) {
				if (vaddr[vaddr_idx] ==
				    tmp->vaddr[L1][l1_idx]) {
					wins_index = tmp->scan_num;
					tmp->sliding_windows[wins_index]
							    [l1_idx] = TRUE_REF;
					l1_idx++;
					vaddr_idx++;
				} else if (vaddr[vaddr_idx] <
					   tmp->vaddr[L1][l1_idx]) {
					vaddr_idx++;
				} else {
					l1_idx++;
				}
			}

			l2_idx = 0;
			vaddr_idx = 0;
			while (vaddr_idx < page_cnt &&
			       l2_idx < tmp->page_num[L2]) {
				if (vaddr[vaddr_idx] ==
				    tmp->vaddr[L2][l2_idx]) {
					wins_index = tmp->scan_num;
					tmp->sliding_windows[wins_index]
							    [l2_idx +
							     tmp->page_num[L1]] =
						TRUE_REF;
					l2_idx++;
					vaddr_idx++;
				} else if (vaddr[vaddr_idx] <
					   tmp->vaddr[L2][l2_idx]) {
					vaddr_idx++;
				} else {
					l2_idx++;
				}
			}
		}
	}
	up_read(&statistic_lock);
}

static void update_statistic_scan_num(pid_t pid)
{
	struct statistics_tracking_info *tmp;
	down_read(&statistic_lock);
	list_for_each_entry(tmp, &statistic_pid_list, node) {
		if (tmp->pid == pid) {
			tmp->scan_num++;
			tmp->scan_num %= tmp->window_num;
			break;
		}
	}
	up_read(&statistic_lock);
}

static bool memslot_is_mem(struct kvm_memory_slot *memslot)
{
	if (memslot->base_gfn < (1 << GB_TO_NORMAL_PAGE_SHIFT)) {
		return false;
	}
	if (memslot->npages <= (1 << HUGE_TO_NORMAL_PAGE_SHIFT)) {
		return false;
	}
	return true;
}

static int collect_statistic_vaddr(struct kvm *kvm,
				   struct kvm_memory_slot *memslot,
				   struct access_pid *ap, int page_size,
				   u64 *vaddr, u64 nr_pages, u64 *cur_index)
{
	gpa_t gpa;
	unsigned long hva;
	bool is_young;

	for (gpa = memslot->base_gfn << PAGE_SHIFT;
	     gpa < (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;
	     gpa += (gpa_t)page_size) {
		is_young =
			smap_kvm_pgtable_stage2_mkold(kvm->arch.mmu.pgt, gpa);
		if (!is_young && !access_pid_cur_last_scanning(ap))
			continue;
		hva = gfn_to_hva_memslot(memslot, gpa_to_gfn(gpa));
		if (!get_vma_if_huge_page(kvm, hva))
			continue;
		if (is_young) {
			vaddr[(*cur_index)++] = hva;
			if (*cur_index >= nr_pages)
				return 0;
		}
	}
	return 0;
}

static void smap_on_pte_young_cb(u64 gpa, bool is_young, bool pte_valid,
				 void *arg)
{
	struct smap_stage2_range_mkold_data *ctx = arg;
	unsigned long hva;
	struct access_pid *ap = (struct access_pid *)ctx->ap;
	bool set_young;

	if (ap->type == STATISTIC_SCAN) {
		/* statistic scan only records valid pages accessed in this round */
		if (!is_young || !pte_valid || !ctx->stat)
			return;
		if (ctx->stat->count >= ctx->stat->capacity)
			return;
		ctx->stat->vaddr[ctx->stat->count++] =
			gfn_to_hva_memslot(ctx->memslot, gpa_to_gfn(gpa));
		return;
	}

	if (!is_young && !access_pid_cur_last_scanning(ap))
		return;

	hva = gfn_to_hva_memslot(ctx->memslot, gpa_to_gfn(gpa));
	set_young = is_young && pte_valid;
	if (set_young || access_pid_cur_last_scanning(ap))
		(void)hva_to_hpa(ctx->kvm, hva, ap, set_young);
}

static void smap_on_hole_cb(u64 gpa_start, u64 gpa_end, void *arg)
{
	struct smap_stage2_range_mkold_data *ctx = arg;
	struct access_pid *ap = ctx->ap;
	unsigned long hva;
	gfn_t start = gpa_start >> PAGE_SHIFT;
	gfn_t end = gpa_end >> PAGE_SHIFT;
	gfn_t gfn;

	/* statistic scan collects 4K pages, block-level holes are ignored */
	if (ap->type == STATISTIC_SCAN)
		return;

	if (!access_pid_cur_last_scanning(ap))
		return;

	for (gfn = start; gfn < end; gfn += ctx->stride) {
		hva = gfn_to_hva_memslot(ctx->memslot, gfn);
		(void)hva_to_hpa(ctx->kvm, hva, ap, false);
	}
}

static int process_memslot_pages(struct kvm *kvm,
				 struct kvm_memory_slot *memslot,
				 struct access_pid *ap, int page_size,
				 struct smap_statistic_collect *stat)
{
	u64 memslot_start = memslot->base_gfn << PAGE_SHIFT;
	u64 memslot_end = (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;
	struct smap_stage2_range_mkold_data data = {
		.kvm = kvm,
		.memslot = memslot,
		.ap = (void *)ap,
		.on_pte_young = smap_on_pte_young_cb,
		.on_hole = smap_on_hole_cb,
		.stat = stat,
	};
	data.stride = (page_size == PMD_SIZE) ? PTRS_PER_PMD : 1;

	return smap_kvm_pgtable_stage2_mkold_range(kvm->arch.mmu.pgt,
						   memslot_start,
						   memslot_end - memslot_start,
						   &data);
}

/*
 * Memslots are ordered by guest gfn instead of host hva, so collecting hvAs
 * in memslot order may break the ascending order expected by
 * update_statistic_scan_freq_mm(). Sort the guest RAM memslots by
 * userspace_addr first: hvAs inside one memslot are monotonic with gfn, so
 * the concatenated collection stays sorted.
 */
#define STATISTIC_MEMSLOT_SORT_MAX 64

static int memslot_hva_cmp(const void *a, const void *b)
{
	struct kvm_memory_slot *const *slot_a = a;
	struct kvm_memory_slot *const *slot_b = b;

	if ((*slot_a)->userspace_addr < (*slot_b)->userspace_addr)
		return -1;
	if ((*slot_a)->userspace_addr > (*slot_b)->userspace_addr)
		return 1;
	return 0;
}

/*
 * Collect the guest RAM memslots in ascending hva order.
 * Returns the memslot count, or -1 if there are more than cap memslots.
 */
static int collect_statistic_memslots(struct kvm_memslots *slots,
				      struct kvm_memory_slot **ordered, int cap)
{
	struct kvm_memory_slot *memslot;
	int cnt = 0;
	int bkt;

	kvm_for_each_memslot(memslot, bkt, slots) {
		if (!memslot_is_mem(memslot))
			continue;
		if (cnt >= cap) {
			pr_warn_once(
				"statistic scan: more than %d memslots, hva order not sorted\n",
				cap);
			return -1;
		}
		ordered[cnt++] = memslot;
	}
	sort(ordered, cnt, sizeof(*ordered), memslot_hva_cmp, NULL);
	return cnt;
}

/* 4K statistic scan: collect HVAs of accessed 4K leaves into vaddr */
static int collect_statistic_4k(struct kvm *kvm, struct access_pid *ap,
				struct kvm_memslots *slots, u64 *vaddr, u64 cap,
				u64 *count)
{
	struct smap_statistic_collect stat = {
		.vaddr = vaddr,
		.capacity = cap,
	};
	struct kvm_memory_slot *ordered[STATISTIC_MEMSLOT_SORT_MAX];
	struct kvm_memory_slot *memslot;
	int nr, bkt, i, ret;

	nr = collect_statistic_memslots(slots, ordered, ARRAY_SIZE(ordered));
	if (nr < 0) {
		kvm_for_each_memslot(memslot, bkt, slots) {
			if (!memslot_is_mem(memslot))
				continue;
			ret = process_memslot_pages(kvm, memslot, ap, PAGE_SIZE,
						    &stat);
			if (ret)
				return ret;
		}
	} else {
		for (i = 0; i < nr; i++) {
			ret = process_memslot_pages(kvm, ordered[i], ap,
						    PAGE_SIZE, &stat);
			if (ret)
				return ret;
		}
	}
	*count = stat.count;
	return 0;
}

/* 2M statistic scan: collect HVAs of accessed huge pages into vaddr */
static int collect_statistic_2m(struct kvm *kvm, struct access_pid *ap,
				struct kvm_memslots *slots, int page_size,
				u64 *vaddr, u64 cap, u64 *count)
{
	struct kvm_memory_slot *ordered[STATISTIC_MEMSLOT_SORT_MAX];
	struct kvm_memory_slot *memslot;
	u64 collected = 0;
	int nr, bkt, i;

	nr = collect_statistic_memslots(slots, ordered, ARRAY_SIZE(ordered));
	if (nr < 0) {
		kvm_for_each_memslot(memslot, bkt, slots) {
			if (!memslot_is_mem(memslot))
				continue;
			collect_statistic_vaddr(kvm, memslot, ap, page_size,
						vaddr, cap, &collected);
		}
	} else {
		for (i = 0; i < nr; i++)
			collect_statistic_vaddr(kvm, ordered[i], ap, page_size,
						vaddr, cap, &collected);
	}
	*count = collected;
	return 0;
}

static void finish_statistic_scan(struct kvm *kvm, struct access_pid *ap,
				  u64 *vaddr, u64 count)
{
	kvm_flush_remote_tlbs(kvm);
	clear_statistic_tracking_info(ap->pid);
	update_statistic_scan_freq_mm(vaddr, count, ap->pid);
	update_statistic_scan_num(ap->pid);
}

static int scan_kvm_memslots(struct kvm *kvm, struct access_pid *ap,
			     int page_size, u64 *stat_vaddr, u64 stat_page_cap)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int bkt, ret;
	u64 count;

	if (!kvm || !kvm->arch.mmu.pgt) {
		pr_err("invalid kvm passed to scan kvm memslots\n");
		return -EINVAL;
	}

	slots = kvm_memslots(kvm);
	if (!slots)
		return -EINVAL;

	if (ap->type != STATISTIC_SCAN) {
		kvm_for_each_memslot(memslot, bkt, slots) {
			if (!memslot_is_mem(memslot))
				continue;
			process_memslot_pages(kvm, memslot, ap, page_size,
					      NULL);
		}
		kvm_flush_remote_tlbs(kvm);
		return 0;
	}

	if (!stat_vaddr || !stat_page_cap)
		return -ENOMEM;
	if (page_size == PAGE_SIZE)
		ret = collect_statistic_4k(kvm, ap, slots, stat_vaddr,
					   stat_page_cap, &count);
	else
		ret = collect_statistic_2m(kvm, ap, slots, page_size,
					   stat_vaddr, stat_page_cap, &count);
	/* A failed round must not be published nor advance the scan window */
	if (ret)
		return ret;
	finish_statistic_scan(kvm, ap, stat_vaddr, count);
	return 0;
}

static inline int pre_scan_kvm_memslots(struct kvm *kvm)
{
	int idx = srcu_read_lock(&kvm->srcu);
	mmap_read_lock(kvm->mm);
	read_lock(&kvm->mmu_lock);
	return idx;
}

static inline void post_scan_kvm_memslots(struct kvm *kvm, int idx)
{
	read_unlock(&kvm->mmu_lock);
	mmap_read_unlock(kvm->mm);
	srcu_read_unlock(&kvm->srcu, idx);
}

static int hva_cmp(const void *a, const void *b)
{
	const struct hva_info *x = a, *y = b;
	if (x->nid < nr_local_numa && y->nid >= nr_local_numa) {
		return -1;
	}
	if (x->nid >= nr_local_numa && y->nid < nr_local_numa) {
		return 1;
	}
	if (x->va < y->va) {
		return -1;
	}
	if (x->va > y->va) {
		return 1;
	}
	return 0;
}

static int get_total_huge_page_nr(struct kvm *kvm, u64 *total_huge_page_nr)
{
	u64 huge_page_nr = 0;
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	gpa_t gpa;
	int bkt;
	slots = kvm_memslots(kvm);
	if (!slots) {
		return -EINVAL;
	}
	kvm_for_each_memslot(memslot, bkt, slots) {
		unsigned long hva;
		for (gpa = memslot->base_gfn << PAGE_SHIFT;
		     gpa < (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;
		     gpa += g_pagesize_huge) {
			hva = gfn_to_hva_memslot(memslot, gpa_to_gfn(gpa));
			if (get_vma_if_huge_page(kvm, hva))
				++huge_page_nr;
		}
	}
	*total_huge_page_nr = huge_page_nr;
	return 0;
}

/* collector capacity: memslot pages converted to scan granularity */
static int get_total_stat_page_nr(struct kvm *kvm, int page_size,
				  u64 *total_page_nr)
{
	u64 page_nr = 0;
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int bkt;

	slots = kvm_memslots(kvm);
	if (!slots)
		return -EINVAL;
	kvm_for_each_memslot(memslot, bkt, slots) {
		if (!memslot_is_mem(memslot))
			continue;
		page_nr += memslot->npages;
	}
	*total_page_nr = page_nr >> (__builtin_ctz(page_size) - PAGE_SHIFT);
	return 0;
}

static int get_total_page_nr(struct smap_vma_struct *vma_array, int len,
			     u64 *total_page_nr)
{
	u64 nr_page = 0;
	int i;

	if (!vma_array) {
		pr_err("vma array is NULL\n");
		return -EINVAL;
	}

	for (i = 0; i < len; i++) {
		nr_page += PHYS_PFN(vma_array[i].end_vaddr) -
			   PHYS_PFN(vma_array[i].start_vaddr);
	}
	*total_page_nr = nr_page;
	return 0;
}

static int get_vma_numa_node(struct kvm *kvm, struct vm_area_struct *vma,
			     unsigned long addr)
{
	struct hstate *h;
	unsigned long hmask;
	unsigned long sz;
	pte_t *ptep;
	pte_t pte;
	int node_calc, page_size;
	u64 index;
	phys_addr_t paddr;

	node_calc = NUMA_NO_NODE;
	if (!kvm) {
		return NUMA_NO_NODE;
	}

	h = hstate_vma(vma);
	hmask = huge_page_mask(h);
	sz = huge_page_size(h);
	if (sz != g_pagesize_huge) {
		return NUMA_NO_NODE;
	}

	ptep = huge_pte_offset(kvm->mm, addr & hmask, sz);
	if (!ptep) {
		return 0;
	}

	pte = smap_huge_ptep_get(ptep);
	if (!pte_present(pte)) {
		return 0;
	}

	paddr = PFN_PHYS(pte_pfn(pte));
	page_size = g_pagesize_huge;
	if (calc_paddr_nid_iomem(paddr, &node_calc, page_size) == 0) {
		return node_calc;
	} else if (calc_paddr_acidx_acpi(paddr, &node_calc, &index,
					 page_size) == 0) {
		return node_calc;
	}

	return NUMA_NO_NODE;
}

static int fill_vaddrs_info(struct kvm *kvm, struct hva_info *hva_vec, u64 len,
			    u64 *l1_page_num, u64 *l2_page_num)
{
	int bkt, cur_node;
	u64 l1_page_nr = 0, l2_page_nr = 0;
	gpa_t gpa;
	struct kvm_memory_slot *memslot;
	struct kvm_memslots *slots = kvm_memslots(kvm);
	u64 idx = 0;
	if (!slots) {
		return -EINVAL;
	}
	kvm_for_each_memslot(memslot, bkt, slots) {
		unsigned long hva;
		for (gpa = memslot->base_gfn << PAGE_SHIFT;
		     gpa < (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;
		     gpa += g_pagesize_huge) {
			if (!memslot_is_mem(memslot))
				continue;
			struct vm_area_struct *vma;
			if (idx >= len) {
				break;
			}
			hva = gfn_to_hva_memslot(memslot, gpa_to_gfn(gpa));
			vma = get_vma_if_huge_page(kvm, hva);
			if (vma == NULL)
				continue;
			cur_node = get_vma_numa_node(kvm, vma, hva);
			hva_vec[idx] = (struct hva_info){ hva, cur_node };
			if (cur_node >= nr_local_numa &&
			    cur_node < MAX_NUMNODES)
				++l2_page_nr;
			else if (cur_node < nr_local_numa && cur_node >= 0)
				++l1_page_nr;
			else {
				pr_err("invalid NUMA index: %d\n", cur_node);
				return -EFAULT;
			}
			++idx;
		}
	}
	if (l1_page_nr + l2_page_nr != len) {
		pr_err("failed to fill page info due to number of pages to fill not equal to that of total pages\n");
		return -EFAULT;
	}
	*l1_page_num = l1_page_nr;
	*l2_page_num = l2_page_nr;
	return 0;
}

static int get_hva_info_by_scan_kvm_memslots(struct kvm *kvm, u64 *l1_page_num,
					     u64 *l2_page_num,
					     u64 total_huge_page_num,
					     u64 *vaddrs,
					     struct hva_info *hva_vec)
{
	u64 l1_page_nr, l2_page_nr, i;
	if (!kvm || !kvm->arch.mmu.pgt) {
		pr_err("invalid kvm passed to scan kvm memslots\n");
		return -EINVAL;
	}

	l1_page_nr = 0;
	l2_page_nr = 0;
	if (fill_vaddrs_info(kvm, hva_vec, total_huge_page_num, &l1_page_nr,
			     &l2_page_nr))
		return -EINVAL;
	pr_debug("L1 huge page num=%llu, L2 huge page num=%llu\n", l1_page_nr,
		 l2_page_nr);
	sort(hva_vec, total_huge_page_num, sizeof(struct hva_info), hva_cmp,
	     NULL);
	for (i = 0; i < total_huge_page_num; ++i) {
		vaddrs[i] = hva_vec[i].va;
	}
	*l1_page_num = l1_page_nr;
	*l2_page_num = l2_page_nr;
	return 0;
}

static int alloc_vaddr_info(struct hva_info **hva_vec, u64 **vaddrs,
			    u64 total_page_nr)
{
	struct hva_info *hva_vec_tmp = NULL;
	u64 *vaddrs_tmp = NULL;

	hva_vec_tmp = vzalloc(total_page_nr * sizeof(struct hva_info));
	if (!hva_vec_tmp) {
		pr_err("unable to allocate memory for page info vector\n");
		return -ENOMEM;
	}
	vaddrs_tmp = vzalloc(total_page_nr * sizeof(u64));
	if (!vaddrs_tmp) {
		pr_err("unable to allocate memory for virtual address vector\n");
		vfree(hva_vec_tmp);
		return -ENOMEM;
	}
	*vaddrs = vaddrs_tmp;
	*hva_vec = hva_vec_tmp;
	return 0;
}

static int alloc_vaddr_info_vm(struct kvm *kvm, struct hva_info **hva_vec,
			       u64 **vaddrs, u64 total_huge_page_nr)
{
	if (!kvm || !kvm->arch.mmu.pgt) {
		pr_err("invalid kvm passed to allocate memory to store page info\n");
		return -EINVAL;
	}

	return alloc_vaddr_info(hva_vec, vaddrs, total_huge_page_nr);
}

/* Caller must free those pointers after successfully called this function */
static int get_kvm_by_pid(pid_t pid_nr, struct pid **pid,
			  struct task_struct **task, struct file **filp,
			  struct kvm **kvm)
{
	struct pid *pid_tmp;
	struct task_struct *task_tmp;
	struct file *filp_tmp;
	struct kvm *kvm_tmp;

	pid_tmp = find_get_pid(pid_nr);
	if (!pid_tmp) {
		pr_err("unable to find pid: %d\n", pid_nr);
		return -ESRCH;
	}
	task_tmp = get_pid_task(pid_tmp, PIDTYPE_PID);
	if (!task_tmp) {
		pr_err("unable to get task struct of pid: %d\n", pid_nr);
		put_pid(pid_tmp);
		return -ESRCH;
	}
	filp_tmp = get_kvm_file_from_task(task_tmp);
	if (!filp_tmp) {
		put_task_struct(task_tmp);
		put_pid(pid_tmp);
		pr_err("unable to get kvm file of pid: %d\n", pid_nr);
		return -EBADF;
	}
	kvm_tmp = filp_tmp->private_data;
	if (!kvm_tmp) {
		fput(filp_tmp);
		put_task_struct(task_tmp);
		put_pid(pid_tmp);
		pr_err("unable to get kvm of pid: %d\n", pid_nr);
		return -EINVAL;
	}
	*pid = pid_tmp;
	*task = task_tmp;
	*filp = filp_tmp;
	*kvm = kvm_tmp;
	return 0;
}

int scan_hva_info(pid_t pid_nr, u64 *l1_page_num, u64 *l2_page_num,
		  u64 **l1_vaddr, u64 **l2_vaddr)
{
	struct pid *pid;
	struct task_struct *task;
	struct file *filp;
	struct kvm *kvm;
	int ret, srcu_idx;
	u64 l1_page_num_tmp, l2_page_num_tmp, total_huge_page_nr;
	u64 *vaddrs;
	struct hva_info *hva_vec;
	ret = get_kvm_by_pid(pid_nr, &pid, &task, &filp, &kvm);
	if (ret) {
		pr_err("unable to get kvm of pid: %d\n", pid_nr);
		return ret;
	}

	srcu_idx = pre_scan_kvm_memslots(kvm);
	ret = get_total_huge_page_nr(kvm, &total_huge_page_nr);
	post_scan_kvm_memslots(kvm, srcu_idx);
	if (ret) {
		pr_err("unable to get total hugepage number of pid: %d\n",
		       pid_nr);
		goto out_free_task_file;
	}

	ret = alloc_vaddr_info_vm(kvm, &hva_vec, &vaddrs, total_huge_page_nr);
	if (ret) {
		pr_err("unable to allocate memory to store virtual address info of pid: %d\n",
		       pid_nr);
		goto out_free_task_file;
	}

	srcu_idx = pre_scan_kvm_memslots(kvm);
	ret = get_hva_info_by_scan_kvm_memslots(kvm, &l1_page_num_tmp,
						&l2_page_num_tmp,
						total_huge_page_nr, vaddrs,
						hva_vec);
	post_scan_kvm_memslots(kvm, srcu_idx);

	if (ret) {
		pr_err("unable to get frequency info of all pages of pid: %d by scan kvm\n",
		       pid_nr);
		vfree(hva_vec);
		vfree(vaddrs);
		goto out_free_task_file;
	}
	*l1_page_num = l1_page_num_tmp;
	*l2_page_num = l2_page_num_tmp;
	*l1_vaddr = vaddrs;
	*l2_vaddr = vaddrs + l1_page_num_tmp;
	vfree(hva_vec);
out_free_task_file:
	fput(filp);
	put_task_struct(task);
	put_pid(pid);
	return ret;
}

static int take_vma_snapshot(struct mm_struct *mm,
			     struct smap_vma_struct **vma_arr, int *vma_count);
static int small_vma_walk(struct mm_struct *mm, unsigned long start_vaddr,
			  unsigned long end_vaddr, struct pte_walk *pte_walk,
			  const struct mm_walk_ops *ops);
static int huge_vma_walk(struct mm_struct *mm, struct smap_vma_struct *vma,
			 struct pte_walk *pte_walk,
			 const struct mm_walk_ops *ops);

static int fill_pte_va(pte_t *pte, unsigned long addr, unsigned long next,
		       struct mm_walk *walk)
{
	struct pte_walk *pte_walk = walk->private;
	struct hva_info *hva_vec = pte_walk->hva_info;
	int page_size = pte_walk->page_size;
	int nid;
	int ret;
	u64 pa_index;
	phys_addr_t paddr;
	pte_t ptent = ptep_get(pte);

	if (is_swap_pte(ptent) || !pte_present(ptent))
		return 0;

	paddr = (phys_addr_t)__pte_to_phys(ptent);
	if (paddr == 0)
		return 0;

	ret = calc_paddr_acidx_acpi(paddr, &nid, &pa_index, page_size);
	if (ret == 0) {
		pr_debug("write %#lx %d to %llu entry\n", addr, nid,
			 pte_walk->index);
		hva_vec[pte_walk->index].va = addr;
		hva_vec[pte_walk->index++].nid = nid;
		pte_walk->nr_page[L1]++;
		return 0;
	}

	ret = calc_paddr_nid_iomem(paddr, &nid, page_size);
	if (ret == 0) {
		pr_debug("write %#lx %d to %llu entry\n", addr, nid,
			 pte_walk->index);
		hva_vec[pte_walk->index].va = addr;
		hva_vec[pte_walk->index++].nid = nid;
		pte_walk->nr_page[L2]++;
	} else {
		pr_debug("unable to calc %#llx nid\n", paddr);
	}
	return 0;
}

static const struct mm_walk_ops stat_ops = {
	.pte_entry = fill_pte_va,
};

/*
 * Walk the given HVA ranges at 4K granularity, collect valid pages and
 * group them by local and remote node. A range may be a process VMA or a
 * guest memslot HVA range.
 */
static int scan_hva_info_4k_ranges(struct mm_struct *mm,
				   struct smap_vma_struct *ranges,
				   int range_cnt, u64 *page_num, u64 **vaddr)
{
	struct pte_walk pte_walk = { .index = 0 };
	struct hva_info *hva_vec = NULL;
	u64 *vaddrs = NULL;
	u64 total_page_nr;
	int i, ret;

	ret = get_total_page_nr(ranges, range_cnt, &total_page_nr);
	if (ret)
		return ret;

	ret = alloc_vaddr_info(&hva_vec, &vaddrs, total_page_nr);
	if (ret)
		return ret;

	pte_walk.hva_info = hva_vec;
	pte_walk.page_size = PAGE_SIZE;
	for (i = 0; i < range_cnt; i++) {
		if (ranges[i].end_vaddr - ranges[i].start_vaddr <
		    MMAPLOCK_BATCH_SIZE) {
			ret = small_vma_walk(mm, ranges[i].start_vaddr,
					     ranges[i].end_vaddr, &pte_walk,
					     &stat_ops);
		} else {
			ret = huge_vma_walk(mm, &ranges[i], &pte_walk,
					    &stat_ops);
		}
		if (ret) {
			vfree(hva_vec);
			vfree(vaddrs);
			return ret;
		}
	}

	/* pte_walk.index stores valid hva length and sort valid hva only */
	sort(hva_vec, pte_walk.index, sizeof(struct hva_info), hva_cmp, NULL);
	for (i = 0; i < pte_walk.index; i++)
		vaddrs[i] = hva_vec[i].va;
	page_num[L1] = pte_walk.nr_page[L1];
	page_num[L2] = pte_walk.nr_page[L2];
	pr_debug("L1 page num=%llu, L2 page num=%llu\n", page_num[L1],
		 page_num[L2]);
	vaddr[L1] = vaddrs;
	vaddr[L2] = vaddrs + page_num[L1];
	vfree(hva_vec);
	return 0;
}

int scan_hva_info_4k(pid_t pid, u64 *page_num, u64 **vaddr)
{
	struct smap_vma_struct *vma_array = NULL;
	struct mm_struct *mm;
	int vma_count = 0;
	int ret;

	mm = get_mm_by_pid(pid);
	if (IS_ERR_OR_NULL(mm)) {
		pr_err("bad mm of pid: %d\n", pid);
		return -EINVAL;
	}
	ret = take_vma_snapshot(mm, &vma_array, &vma_count);
	if (ret) {
		pr_err("failed to take VMA snapshot, ret: %d\n", ret);
		goto out_mm;
	}
	ret = scan_hva_info_4k_ranges(mm, vma_array, vma_count, page_num,
				      vaddr);
	if (ret)
		pr_err("failed to scan hva info of pid: %d, ret: %d\n", pid,
		       ret);
	kfree(vma_array);
out_mm:
	mmput(mm);
	return ret;
}

static void release_resources(struct file *filp, struct task_struct *task,
			      struct pid *pid)
{
	if (filp)
		fput(filp);
	if (task)
		put_task_struct(task);
	if (pid)
		put_pid(pid);
}

/* count HVA ranges of guest memslots for statistic scan */
static int count_memslot_ranges(struct kvm *kvm, int *need)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int srcu_idx, bkt;

	srcu_idx = srcu_read_lock(&kvm->srcu);
	slots = kvm_memslots(kvm);
	if (!slots) {
		srcu_read_unlock(&kvm->srcu, srcu_idx);
		return -EINVAL;
	}
	*need = 0;
	kvm_for_each_memslot(memslot, bkt, slots) {
		if (memslot_is_mem(memslot))
			(*need)++;
	}
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	return 0;
}

/* copy memslot HVA ranges under srcu/mmu_lock, truncating on overflow */
static int fill_memslot_ranges(struct kvm *kvm, struct smap_vma_struct *segs,
			       int cap, int *cnt)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int srcu_idx, bkt;

	*cnt = 0;
	srcu_idx = srcu_read_lock(&kvm->srcu);
	read_lock(&kvm->mmu_lock);
	slots = kvm_memslots(kvm);
	if (!slots) {
		read_unlock(&kvm->mmu_lock);
		srcu_read_unlock(&kvm->srcu, srcu_idx);
		return -EINVAL;
	}
	kvm_for_each_memslot(memslot, bkt, slots) {
		if (!memslot_is_mem(memslot))
			continue;
		if (*cnt >= cap)
			break;
		segs[*cnt].start_vaddr = memslot->userspace_addr;
		segs[*cnt].end_vaddr = memslot->userspace_addr +
				       ((u64)memslot->npages << PAGE_SHIFT);
		(*cnt)++;
	}
	read_unlock(&kvm->mmu_lock);
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	return 0;
}

/* allocate for the actual memslot count and snapshot HVA ranges */
static int collect_memslot_ranges(struct kvm *kvm,
				  struct smap_vma_struct **seg_p, int *seg_cnt)
{
	struct smap_vma_struct *segs;
	int need, ret;

	ret = count_memslot_ranges(kvm, &need);
	if (ret || !need)
		return ret ? ret : -EINVAL;
	segs = kcalloc(need, sizeof(*segs), GFP_KERNEL);
	if (!segs)
		return -ENOMEM;
	ret = fill_memslot_ranges(kvm, segs, need, seg_cnt);
	if (ret || !*seg_cnt) {
		kfree(segs);
		return ret ? ret : -EINVAL;
	}
	*seg_p = segs;
	return 0;
}

/*
 * Initialize 4K statistic scan for a VM: walk only the HVA ranges of guest
 * memslots and reuse the page table walk of the 4K process case.
 */
int scan_hva_info_4k_vm(pid_t pid, u64 *page_num, u64 **vaddr)
{
	struct smap_vma_struct *segs = NULL;
	struct mm_struct *mm = NULL;
	struct pid *pid_s;
	struct task_struct *task;
	struct file *filp;
	struct kvm *kvm;
	int seg_cnt = 0;
	int ret;

	ret = get_kvm_by_pid(pid, &pid_s, &task, &filp, &kvm);
	if (ret) {
		pr_err("unable to get kvm of pid: %d\n", pid);
		return ret;
	}
	ret = collect_memslot_ranges(kvm, &segs, &seg_cnt);
	if (ret)
		goto out_segs;

	mm = get_mm_by_pid(pid);
	if (IS_ERR_OR_NULL(mm)) {
		pr_err("bad mm of pid: %d\n", pid);
		ret = -EINVAL;
		goto out_segs;
	}

	ret = scan_hva_info_4k_ranges(mm, segs, seg_cnt, page_num, vaddr);
	if (ret)
		pr_err("failed to scan hva info of pid: %d, ret: %d\n", pid,
		       ret);
	mmput(mm);
out_segs:
	kfree(segs);
	release_resources(filp, task, pid_s);
	return ret;
}

/*
 * The HVA collector of statistic scan can be large; allocate it with vzalloc
 * outside mmap_lock/mmu_lock to avoid sleeping under the lock and GFP_ATOMIC
 * allocation failures.
 */
static int alloc_statistic_vaddr(struct kvm *kvm, int page_size, u64 **vaddr,
				 u64 *cap)
{
	int srcu_idx;
	int ret;

	srcu_idx = pre_scan_kvm_memslots(kvm);
	ret = get_total_stat_page_nr(kvm, page_size, cap);
	post_scan_kvm_memslots(kvm, srcu_idx);
	if (ret || !*cap)
		return ret ? ret : -EINVAL;
	*vaddr = vzalloc(sizeof(u64) * *cap);
	return *vaddr ? 0 : -ENOMEM;
}

static int scan_forward_vm(struct access_pid *ap, int page_size)
{
	struct pid *pid_s;
	struct task_struct *task;
	struct file *filp;
	struct kvm *kvm;
	u64 *stat_vaddr = NULL;
	u64 stat_page_cap = 0;
	int srcu_idx;
	int ret;

	if (ap->type == NO_SCAN)
		return 0;
	ret = get_kvm_by_pid(ap->pid, &pid_s, &task, &filp, &kvm);
	if (ret)
		return ret;

	if (ap->type == STATISTIC_SCAN) {
		ret = alloc_statistic_vaddr(kvm, page_size, &stat_vaddr,
					    &stat_page_cap);
		if (ret) {
			pr_err("failed to alloc vaddr for statistic scan of pid: %d\n",
			       ap->pid);
			release_resources(filp, task, pid_s);
			return ret;
		}
	}

	srcu_idx = pre_scan_kvm_memslots(kvm);
	ret = scan_kvm_memslots(kvm, ap, page_size, stat_vaddr, stat_page_cap);
	if (ret)
		pr_err("failed to scan kvm mem slots for pid: %d\n", ap->pid);
	post_scan_kvm_memslots(kvm, srcu_idx);
	vfree(stat_vaddr);
	release_resources(filp, task, pid_s);
	return ret;
}

static inline void smap_flush_tlb_mm(struct mm_struct *mm)
{
	unsigned long asid;
	dsb(ishst);
	asid = __TLBI_VADDR(0, ASID(mm));
	__tlbi(aside1is, asid);
	__tlbi_user(aside1is, asid);
	dsb(ish);
}

static int check_pte_young(pte_t *pte, unsigned long addr, unsigned long next,
			   struct mm_walk *walk)
{
	struct pte_walk *pte_walk = walk->private;
	pte_t ptent = ptep_get(pte);
	phys_addr_t paddr = (phys_addr_t)__pte_to_phys(ptent);
	bool is_young = pte_young(ptent);

	if (paddr == 0 || is_swap_pte(ptent) || !pte_present(ptent))
		return 0;

	if (pte_walk->scan_result_cnt >= SCAN_RESULT_CAPACITY)
		return 0;

	/* 64KiB group optimization: for NORMAL_SCAN, skip the rest of the group
	 * when the first page is young.
	 */
	if (pte_walk->type == NORMAL_SCAN) {
		bool is_first = (addr & (scan_group_size - 1)) == 0;
		if (is_first)
			pte_walk->group_hot = is_young;
		else if (pte_walk->group_hot) {
			pte_walk->group_hot_skip_cnt++;
			goto save_res;
		}
	}

	if (is_young) {
		if (pte_walk->type == STATISTIC_SCAN)
			pte_walk->statistic_vaddr[pte_walk->statistic_cnt++] =
				addr;
#ifdef OS_VENDOR_V1
		__ptep_test_and_clear_young(pte);
#else
		__ptep_test_and_clear_young(NULL, 0, pte);
#endif
		pte_walk->flag = true;
	}

save_res:
	pte_walk->scan_results[pte_walk->scan_result_cnt].paddr = paddr;
	pte_walk->scan_results[pte_walk->scan_result_cnt].hot = is_young;
	pte_walk->scan_result_cnt++;

	return 0;
}

static const struct mm_walk_ops pte_range_ops = {
	.pte_entry = check_pte_young,
};

static inline int cal_acidx_and_node_by_paddr(phys_addr_t paddr,
					      struct access_pid *ap, int *nid,
					      u64 *pa_idx, int page_size)
{
	int ret;

	ret = calc_paddr_acidx_acpi(paddr, nid, pa_idx, page_size);
	if (ret) {
		ret = remote_ram_snapshot_paddr_acidx(
			ap->remote_snapshot, paddr, nid, pa_idx, page_size);
	}

	return ret;
}

static void process_scan_results(struct pte_walk *pte_walk)
{
	u64 i, pa_idx;
	int ret;
	bool is_last_scan;
	int nid = 0;

	if (!pte_walk || !pte_walk->ap)
		return;

	is_last_scan = access_pid_cur_last_scanning(pte_walk->ap);

	for (i = 0; i < pte_walk->scan_result_cnt; i++) {
		struct scan_result_entry *entry = &pte_walk->scan_results[i];

		if (entry->hot) {
			unsigned long pfn = PHYS_PFN(entry->paddr);

			if (pfn_valid(pfn))
				folio_set_young(pfn_folio(pfn));
		}

		if (nid < nr_local_numa) {
			ret = calc_paddr_acidx_acpi_known_nid(
				entry->paddr, nid, &pa_idx, PAGE_SIZE);
			if (ret) {
				ret = cal_acidx_and_node_by_paddr(entry->paddr,
								  pte_walk->ap,
								  &nid, &pa_idx,
								  PAGE_SIZE);
			}
		} else {
			ret = remote_ram_snapshot_paddr_acidx_known_nid(
				pte_walk->ap->remote_snapshot, entry->paddr,
				nid, &pa_idx, PAGE_SIZE);
			if (ret) {
				ret = cal_acidx_and_node_by_paddr(entry->paddr,
								  pte_walk->ap,
								  &nid, &pa_idx,
								  PAGE_SIZE);
			}
		}
		if (ret)
			continue;
		entry->nid = nid;
		if (entry->hot) {
			/* Pure HW mode: remote freq comes from hardware only;
			 * other modes add software accumulation.
			 */
			if (!is_freq_hist_only_node(entry->nid)) {
				struct page *pg =
					smap_paddr_to_page(entry->paddr);

				smap_page_freq_inc(pg);
			}
		}
		if (is_last_scan) {
			add_to_bm_page_fast(entry->paddr, entry->nid, pa_idx,
					    pte_walk->ap);
		}
		cond_resched();
	}
}

static int small_vma_walk(struct mm_struct *mm, unsigned long start_vaddr,
			  unsigned long end_vaddr, struct pte_walk *pte_walk,
			  const struct mm_walk_ops *ops)
{
	int ret;

	ret = mmap_read_lock_killable(mm);
	if (ret) {
		pr_err("unable to get mmap read lock, ret: %d\n", ret);
		return ret;
	}
	pte_walk->scan_result_cnt = 0;

	ret = walk_page_range(mm, start_vaddr, end_vaddr, ops, pte_walk);
	mmap_read_unlock(mm);
	cond_resched();
	process_scan_results(pte_walk);
	return ret;
}

static int huge_vma_walk(struct mm_struct *mm, struct smap_vma_struct *vma,
			 struct pte_walk *pte_walk,
			 const struct mm_walk_ops *ops)
{
	int ret;
	unsigned long start_vaddr;
	unsigned long end_vaddr;
	unsigned long end;

	start_vaddr = vma->start_vaddr;
	end_vaddr = vma->end_vaddr;
	while (start_vaddr < end_vaddr) {
		end = (start_vaddr + MMAPLOCK_BATCH_SIZE);
		end = min(end, end_vaddr);
		ret = small_vma_walk(mm, start_vaddr, end, pte_walk, ops);
		if (ret) {
			return ret;
		}
		start_vaddr = end;
	}
	return 0;
}

static int take_vma_snapshot(struct mm_struct *mm,
			     struct smap_vma_struct **vma_arr, int *vma_count)
{
	int count = 0;
	int ret;
	int i = 0;
	struct vm_area_struct *vma;
	struct smap_vma_struct *arr;

	ret = mmap_read_lock_killable(mm);
	if (ret) {
		pr_err("unable to get mmap read lock, ret: %d\n", ret);
		return ret;
	}
	for (vma = find_vma(mm, 0); vma; vma = find_vma(mm, vma->vm_end)) {
		count++;
	}
	if (!count) {
		pr_err("unable to get VMA\n");
		mmap_read_unlock(mm);
		return -EINVAL;
	}
	arr = kzalloc(sizeof(struct smap_vma_struct) * count, GFP_KERNEL);
	if (!arr) {
		pr_err("unable to allocate memory for SMAP VMA descriptor\n");
		mmap_read_unlock(mm);
		return -ENOMEM;
	}
	for (vma = find_vma(mm, 0); vma && i < count;
	     vma = find_vma(mm, vma->vm_end), i++) {
		arr[i].start_vaddr = vma->vm_start;
		arr[i].end_vaddr = vma->vm_end;
	}
	*vma_count = count;
	*vma_arr = arr;
	mmap_read_unlock(mm);
	return 0;
}

static int setup_statistic_scan(struct pte_walk *pte_walk, int pid,
				struct smap_vma_struct *vma_array,
				int vma_count)
{
	int ret;
	u64 total_page_nr;

	if (pte_walk->type != STATISTIC_SCAN)
		return 0;

	ret = get_total_page_nr(vma_array, vma_count, &total_page_nr);
	if (ret) {
		pr_err("failed to get %d page number, ret: %d\n", pid, ret);
		return -EINVAL;
	}
	pte_walk->statistic_vaddr = vzalloc(total_page_nr * sizeof(u64));
	if (!pte_walk->statistic_vaddr) {
		pr_err("failed to malloc statistic vaddr array, ret: %d\n",
		       pid);
		return -ENOMEM;
	}
	return 0;
}

static void update_and_cleanup_statistic(int pid, struct pte_walk *pte_walk,
					 struct mm_struct *mm,
					 struct smap_vma_struct *vma_array)
{
	clear_statistic_tracking_info(pid);
	update_statistic_scan_freq_mm(pte_walk->statistic_vaddr,
				      pte_walk->statistic_cnt, pid);
	update_statistic_scan_num(pid);

	if (pte_walk->flag) {
		smap_flush_tlb_mm(mm);
	}

	kfree(vma_array);

	if (pte_walk->type == STATISTIC_SCAN) {
		vfree(pte_walk->statistic_vaddr);
	}
}

static int walk_all_vmas(struct mm_struct *mm,
			 struct smap_vma_struct *vma_array, int vma_count,
			 struct pte_walk *pte_walk, int pid)
{
	int i, ret;

	for (i = 0; i < vma_count; i++) {
		if (vma_array[i].end_vaddr - vma_array[i].start_vaddr <
		    MMAPLOCK_BATCH_SIZE) {
			ret = small_vma_walk(mm, vma_array[i].start_vaddr,
					     vma_array[i].end_vaddr, pte_walk,
					     &pte_range_ops);
		} else {
			ret = huge_vma_walk(mm, &vma_array[i], pte_walk,
					    &pte_range_ops);
		}
		if (ret) {
			pr_err("failed to walk VMA, pid: %d, ret: %d\n", pid,
			       ret);
			return ret;
		}
	}
	return 0;
}

static int scan_forward_4k_mm(struct access_pid *ap, int page_size)
{
	int ret;
	int vma_count = 0;
	struct mm_struct *mm;
	struct smap_vma_struct *vma_array = NULL;
	struct pte_walk pte_walk = { .flag = false,
				     .ap = ap,
				     .group_hot_skip_cnt = 0,
				     .scan_results = NULL,
				     .scan_result_cnt = 0 };
	pte_walk.pid = ap->pid;
	pte_walk.type = ap->type;

	mm = get_mm_by_pid(ap->pid);
	if (IS_ERR_OR_NULL(mm)) {
		pr_err("bad mm of pid: %d\n", ap->pid);
		return -EINVAL;
	}

	pte_walk.scan_results = vzalloc(SCAN_RESULT_CAPACITY *
					sizeof(struct scan_result_entry));
	if (!pte_walk.scan_results) {
		pr_err("failed to allocate scan results buffer\n");
		mmput(mm);
		return -ENOMEM;
	}

	ret = take_vma_snapshot(mm, &vma_array, &vma_count);
	if (ret) {
		pr_err("failed to take VMA snapshot, ret: %d\n", ret);
		vfree(pte_walk.scan_results);
		mmput(mm);
		return -EINVAL;
	}

	ret = setup_statistic_scan(&pte_walk, ap->pid, vma_array, vma_count);
	if (ret) {
		pr_err("failed to setup statistic scan, ret: %d\n", ret);
		vfree(pte_walk.scan_results);
		kfree(vma_array);
		mmput(mm);
		return -EINVAL;
	}

	ret = walk_all_vmas(mm, vma_array, vma_count, &pte_walk, ap->pid);
	vfree(pte_walk.scan_results);

	update_and_cleanup_statistic(ap->pid, &pte_walk, mm, vma_array);
	mmput(mm);

	if (pte_walk.type == NORMAL_SCAN)
		pr_debug("pid %d: group_hot skipped %llu pages\n", ap->pid,
			 pte_walk.group_hot_skip_cnt);

	return ret;
}

int scan_accessed_bit_forward_vm(struct access_pid *ap, int page_size)
{
	return scan_forward_vm(ap, page_size);
}

int scan_accessed_bit_forward_mm(struct access_pid *ap, int page_size)
{
	if (page_size == PAGE_SIZE) {
		return scan_forward_4k_mm(ap, page_size);
	}
	return -EINVAL;
}
