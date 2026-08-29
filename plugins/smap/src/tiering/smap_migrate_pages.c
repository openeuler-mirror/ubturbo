// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP Tiering Memory Solution: SMAP MIGRATE PAGES
 */

#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/rmap.h>
#include <linux/ktime.h>
#include <linux/gfp.h>
#include <linux/cpumask.h>
#include <linux/page-isolation.h>
#include <linux/limits.h>
#include <linux/mm.h>

#include "numa.h"
#include "rmap.h"
#include "iomem.h"
#include "dump_info.h"
#include "smap_migrate_wrapper.h"
#include "common.h"
#include "critical.h"
#include "smap_migrate_pages.h"

#define MAX_MIGRATE_NUMA_RETRY_TIME 10

#define NR_PAGE_LOW_WMARK 32

#define INVALID_PADDR 0

unsigned int remote_migrate_mode = MIGRATE_ASYNC;

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_migrate: " fmt

struct num_list {
	struct hlist_head hlist_head;
};

struct num_node {
	int num;
	u64 page_phy_addr;
	struct hlist_node hlist_node;
};

extern u32 g_pagesize_huge;

static struct migrate_node {
	int next_nid;
	unsigned long nr[SMAP_MAX_NUMNODES];
} migrate_node;

#ifdef CONFIG_MIGRATE_PAGES_DMA_OFFLOADING
void set_remote_migrate_mode(unsigned int mode)
{
	if (mode) {
		remote_migrate_mode = MIGRATE_ASYNC_DMA_OFFLOADING;
	} else {
		remote_migrate_mode = MIGRATE_ASYNC;
	}
	pr_info("set remote migrate mode: %u\n", remote_migrate_mode);
}
#else
void set_remote_migrate_mode(unsigned int mode)
{
	pr_info("config MIGRATE_PAGES_DMA_OFFLOADING is not enabled, remote migrate mode is not supported\n");
}
#endif

static int smap_check_huge_page_for_migration(struct page *page, pid_t pid)
{
	struct page_task_arg pta = { 0 };
	if (!PageHead(page))
		return -1;
	if (pid) {
		pta.type = PAGE_PID_TYPE;
		pta.pid = pid;
		find_page_task(page, 0, &pta);
		if (!pta.found) {
			pr_debug("wrong pid %d\n", pid);
			return -EINVAL;
		}
	}
	return 0;
}

static int smap_add_page_for_migration(struct page *page, struct folio **folios,
				       unsigned int *nr_folios, pid_t pid,
				       bool migrate_all)
{
	int err;
	err = PTR_ERR(page);
	if (IS_ERR(page)) {
		pr_debug("invalid page passed to add page for migration\n");
		return err;
	}

	err = -ENOENT;
	if (!page) {
		pr_debug("null page passed to add page for migration\n");
		return err;
	}

	err = -EACCES;
	if (page_mapcount(page) > 1 && !migrate_all) {
		pr_debug("invalid page map count or null migrate all flag\n");
		return err;
	}
	if (!folio_try_get(page_folio(page))) {
		pr_debug("failed to add folio reference\n");
		return -EINVAL;
	}
	if (PageHuge(page)) {
		err = smap_check_huge_page_for_migration(page, pid);
		if (err) {
			folio_put(page_folio(page));
			return err;
		}
	}

	folios[*nr_folios] = page_folio(page);
	(*nr_folios)++;
	return 0;
}

struct migration_target_control {
	int nid; /* preferred node id */
	nodemask_t *nmask;
	gfp_t gfp_mask;
};

static void put_folios(struct folio **folios, unsigned int nr_folios)
{
	unsigned int i;
	for (i = 0; i < nr_folios; i++) {
		folio_put(folios[i]);
	}
}

static int smap_isolate_and_migrate_folios(struct folio **folios,
					   unsigned int nr_folios,
					   new_folio_t get_new_folio,
					   free_folio_t put_new_folio,
					   unsigned long private,
					   enum migrate_mode mode,
					   unsigned int *nr_succeeded)
{
	int ret = 0;
	unsigned int i;
	struct folio *folio;
	LIST_HEAD(source);

	for (i = 0; i < nr_folios; i++) {
		folio = folios[i];
		if (!folio_test_hugetlb(folio)) {
			VM_BUG_ON_FOLIO(!folio_ref_count(folio), folio);
		}
		fp_isolate_folio_to_list(folio, &source);
		if (!folio_test_hugetlb(folio)) {
			folio_put(folio);
		}
	}
	if (!list_empty(&source)) {
		ret = fp_migrate_pages(&source, get_new_folio, put_new_folio,
				       private, mode, MR_HOTNESS, nr_succeeded);
		if (ret)
			fp_putback_movable_pages(&source);
		if (nr_succeeded && *nr_succeeded)
			count_vm_numa_events(NUMA_PAGE_MIGRATE, *nr_succeeded);
	}

	return ret;
}

