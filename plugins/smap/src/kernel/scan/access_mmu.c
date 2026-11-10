// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP access mmu module
 */

#include <linux/hugetlb.h>
#include <linux/pagewalk.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/sched/mm.h>
#include <linux/mm_types.h>
#include <linux/page-isolation.h>

#include "check.h"
#include "scan_main.h"
#include "access_tracking_wrapper.h"
#include "access_mmu.h"
#include "iomem.h"

#define PAGEMAP_WALK_SIZE (PMD_SIZE)
#define PAGEMAP_WALK_MASK (PMD_MASK)

#define PM_ENTRY_BYTES sizeof(pagemap_entry_t)
#define PM_PFRAME_BITS 55
#define PM_PFRAME_MASK GENMASK_ULL(PM_PFRAME_BITS - 1, 0)
#define PM_SOFT_DIRTY BIT_ULL(55)
#define PM_MMAP_EXCLUSIVE BIT_ULL(56)
#define PM_UFFD_WP BIT_ULL(57)
#define PM_FILE BIT_ULL(61)
#define PM_SWAP BIT_ULL(62)
#define PM_PRESENT BIT_ULL(63)

#define PM_END_OF_BUFFER 1

#define FILE_NAME_MAX 32
#define RAM_SLOT_ID 2
#ifdef CONFIG_MEM_SOFT_DIRTY
#define VM_SOFTDIRTY 0x08000000 /* Not soft dirty clean area */
#else
#define VM_SOFTDIRTY 0
#endif

#define PM_LEN 16384
#define PROC_READ_PM_LEN 128

#define MAPPING_U32_BITS (32)

#define MAPPING_PRIOR_BIT 6

static inline pagemap_entry_t make_pme(u64 frame, u64 flags)
{
	return (pagemap_entry_t){ .pme = (frame & PM_PFRAME_MASK) | flags };
}

int add_to_bm_page(u64 paddr, struct page *page, struct access_pid *ap)
{
	int nid, ret;
	u64 acidx;
	u8 freq;
	bool first_hit;
	int page_size = is_access_hugepage() ? g_pagesize_huge : PAGE_SIZE;

	nid = page_to_nid(page);
	if (unlikely(nid == NUMA_NO_NODE))
		return -EINVAL;

	if (nid < nr_local_numa)
		ret = calc_paddr_acidx_acpi_known_nid(paddr, nid, &acidx,
						      page_size);
	else
		ret = remote_ram_snapshot_paddr_acidx_known_nid(
			ap->remote_snapshot, paddr, nid, &acidx, page_size);

	if (ret)
		return ret;

	if (BIT_WORD(acidx) >= ap->bm_len[nid])
		return -ERANGE;

	/* Multiple VAs may be mapped to the same PA. So run test_bit firstly:
	 * the bitmap is window-scoped (wiped and reallocated by
	 * access_walk_pagemap_prepare() at the start of the final round), so
	 * "not yet present" is the per-window first-hit marker. Cold
	 * accounting below must honor it: cold duration accumulates in
	 * seconds, once per scan cycle, on the final round (see
	 * ap_cycle_cold_secs); hot pages reset it. Without the dedup a page
	 * hit by N VAs would accumulate N cold seconds per scan cycle
	 * instead of one and reach smap.l3.cold.threshold N times faster.
	 * Gated by smap.swap.enable: with the switch off no swap activity
	 * happens at all (counting, staging, swap-out). VM anon base pages
	 * whose accumulated cold time reaches the configured swap period
	 * are staged for kernel swap-out. A page cannot be staged twice:
	 * staging requires mapcount==1 (one virtual address per pid) and
	 * each pid owns its own buffer; a stage failure leaves the page
	 * flags untouched, so the page keeps its accumulated duration and
	 * is probed again next window.
	 */
	first_hit = !_test_bit(acidx, ap->paddr_bm[nid]);

	freq = smap_page_freq_get(page);
	if (first_hit && get_swap_enable() &&
	    access_pid_cur_last_scanning(ap)) {
		if (freq == 0) {
			u8 period = smap_page_cold_period_add(
				page, ap_cycle_cold_secs(ap));

			if (ap->pid_type == SMAP_PID_VM &&
			    period >= get_l3_cold_threshold()) {
				if (smap_swap_stage(ap, page))
					return 0;
			}
			/* Staged pages belong to the swap worker now: the
			 * early return keeps them out of the migration
			 * candidacy bitmap entirely; a failed stage (buffer
			 * full / capability off) falls through to the bitmap
			 * with no page-flags compensation needed.
			 */
		} else {
			smap_page_cold_periods_reset(page);
		}
	}

	if (first_hit) {
		__set_bit(acidx, ap->paddr_bm[nid]);
		ap->page_num[nid]++;
	}
	return 0;
}