unsigned int smap_migrate(struct folio **folios, unsigned int nr_folios,
			  int to_node, enum smap_migrate_type type)
{
	int err = 0;
	unsigned int nr_succeeded = 0;
	ktime_t start_time = 0;
	ktime_t mig_time = 0;
	if (nr_folios == 0 || !folios) {
		pr_debug("no folio to migrate\n");
		return nr_folios;
	}

	if (to_node < 0 || to_node >= SMAP_MAX_NUMNODES) {
		put_folios(folios, nr_folios);
		pr_debug(
			"invalid destination node: %d passed to SMAP migrate\n",
			to_node);
		return nr_folios;
	}
	start_time = ktime_get();
	if (MIGRATE_TYPE_BACK == type) {
		err = isolate_and_migrate_folios(
			folios, nr_folios, smap_alloc_new_node_page_mig_back,
			NULL, to_node, remote_migrate_mode, &nr_succeeded);
		if (err) {
			pr_err("failed to migrate back, ret: %d\n", err);
		}
	} else if (MIGRATE_TYPE_HOTNESS == type) {
		err = isolate_and_migrate_folios(folios, nr_folios,
						 smap_alloc_new_node_page, NULL,
						 to_node, remote_migrate_mode,
						 &nr_succeeded);
		if (err) {
			pr_err("failed to migrate, ret: %d\n", err);
		}
	} else if (MIGRATE_TYPE_REMOTE == type) {
		err = smap_isolate_and_migrate_folios(
			folios, nr_folios, smap_alloc_new_node_page, NULL,
			to_node, remote_migrate_mode, &nr_succeeded);
		if (err > 0)
			pr_warn("isolat or migrate remote range err: %d\n",
				err);
		else if (err < 0)
			pr_err("failed to migrate folios, err: %d\n", err);
	}
	if (smap_pgtype == HUGE_PAGE) {
		nr_succeeded >>= (__builtin_ctz(g_pagesize_huge) - PAGE_SHIFT);
	}
	mig_time = calc_time_us(start_time);
	pr_debug(
		"migration time spend: %lldus, nr_folios: %u, nr_succeeded: %d\n",
		mig_time, nr_folios, nr_succeeded);
	if (err == 0 && nr_succeeded == 0 && smap_pgtype == NORMAL_PAGE) {
		if (folio_try_get(folios[0])) {
			shake_page(&folios[0]->page);
			folio_put(folios[0]);
		}
	}
	return nr_folios - nr_succeeded;
}

static inline void refresh_nodes_nr_free(void)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(migrate_node.nr); i++) {
		if (i < nr_local_numa) {
			migrate_node.nr[i] = get_node_nr_free_pages(i);
		} else {
			migrate_node.nr[i] = 0;
		}
	}
}

/*
 * find_node_to_migrate_rr - round-robin to find node to migrate
 *
 * @nid:	The node id we want to migrate, -1 means no such node.
 * @out_nid:	The node id we found
 *
 * Returns 0 if we found, or an error code.
 */
static int find_node_to_migrate_rr(int nid, int *out_nid)
{
	int i, temp_nid;

	if (nid >= 0) {
		if (nid >= nr_local_numa) {
			return -EINVAL;
		}
		if (migrate_node.nr[nid] > NR_PAGE_LOW_WMARK) {
			*out_nid = nid;
			return 0;
		}
	}
	i = temp_nid = migrate_node.next_nid;
	migrate_node.next_nid = (migrate_node.next_nid + 1) % nr_local_numa;
	do {
		if (migrate_node.nr[i] > NR_PAGE_LOW_WMARK) {
			*out_nid = i;
			return 0;
		}
		i = (i + 1) % nr_local_numa;
	} while (i != temp_nid);
	return -ENOMEM;
}

static int smap_add_page_for_migrate_back(u64 pa,
					  struct folio ***migrate_folios,
					  unsigned int *mig_pages_cnt,
					  int dest_nid, bool migrate_all)
{
	int ret;
	int nid = NUMA_NO_NODE;
	unsigned long pfn;
	struct page *page;
	struct page_task_arg pta = { 0 };

	pfn = PHYS_PFN(pa);
	if (!pfn_valid(pfn)) {
		return -ENXIO;
	}
	page = pfn_to_online_page(pfn);
	if (!page) {
		return -EIO;
	}

	if (IS_ERR(page)) {
		return PTR_ERR(page);
	}

	if (page_mapcount(page) > 1 && !migrate_all) {
		return -EACCES;
	}
	ret = 0;
	pta.type = PAGE_NODE_TYPE;
	if (PageHuge(page)) {
		if (PageHead(page)) {
			find_page_task(page, 0, &pta);
			if (pta.found &&
			    pta.nr_cpus_allowed < num_online_cpus()) {
				nid = pta.node;
			} else {
				ret = find_node_to_migrate_rr(dest_nid, &nid);
			}
		}
	} else {
		struct page *head = compound_head(page);
		if (__folio_test_movable(page_folio(head))) {
			return -EINVAL;
		}

		find_page_task(head, 0, &pta);
		if (pta.found && pta.nr_cpus_allowed < num_online_cpus()) {
			nid = pta.node;
		} else {
			ret = find_node_to_migrate_rr(dest_nid, &nid);
		}
	}
	if (ret)
		return ret;

	if (nid < 0 || nid >= nr_local_numa) {
		pr_err("invalid local NUMA node: %d passed to add pages for migrate back\n",
		       nid);
		return -EINVAL;
	}
	if (!folio_try_get(page_folio(page))) {
		pr_debug("unable to add folio reference\n");
		return -EINVAL;
	}
	migrate_folios[nid][mig_pages_cnt[nid]] = page_folio(page);
	mig_pages_cnt[nid]++;

	return 0;
}

static bool check_addr_range_valid(struct migrate_back_subtask *task)
{
	__u64 pa;
	unsigned long tmp_pfn;
	struct page *tmp_page;

	for (pa = task->pa_start; pa < task->pa_end; pa += g_pagesize_huge) {
		tmp_pfn = PHYS_PFN(pa);
		if (!pfn_valid(tmp_pfn)) {
			return false;
		}
		tmp_page = pfn_to_online_page(tmp_pfn);
		if (!tmp_page) {
			return false;
		}
		if (get_pageblock_migratetype(tmp_page) == MIGRATE_ISOLATE)
			return false;
	}
	return true;
}

void smap_handle_migrate_back_subtask(struct migrate_back_subtask *task)
{
	int ret;
	unsigned int nr_pre_migrate_fail, nr_migrate_fail;
	__u64 pa;
	unsigned long pfn;
	struct page *page;
	unsigned int page_size = is_smap_pg_huge() ? g_pagesize_huge
						   : PAGE_SIZE;
#ifdef DEBUG
	ktime_t start_time, end_time;
	s64 delta_time_ms;
	unsigned int nr_folios_backup = 0;
#endif

	if (!check_addr_range_valid(task)) {
		pr_err("MIGRATE_ISOLATE pages in range\n");
		task->status = MB_SUBTASK_ERR;
		task->isolated_flag = true;
		return;
	}
	task->isolated_flag = false;

	refresh_nodes_nr_free();

	nr_pre_migrate_fail = nr_migrate_fail = 0;
	unsigned int nr_folios = 0;
	unsigned int nr_folios_min = 0;
	unsigned int cnt = 0;
	unsigned long max_nr_folios =
		(task->pa_end - task->pa_start + 1) / page_size;
	struct folio **migrate_folios =
		vzalloc(max_nr_folios * sizeof(struct folio *));
	if (!migrate_folios) {
		task->status = MB_SUBTASK_ERR;
		return;
	}

	for (pa = task->pa_start; pa < task->pa_end; pa += page_size) {
		pfn = PHYS_PFN(pa);
		if (!pfn_valid(pfn)) {
			continue;
		}
		page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		if (!is_smap_pg_huge()) {
			if (!(!__folio_test_movable(page_folio(page)) &&
			      page_ref_count(page) != 0)) {
				continue;
			}
			if (PageTransHuge(page) || PageHuge(page)) {
				continue;
			}
		}
		if (is_migrate_isolate_page(page)) {
			continue;
		}
		if (is_smap_pg_huge() && PageHuge(page) && !PageHead(page)) {
			continue;
		}
		ret = smap_add_page_for_migration(
			page, migrate_folios, &nr_folios, 0, MPOL_MF_MOVE_ALL);
		if (ret) {
			nr_pre_migrate_fail++;
		}
	}
	if (nr_folios == 0) {
		task->status = MB_SUBTASK_DONE;
		vfree(migrate_folios);
		return;
	}

#ifdef DEBUG
	start_time = ktime_get();
	nr_folios_backup = nr_folios;
#endif
	do {
		if (node_is_critical_err(task->src_nid)) {
			pr_err_ratelimited("critical error on node %d\n",
					   task->src_nid);
			break;
		}
		nr_folios_min = MIN(nr_folios, NR_BATCHED_MIGRATION);
		nr_migrate_fail += smap_migrate(
			&migrate_folios[cnt * NR_BATCHED_MIGRATION],
			nr_folios_min, task->src_nid, MIGRATE_TYPE_BACK);
		nr_folios -= nr_folios_min;
		cnt++;
	} while (nr_folios != 0);

#ifdef DEBUG
	end_time = ktime_get();
	delta_time_ms = ktime_to_ms(ktime_sub(end_time, start_time));
	pr_debug("migrate back total %lu pages, use %lldms\n", nr_folios_backup,
		 delta_time_ms);
#endif
	vfree(migrate_folios);
	if (nr_migrate_fail) {
		task->status = MB_SUBTASK_ERR;
		pr_err("migrate to node%d failed %d pages\n", task->src_nid,
		       nr_migrate_fail);
	}
	task->status =
		(nr_migrate_fail || (!is_smap_pg_huge() && nr_pre_migrate_fail))
			? MB_SUBTASK_ERR
			: MB_SUBTASK_DONE;
}