int add_to_bm_page_fast(u64 paddr, int nid, u64 acidx, struct access_pid *ap)
{
	if (BIT_WORD(acidx) >= ap->bm_len[nid])
		return -ERANGE;

	/* Multiple VAs may be mapped to the same PA. So run test_bit firstly */
	if (!_test_bit(acidx, ap->paddr_bm[nid])) {
		__set_bit(acidx, ap->paddr_bm[nid]);
		ap->page_num[nid]++;
	}
	return 0;
}

static int calc_vaddr_acidx(u64 vaddr, struct vm_mapping_info *info, u64 *acidx)
{
	int i;
	*acidx = 0;
	int shift = __builtin_ctz(g_pagesize_huge);
	for (i = 0; i < info->nr_segs; i++) {
		bool flag = vaddr < info->segs[i].start ||
			    vaddr >= info->segs[i].end;
		if (flag) {
			*acidx += info->segs[i].hugepages;
			continue;
		}
		*acidx += (vaddr - info->segs[i].start) >> shift;
		return 0;
	}
	return -ERANGE;
}

static void set_pa_prior(struct access_pid *ap, u64 vaddr)
{
	u8 prior;
	int prior_bits;
	u64 va_idx;
	int shift = __builtin_ctz(g_pagesize_huge);
	int ret = calc_vaddr_acidx(vaddr, &ap->info, &va_idx);
	if (ret != 0 || va_idx >= ap->info.vm_size) {
		pr_debug("set pa prior out of range\n");
		return;
	}
	if (va_idx & (1 << (shift - 1))) {
		pr_debug("va_idx is not aligned\n");
		return;
	}

	prior_bits = MAPPING_U32_BITS - __builtin_clz(ap->info.vm_size);
	if (prior_bits > MAPPING_PRIOR_BIT) {
		prior = va_idx >> (prior_bits - MAPPING_PRIOR_BIT);
	} else {
		prior = va_idx;
	}
	ap->info.priors[va_idx] = prior;
}

int add_to_bm_hugepage(u64 vaddr, struct page *page, struct access_pid *ap)
{
	int nid, ret;
	u64 acidx;
	u64 paddr;
	u8 freq;
	bool first_hit;
	int page_size = is_access_hugepage() ? g_pagesize_huge : PAGE_SIZE;

	paddr = page_to_phys(page);
	nid = page_to_nid(page);
	if (unlikely(nid == NUMA_NO_NODE))
		return -EINVAL;

	if (nid < nr_local_numa)
		ret = calc_paddr_acidx_acpi_known_nid(paddr, nid, &acidx,
						      page_size);
	else
		ret = remote_ram_snapshot_paddr_acidx_known_nid(
			ap->remote_snapshot, paddr, nid, &acidx, page_size);

	if (ret)
		return ret;

	if (BIT_WORD(acidx) >= ap->bm_len[nid]) {
		return -ERANGE;
	}

	/* Multiple VAs may be mapped to the same PA. So run test_bit firstly.
	 * The bitmap is window-scoped, so "not yet present" is the per-window
	 * first-hit marker: run cold accounting below once per PA per window
	 * (same rationale as add_to_bm_page()). */
	first_hit = !test_bit(acidx, ap->paddr_bm[nid]);

	freq = smap_page_freq_get(page);
	/*
	 * Cold duration accumulates in seconds, once per scan cycle, on the
	 * final round of the window. Gated by smap.swap.enable: with the
	 * switch off none of the swap-related accounting runs.
	 *
	 * hugetlb capability gate is explicit here (hugetlb_swap_enabled,
	 * probed from /proc/meminfo HugePages_Swap and delivered by the swap
	 * config ioctl): if the running kernel cannot swap hugetlb folios,
	 * 2M pages fall straight back to the migration bitmap and smap_swap
	 * staging is never attempted. smap_swap_stage() re-checks the
	 * capability defensively.
	 */
	if (first_hit && get_swap_enable() && smap_swap_hugetlb_enabled() &&
	    access_pid_cur_last_scanning(ap)) {
		if (freq == 0) {
			u8 period = smap_page_cold_period_add(
				page, ap_cycle_cold_secs(ap));

			/* staged pages belong to the swap worker: the early
			 * return keeps them out of the migration candidacy
			 * bitmap; a failed stage needs no page-flags
			 * compensation */
			if (period >= get_l3_cold_threshold()) {
				if (smap_swap_stage(ap, page))
					return 0;
			}
		} else {
			smap_page_cold_periods_reset(page);
		}
	}

	if (first_hit) {
		set_bit(acidx, ap->paddr_bm[nid]);
		ap->page_num[nid]++;
	}
	set_pa_prior(ap, vaddr);
	return 0;
}