static void process_pages_for_migration(struct migrate_back_subtask *task,
					struct folio ***migrate_folios,
					unsigned int *mig_pages_cnt,
					unsigned long *nr_pre_migrate_fail,
					unsigned long *nr_pre_migrate)
{
	int ret;
	__u64 pa;
	unsigned long pfn;
	struct page *page;
	*nr_pre_migrate_fail = *nr_pre_migrate = 0;

	for (pa = task->pa_start; pa < task->pa_end; pa += PAGE_SIZE) {
		pfn = PHYS_PFN(pa);
		if (!pfn_valid(pfn)) {
			continue;
		}
		page = pfn_to_online_page(pfn);
		if (!page) {
			continue;
		}
		if (__folio_test_movable(page_folio(page)) ||
		    page_ref_count(page) == 0 || PageTransHuge(page) ||
		    PageHuge(page)) {
			continue;
		}
		if (is_migrate_isolate_page(page)) {
			continue;
		}
		ret = smap_add_page_for_migrate_back(pa, migrate_folios,
						     mig_pages_cnt,
						     task->dest_nid,
						     MPOL_MF_MOVE_ALL);
		if (ret) {
			(*nr_pre_migrate_fail)++;
		} else {
			(*nr_pre_migrate)++;
		}
	}
}

void smap_handle_migrate_back_subtask_4k(struct migrate_back_subtask *task)
{
	int i, j, cnt;
	unsigned int nr_migrate_fail, nr_fail, nr_folios_min;
	unsigned int mig_pages_cnt[SMAP_MAX_LOCAL_NUMNODES] = { 0 };
	struct folio **migrate_folios[SMAP_MAX_LOCAL_NUMNODES] = { NULL };
	unsigned long nr_pre_migrate_fail;
	unsigned long max_nr_folios =
		(task->pa_end - task->pa_start) / PAGE_SIZE;
	unsigned long nr_pre_migrate = 0;
#ifdef DEBUG
	ktime_t start_time, end_time;
	s64 delta_time_ms;
#endif
	for (i = 0; i < SMAP_MAX_LOCAL_NUMNODES; i++) {
		migrate_folios[i] =
			vzalloc(max_nr_folios * sizeof(struct folio *));
		if (!migrate_folios[i]) {
			for (j = 0; j < i; j++) {
				vfree(migrate_folios[j]);
			}
			task->status = MB_SUBTASK_ERR;
			pr_err("unable to allocate memory for migrate folio list\n");
			return;
		}
	}
	refresh_nodes_nr_free();
	nr_pre_migrate_fail = nr_migrate_fail = 0;
	process_pages_for_migration(task, migrate_folios, mig_pages_cnt,
				    &nr_pre_migrate_fail, &nr_pre_migrate);
	for (i = 0; i < SMAP_MAX_LOCAL_NUMNODES; i++) {
		if (mig_pages_cnt[i] == 0) {
			vfree(migrate_folios[i]);
			continue;
		}

#ifdef DEBUG
		start_time = ktime_get();
#endif
		cnt = 0;
		nr_fail = 0;
		do {
			if (node_is_critical_err(i)) {
				pr_err_ratelimited(
					"critical error on node %d\n", i);
				break;
			}
			nr_folios_min =
				MIN(mig_pages_cnt[i], NR_BATCHED_MIGRATION);
			nr_fail += smap_migrate(
				&migrate_folios[i][cnt * NR_BATCHED_MIGRATION],
				nr_folios_min, i, MIGRATE_TYPE_BACK);
			mig_pages_cnt[i] -= nr_folios_min;
			cnt++;
		} while (mig_pages_cnt[i] != 0);
#ifdef DEBUG
		end_time = ktime_get();
		delta_time_ms = ktime_to_ms(ktime_sub(end_time, start_time));
		pr_debug("migrate back total %lu pages, use %lldms\n",
			 nr_pre_migrate, delta_time_ms);
#endif
		if (nr_fail) {
			task->status = MB_SUBTASK_ERR;
			pr_err("migrate to node: %d failed %d pages\n", i,
			       nr_fail);
			nr_migrate_fail += nr_fail;
		}
		vfree(migrate_folios[i]);
	}
	task->status = (nr_migrate_fail || nr_pre_migrate_fail)
			       ? MB_SUBTASK_ERR
			       : MB_SUBTASK_DONE;
}

int is_filter_4k(struct page *page, int page_size)
{
	if (page_size == PAGE_SIZE) {
		if (PageTransHuge(page)) {
			return PAGE_TYPE_TRANSHUGE;
		}
		if (PageHuge(page)) {
			return PAGE_TYPE_HUGE;
		}
		if (__folio_test_movable(page_folio(page))) {
			return PAGE_TYPE_NOR_LRU;
		}
		if (page_ref_count(page) == 0) {
			return PAGE_TYPE_ZERO_REF;
		}
	}
	return -1;
}

static inline bool is_filter_anon(struct page *page)
{
	if (PageHuge(page)) {
		return false;
	}
	return !PageAnon(page) || page_mapcount(page) > 1;
}

/*
 * struct mig_batch - per-batch mutable state for one NR_BATCHED_MIGRATION slice.
 *
 * Holds the folio array being filled and the counters that are reset each
 * batch. Cross-batch/cross-entry accumulators live in struct mig_stats.
 */
struct mig_batch {
	struct folio **folios;
	unsigned int nr_folios;
	unsigned int cnt;
	unsigned int tmp_pre_nr;
	unsigned int pre_failed;
	int pre_err;
};

/*
 * struct mig_stats - cross-entry migration statistics accumulated over one ioctl.
 *
 * Throttled down from do_migrate through migrate_pid_interleaved/
 * migrate_one_batch/collect_one_folio as a single pointer in place of four
 * counter pointers. pre_migrate_num accumulates across one do_migrate call
 * (debug logging only).
 */
struct mig_stats {
	unsigned int mig_num;
	unsigned int non_anon_num;
	unsigned int failed_num;
	unsigned int pre_migrate_num;
	size_t nr_abnormal[NR_ABNORMAL];
};

/*
 * collect_one_folio - filter and isolate a single candidate folio.
 *
 * Counts every visited folio into stats->mig_num, then skips invalid /
 * non-anon / 4k pages; otherwise queues the folio for migration. On success
 * stats->pre_migrate_num and batch->tmp_pre_nr are bumped; on isolation failure
 * batch->pre_failed and batch->pre_err record it.
 */
static void collect_one_folio(int idx, struct mig_list *mig_list,
			      struct migrate_msg *msg, u64 j,
			      struct mig_batch *batch, struct mig_stats *stats)
{
	struct page *p_page;
	unsigned long pfn;
	u64 p_addr = mig_list[idx].addr[j];
	int err, flag;

	stats->mig_num++;
	if (p_addr == INVALID_PADDR)
		return;

	pfn = PHYS_PFN(p_addr);
	if (!pfn_valid(pfn)) {
		pr_debug("invalid PA\n");
		return;
	}

	p_page = pfn_to_online_page(pfn);
	if (!p_page)
		return;

	if (mig_list[idx].from < nr_local_numa && is_filter_anon(p_page)) {
		stats->non_anon_num++;
		return;
	}

	err = is_filter_4k(p_page, msg->page_size);
	if (err >= 0) {
		stats->nr_abnormal[err]++;
		return;
	}

	flag = smap_add_page_for_migration(p_page, batch->folios,
					   &batch->nr_folios, mig_list[idx].pid,
					   MPOL_MF_MOVE_ALL);
	if (!flag) {
		stats->pre_migrate_num++;
		batch->tmp_pre_nr++;
	} else {
		batch->pre_failed++;
		batch->pre_err = flag;
	}
}

/*
 * migrate_one_batch - collect and migrate one folio batch of an entry.
 *
 * Walks up to NR_BATCHED_MIGRATION folios starting at the offset implied by
 * nr_remain via collect_one_folio, then invokes smap_migrate(). Returns the
 * remaining folio count (0 when the entry is done), -ENOMEM if the folio array
 * cannot be allocated, or -EINVAL on a critical node error.
 */
static int migrate_one_batch(int idx, struct migrate_msg *msg,
			     struct mig_list *mig_list, u64 nr_remain,
			     struct mig_stats *stats)
{
	struct mig_batch batch = { 0 };
	u64 j, folio_index, nr_this_migrate;
	unsigned int batch_failed;

	if (node_is_critical_err(mig_list[idx].from) ||
	    node_is_critical_err(mig_list[idx].to)) {
		pr_warn_ratelimited("[%d] critical error node from %d to %d\n",
				    idx, mig_list[idx].from, mig_list[idx].to);
		return -EINVAL;
	}

	nr_this_migrate = MIN(nr_remain, NR_BATCHED_MIGRATION);
	folio_index = mig_list[idx].nr - nr_remain;
	batch.folios = vzalloc(nr_this_migrate * sizeof(struct folio *));
	if (!batch.folios)
		return -ENOMEM;

	for (j = folio_index;
	     j < mig_list[idx].nr && batch.cnt < nr_this_migrate;
	     j++, batch.cnt++)
		collect_one_folio(idx, mig_list, msg, j, &batch, stats);