static bool is_paddr_belong_remote_node(u64 pa, int nid)
{
	struct ram_segment *seg;
	int numa = NUMA_NO_NODE;

	read_lock(&rem_ram_list_lock);
	list_for_each_entry(seg, &remote_ram_list, node) {
		if (pa < seg->start)
			break;
		if (pa <= seg->end) {
			numa = seg->numa_node;
			break;
		}
	}

	if (unlikely(list_entry_is_head(seg, &remote_ram_list, node))) {
		read_unlock(&rem_ram_list_lock);
		return false;
	}
	read_unlock(&rem_ram_list_lock);
	return numa == nid;
}

static bool is_page_ready_for_migrate(struct page *page)
{
	if (is_migrate_isolate_page(page)) {
		return false;
	}
	if (is_access_hugepage()) {
		if (PageHuge(page) && !PageHead(page))
			return false;
	} else {
		if (__folio_test_movable(page_folio(page)) ||
		    page_ref_count(page) == 0 || PageTransHuge(page) ||
		    PageHuge(page)) {
			return false;
		}
	}
	if (page_mapcount(page) > 1) {
		pr_debug("page mapcount invalid.\n");
		return false;
	}
	if (!PageHuge(page)) {
		if (!folio_try_get(page_folio(page))) {
			return false;
		}
	}
	return true;
}

static int add_to_bm(unsigned long vaddr, pagemap_entry_t *pme,
		     struct pagemapread *pm)
{
	u64 paddr;
	u64 pfn;
	struct page *page;
	pfn = pme->pme & PM_PFRAME_MASK;
	if (!pfn || !(pme->pme & PM_PRESENT))
		goto inc_pm_pos;
	paddr = PFN_PHYS(pfn);

	page = pfn_to_online_page(pfn);
	if (!page)
		goto inc_pm_pos;

	if (pm->mig_type == REMOTE_MIGRATE) {
		pm->mig_info.page_cnt++;
		if (!is_paddr_belong_remote_node(paddr,
						 pm->mig_info.remote_nid)) {
			goto inc_pm_pos;
		}

		if (!is_page_ready_for_migrate(page)) {
			goto inc_pm_pos;
		}

		if (is_page_hwpoison(page)) {
			pr_err("unsupported page type: hwpoison\n");
			goto inc_pm_pos;
		}
		if (pm->mig_info.mig_cnt < pm->mig_info.folios_len) {
			pm->mig_info.folios[pm->mig_info.mig_cnt++] =
				page_folio(page);
		}
	} else {
		if (is_access_hugepage())
			add_to_bm_huge(vaddr, page, pm->ap);
		else
			add_to_bm_page(paddr, page, pm->ap);
	}

inc_pm_pos:
	if (++pm->pos >= pm->len)
		return PM_END_OF_BUFFER;
	return 0;
}

static pagemap_entry_t pte_to_pagemap_entry(struct pagemapread *pm,
					    struct vm_area_struct *vma,
					    unsigned long addr, pte_t pte)
{
	u64 frame = 0, flags = 0;
	swp_entry_t entry;
	pgoff_t offset;

	if (pte_present(pte)) {
		frame = pte_pfn(pte);
		flags |= PM_PRESENT;
	} else if (is_swap_pte(pte)) {
		entry = pte_to_swp_entry(pte);
		offset = swp_offset(entry);
		frame = swp_type(entry) | (offset << MAX_SWAPFILES_SHIFT);
	}

	return make_pme(frame, flags);
}

void pmd_clear_bad(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	pmd_clear(pmd);
}

spinlock_t *__pmd_trans_huge_lock(pmd_t *pmd, struct vm_area_struct *vma)
{
	spinlock_t *ptl;
	ptl = pmd_lock(vma->vm_mm, pmd);
	if (likely(is_swap_pmd(*pmd) || pmd_trans_huge(*pmd) ||
		   pmd_devmap(*pmd)))
		return ptl;
	spin_unlock(ptl);
	return NULL;
}

static int pagemap_pte_range(pte_t *pte, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	struct pagemapread *pm = walk->private;
	pagemap_entry_t pme;

	pme = pte_to_pagemap_entry(pm, vma, addr, ptep_get(pte));
	return add_to_bm(addr, &pme, pm);
}