	if (batch.pre_failed)
		pr_warn("pre_migrate fail %u pages, ret: %d\n",
			batch.pre_failed, batch.pre_err);

	mig_list[idx].failed_pre_migrated_nr +=
		nr_this_migrate - batch.tmp_pre_nr;
	if (batch.nr_folios == 0) {
		pr_debug("no page to migrate\n");
		vfree(batch.folios);
		return nr_remain - batch.cnt;
	}

	pr_debug("migrate_one_batch: [%d] pid %d from %d to %d nr %u\n", idx,
		 mig_list[idx].pid, mig_list[idx].from, mig_list[idx].to,
		 batch.cnt);
	batch_failed = smap_migrate(batch.folios, batch.nr_folios,
				    mig_list[idx].to, MIGRATE_TYPE_HOTNESS);
	mig_list[idx].failed_mig_nr += batch_failed;
	stats->failed_num += batch_failed;
	mig_list[idx].success_to_user = true;

	if (mig_list[idx].failed_mig_nr)
		pr_debug(
			"[%d]: migrate failed, pre_migrate_num: %d, failed_num: %llu\n",
			idx, stats->pre_migrate_num,
			mig_list[idx].failed_mig_nr);

	pr_debug("[%d]: mig_num %d, pre_migrate_num %d, failed_num %llu\n", idx,
		 stats->mig_num, stats->pre_migrate_num,
		 mig_list[idx].failed_mig_nr);

	vfree(batch.folios);
	return nr_remain - batch.cnt;
}

/*
 * init_mig_entry - validate and initialize one mig_list entry before its
 * first batch. Resets the per-entry failure counters and seeds the entry's
 * remaining count in the caller's nr_remain array. Returns 0 on success or
 * -EINVAL if the entry is invalid (caller marks it done and skips it).
 */
static int init_mig_entry(int idx, struct mig_list *mig_list, u64 *nr_remain)
{
	pr_debug("[%d] pid %d from %d to %d nr %llu\n", idx, mig_list[idx].pid,
		 mig_list[idx].from, mig_list[idx].to, mig_list[idx].nr);

	if (is_node_invalid(mig_list[idx].from) ||
	    is_node_invalid(mig_list[idx].to)) {
		pr_warn_ratelimited("[%d] invalid node from %d to %d\n", idx,
				    mig_list[idx].from, mig_list[idx].to);
		return -EINVAL;
	}

	if (mig_list[idx].nr <= 0 || mig_list[idx].nr > MAX_MIG_LIST_NR) {
		pr_warn_ratelimited("[%d] invalid nr %llu\n", idx,
				    mig_list[idx].nr);
		return -EINVAL;
	}

	mig_list[idx].failed_pre_migrated_nr = 0;
	mig_list[idx].failed_mig_nr = 0;
	nr_remain[idx] = mig_list[idx].nr;
	return 0;
}

/*
 * run_one_batch - run one migrate_one_batch for entry i and update its
 * remaining count. Returns 0 after a step (entry advanced, finished, or
 * skipped due to -EINVAL), or a negative errno to abort the whole ioctl.
 * nr_remain[i] == 0 after this call means the entry is done.
 */
static int run_one_batch(int i, struct migrate_msg *msg,
			 struct mig_list *mig_list, u64 *nr_remain,
			 struct mig_stats *stats)
{
	int r = migrate_one_batch(i, msg, mig_list, nr_remain[i], stats);

	if (r < 0 && r != -EINVAL)
		return r;
	if (r == -EINVAL || r == 0)
		nr_remain[i] = 0;
	else
		nr_remain[i] = r;
	return 0;
}

/*
 * migrate_pid_interleaved - drive the interleaved cold/hot swap for one pid.
 *
 * Entries of this pid are already initialized by do_migrate (nr_remain > 0 for
 * active, 0 for done/skipped). Drains the back (promotion, from a remote node)
 * and out (demotion, from a local node) directions one NR_BATCHED_MIGRATION
 * batch at a time in alternation: one back batch, one out batch, ... until
 * neither direction has work left. Each pass picks the first still-active
 * entry of the requested direction. Returns 0 on completion, or a negative
 * errno to abort the whole ioctl.
 */
static int migrate_pid_interleaved(int index, struct migrate_msg *msg,
				   struct mig_list *mig_list, u64 *nr_remain,
				   struct mig_stats *stats)
{
	pid_t cur_pid = mig_list[index].pid;
	bool progressed;
	int i;