static int pagemap_hugetlb_range(pte_t *ptep, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk)
{
	struct pagemapread *pm = walk->private;
	struct vm_area_struct *vma = walk->vma;
	pagemap_entry_t pme;
	u64 flags = 0, frame = 0;
	int err = 0;
	pte_t pte;

	if (vma->vm_flags & VM_SOFTDIRTY)
		flags |= PM_SOFT_DIRTY;

	pte = smap_huge_ptep_get(ptep);
	if (pte_present(pte)) {
		flags |= PM_PRESENT;
		frame = pte_pfn(pte) + ((addr & ~hmask) >> PAGE_SHIFT);
		pme = make_pme(frame, flags);
		err = add_to_bm(addr, &pme, pm);
	}
	return err;
}

static const struct mm_walk_ops pagemap_mm_ops = {
	.pte_entry = pagemap_pte_range,
};

static const struct mm_walk_ops pagemap_vm_ops = {
	.hugetlb_entry = pagemap_hugetlb_range,
};

static void pos_to_addr(struct mm_struct *mm, unsigned long pos,
			unsigned long *vaddr)
{
	unsigned long count, nr_pages;
	u64 last_vaddr;
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	MA_STATE(mas, &mm->mm_mt, 0, ULONG_MAX);
	vmi.mas = mas;
	count = 0;
	last_vaddr = 0;

	mmap_assert_locked(mm);

	for (vma = vma_find(&vmi, ULONG_MAX); vma; vma = vma_next(&vmi)) {
		if (is_access_hugepage() && !is_vm_hugetlb_page(vma)) {
			continue;
		}
		last_vaddr = vma->vm_end;
		if (count == pos) {
			*vaddr = vma->vm_start;
			return;
		}
		nr_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
		if (count + nr_pages > pos) {
			*vaddr = vma->vm_start + (pos - count) * PAGE_SIZE;
			return;
		}
		count += nr_pages;
	}
	*vaddr = last_vaddr;
}

struct mm_struct *get_mm_by_pid(pid_t pid)
{
	struct task_struct *task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	struct mm_struct *mm = ERR_PTR(-ESRCH);
	int err;

	if (task) {
		err = down_read_killable(&task->signal->exec_update_lock);
		if (err) {
			put_task_struct(task);
			return ERR_PTR(err);
		}

		mm = get_task_mm(task);
		up_read(&task->signal->exec_update_lock);
		put_task_struct(task);

		if (!IS_ERR_OR_NULL(mm)) {
			/* ensure this mm_struct can't be freed */
			mmgrab(mm);
			/* but do not pin its memory */
			mmput(mm);
		}
	}

	return mm;
}

void walk_pid_pagemap(struct pagemapread *pm)
{
	struct mm_struct *mm;
	unsigned long svpfn = 0;
	unsigned long evpfn;
	unsigned long start_vaddr, end_vaddr;
	ktime_t start_time, pagemap_time;
	start_time = ktime_get();
	if (!pm) {
		return;
	}
	/* 获取mm_struct */
	mm = get_mm_by_pid(pm->mig_info.pid);
	if (IS_ERR(mm))
		return;
	if (!mm || !mmget_not_zero(mm))
		return;
	pm->len = PM_LEN;

	while (1) {
		pm->pos = 0;
		/* walk page */
		if (mmap_read_lock_killable(mm)) {
			mmput(mm);
			return;
		}
		/* watch out for wraparound */
		evpfn = svpfn + (unsigned long)pm->len;
		if (svpfn > ULONG_MAX - (unsigned long)pm->len)
			evpfn = ULONG_MAX;

		pos_to_addr(mm, svpfn, &start_vaddr);
		pos_to_addr(mm, evpfn, &end_vaddr);
		/* break if reach the last vma */
		if (start_vaddr == end_vaddr) {
			mmap_read_unlock(mm);
			break;
		}

		if (is_access_hugepage()) {
			walk_page_range(mm, start_vaddr, end_vaddr,
					&pagemap_vm_ops, pm);
		} else {
			walk_page_range(mm, start_vaddr, end_vaddr,
					&pagemap_mm_ops, pm);
		}
		mmap_read_unlock(mm);
		svpfn = evpfn;
	}
	pagemap_time = calc_time_us(start_time);
	pr_debug("walk pagemap of pid: %d took %lldus\n", pm->mig_info.pid,
		 pagemap_time);
	mmput(mm);
}
EXPORT_SYMBOL(walk_pid_pagemap);