	do {
		int ret;

		progressed = false;

		/* one back (promotion) batch: first active remote-source entry */
		for (i = index; i < msg->cnt; i++) {
			if (mig_list[i].pid != cur_pid || nr_remain[i] == 0 ||
			    mig_list[i].from < nr_local_numa)
				continue;
			ret = run_one_batch(i, msg, mig_list, nr_remain, stats);
			if (ret)
				return ret;
			progressed = true;
			break;
		}

		/* one out (demotion) batch: first active local-source entry */
		for (i = index; i < msg->cnt; i++) {
			if (mig_list[i].pid != cur_pid || nr_remain[i] == 0 ||
			    mig_list[i].from >= nr_local_numa)
				continue;
			ret = run_one_batch(i, msg, mig_list, nr_remain, stats);
			if (ret)
				return ret;
			progressed = true;
			break;
		}
	} while (progressed);

	return 0;
}

/*
 * do_migrate - drive the cold/hot swap for a batch of mig_list entries.
 *
 * Pre-inits every mig_list entry (valid entries get nr_remain = nr > 0,
 * invalid ones are left at 0 = skip), then groups entries per pid (visited
 * in order of first appearance) and hands each pid to
 * migrate_pid_interleaved, which alternates migrate-back and migrate-out one
 * NR_BATCHED_MIGRATION batch at a time instead of draining one direction
 * fully before starting the other. Entry state lives solely in nr_remain:
 * 0 means done/skipped, > 0 means still active.
 */
int do_migrate(struct migrate_msg *msg, struct mig_list *mig_list)
{
	int i, index, ret;
	struct mig_stats stats = { 0 };
	u64 *nr_remain;

	if (msg->cnt == 0)
		return 0;

	nr_remain = vzalloc(msg->cnt * sizeof(*nr_remain));
	if (!nr_remain)
		return -ENOMEM;

	for (i = 0; i < msg->cnt; i++)
		(void)init_mig_entry(i, mig_list, nr_remain);

	for (index = 0; index < msg->cnt; index++) {
		if (nr_remain[index] == 0)
			continue;
		ret = migrate_pid_interleaved(index, msg, mig_list, nr_remain,
					      &stats);
		if (ret) {
			vfree(nr_remain);
			return ret;
		}
	}

	vfree(nr_remain);
	pr_debug("non anon page number: %u\n", stats.non_anon_num);
	filter_4k_migrate_info(stats.nr_abnormal);
	return stats.failed_num;
}

static int smap_pre_migrate_range(struct folio **folios,
				  unsigned int *nr_folios,
				  unsigned long start_pfn,
				  unsigned long end_pfn)
{
	unsigned long pfn;
	struct page *page, *head;
	int nr_hugepage = 0;
	int nr_normalpage = 0;
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		struct folio *folio;

		page = pfn_to_online_page(pfn);
		if (!page) {
			continue;
		}
		folio = page_folio(page);
		head = &folio->page;
		if (is_smap_pg_huge()) {
			if (PageHuge(page) && !PageHead(page)) {
				continue;
			}
			if (!folio_ref_count(folio)) {
				pr_debug("folio ret count is 0\n");
				continue;
			}
			nr_hugepage++;
		} else {
			if (__folio_test_movable(page_folio(page)) ||
			    page_ref_count(page) == 0 || PageTransHuge(page) ||
			    PageHuge(page)) {
				continue;
			}
			if (!folio_try_get(folio)) {
				pr_debug("unable to add folio reference\n");
				continue;
			}
			nr_normalpage++;
		}
		folios[*nr_folios] = folio;
		(*nr_folios)++;
	}
	pr_info("pre migrate: %d huge page, %d base page\n", nr_hugepage,
		nr_normalpage);
	return nr_hugepage + nr_normalpage;
}

static unsigned int smap_migrate_range(int nid, u64 start_pa, u64 end_pa)
{
	int nr_pre_migrate_cnt;
	int cnt = 0;
	unsigned nr_migrate_fail = 0;
	unsigned long start_pfn = PHYS_PFN(start_pa);
	unsigned long end_pfn = PHYS_PFN(end_pa);
	unsigned int nr_folios = 0;
	unsigned int nr_folios_min = 0;
	struct folio **migrate_folios;

	if (!pfn_valid(start_pfn) || !pfn_valid(end_pfn)) {
		pr_err("invalid pfn passed to migrate range\n");
		return -EINVAL;
	}
	if (start_pfn >= end_pfn ||
	    (end_pfn - start_pfn + 1) > MAX_MIG_LIST_NR) {
		pr_err("invalid pfn passed to migrate range\n");
		return -EINVAL;
	}
	migrate_folios =
		vzalloc((end_pfn - start_pfn + 1) * sizeof(struct folio *));
	if (!migrate_folios) {
		pr_err("unable to allocate memory for migrate folio list\n");
		return -ENOMEM;
	}
	nr_pre_migrate_cnt = smap_pre_migrate_range(migrate_folios, &nr_folios,
						    start_pfn, end_pfn);
	do {
		nr_folios_min = MIN(nr_pre_migrate_cnt, NR_BATCHED_MIGRATION);
		nr_migrate_fail += smap_migrate(
			&migrate_folios[cnt * NR_BATCHED_MIGRATION],
			nr_folios_min, nid, MIGRATE_TYPE_REMOTE);
		if (nr_migrate_fail) {
			pr_err("migrate pre_migrate cnt: %d, mig failed %d pages in pfn range %#lx-%#lx\n",
			       nr_folios_min, nr_migrate_fail, start_pfn,
			       end_pfn);
			vfree(migrate_folios);
			return nr_migrate_fail;
		}
		nr_pre_migrate_cnt -= nr_folios_min;
		cnt++;
	} while (nr_pre_migrate_cnt != 0);
	vfree(migrate_folios);
	return nr_migrate_fail;
}

unsigned int smap_migrate_numa(struct migrate_numa_inner_msg *msg)
{
	unsigned int ret = 0;
	int i;
	int nid = msg->dest_nid;

	if (node_is_critical_err(msg->src_nid) ||
	    node_is_critical_err(msg->dest_nid)) {
		pr_err_ratelimited("critical error on node %d or %d\n",
				   msg->src_nid, msg->dest_nid);
		return -EINVAL;
	}
	for (i = 0; i < msg->count; i++) {
		int retry = MAX_MIGRATE_NUMA_RETRY_TIME;
		u64 start_pa = msg->range[i].pa_start;
		u64 end_pa = msg->range[i].pa_end;
		do {
			ret = smap_migrate_range(nid, start_pa, end_pa);
			if (ret == 0)
				break;
			pr_info("migrate range to %d failed %d pages\n", nid,
				ret);
			if (node_is_critical_err(msg->src_nid) ||
			    node_is_critical_err(msg->dest_nid)) {
				pr_err_ratelimited(
					"critical error on node %d or %d\n",
					msg->src_nid, msg->dest_nid);
				return -EINVAL;
			}
		} while (retry--);
		if (retry == 0)
			return ret;
	}
	return ret;
}

struct folio *alloc_demote_page(struct folio *folio, unsigned long node)
{
	unsigned int order = 0;
	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'folio' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE,
		.nid = node,
	};

	/*
	 * 目标节点为远端节点(node >= nr_local_numa, 即远端内存 tier)时,
	 * 允许跳过水线检测, 保证内存压力下换出仍能成功; 本地节点分配则
	 * 走正常水线回收逻辑。
	 */
	if (node >= nr_local_numa)
		mtc.gfp_mask |= __GFP_MEMALLOC;

	return __folio_alloc(mtc.gfp_mask, order, mtc.nid, mtc.nmask);
}

static bool check_subtask_range(struct migrate_back_task *task,
				unsigned long pfn)
{
	struct migrate_back_subtask *subtask;
	list_for_each_entry(subtask, &task->subtask, task_list) {
		unsigned long start_pfn = PHYS_PFN(subtask->pa_start);
		unsigned long end_pfn = PHYS_PFN(subtask->pa_end);
		if (pfn >= start_pfn && pfn <= end_pfn)
			return true;
	}
	return false;
}

bool is_folio_in_migrate_back_range(struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	struct migrate_back_task *task;
	spin_lock(&migrate_back_task_lock);
	list_for_each_entry(task, &migrate_back_task_list, task_node) {
		if (task->status != MB_TASK_WAITING) {
			continue;
		}

		if (check_subtask_range(task, pfn)) {
			spin_unlock(&migrate_back_task_lock);
			return true;
		}
	}
	spin_unlock(&migrate_back_task_lock);
	return false;
}

static inline bool mig_back_filter(struct folio *folio)
{
	return is_folio_in_migrate_back_range(folio);
}

struct folio *smap_alloc_huge_page_node(struct folio *folio, int nid,
					bool is_mig_back)
{
	nodemask_t nodemask;
	unsigned long size = folio_size(folio);
	gfp_t gfp_mask = GFP_HIGHUSER_MOVABLE;
	if (nid != NUMA_NO_NODE)
		gfp_mask |= __GFP_THISNODE;

	nodes_clear(nodemask);
	node_set(nid, nodemask);

	if (is_mig_back) {
		return get_hugetlb_folio_nodemask(size, nid, &nodemask,
						  gfp_mask, mig_back_filter);
	}
	return get_hugetlb_folio_nodemask(size, nid, &nodemask, gfp_mask, NULL);
}

struct folio *smap_alloc_new_node_page(struct folio *folio, unsigned long node)
{
	if (folio_test_hugetlb(folio)) {
		return smap_alloc_huge_page_node(folio, node, false);
	}
	return alloc_demote_page(folio, node);
}

struct folio *smap_alloc_new_node_page_mig_back(struct folio *folio,
						unsigned long node)
{
	if (folio_test_hugetlb(folio)) {
		return smap_alloc_huge_page_node(folio, node, true);
	}
	return alloc_demote_page(folio, node);
}
