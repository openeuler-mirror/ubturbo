// SPDX-License-Identifier: GPL-2.0-only
/*
 */
#include <linux/mm.h>
#include <linux/idr.h>
#include <linux/mmzone.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/list.h>
#include <linux/page_ref.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/migrate_mode.h>
#include "ucache.h"
#include "ucache_migrate.h"

#define NR_PAGES_TO_MIGRATE 10240

static DEFINE_IDR(nid_success_idr);

/*
 * Pin the memcg of pid: the returned memcg holds a css reference that the
 * caller must drop with mem_cgroup_put(). The lruvec and its LRU lists stay
 * valid while the reference is held.
 */
static struct mem_cgroup *get_memcg_by_pid(pid_t pid)
{
	struct task_struct *task;
	struct mem_cgroup *memcg = NULL;

	rcu_read_lock();
	task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (!task) {
		pr_err("get task_struct from pid %d failed\n", pid);
		goto out;
	}

	memcg = mem_cgroup_from_task(task);
	if (!memcg || !css_tryget_online(&memcg->css)) {
		pr_err("get memcg from task failed\n");
		memcg = NULL;
	}

out:
	rcu_read_unlock();
	return memcg;
}

static bool folio_should_skip(struct folio *folio)
{
	if (folio_test_dirty(folio))
		return true;
	if (folio_test_hugetlb(folio))
		return true;
	if (folio_mapped(folio))
		return true;
	return false;
}

int ucache_scan_folios(int nid, pid_t pid, struct folio **folios,
		       unsigned int *nr_folios)
{
	struct mem_cgroup *memcg = NULL;
	struct lruvec *lruvec = NULL;
	pg_data_t *pgdat = NULL;
	struct list_head *src = NULL;
	unsigned int nr_isolated = 0;
	unsigned int nr_pages = 0;
	struct folio *folio = NULL;

	if (!nr_folios) {
		pr_err("nr_folios == NULL\n");
		return -EPERM;
	}

	memcg = get_memcg_by_pid(pid);
	if (!memcg) {
		pr_err("get memcg from pid %d failed\n", pid);
		return -EPERM;
	}

	pgdat = NODE_DATA(nid);
	if (!pgdat) {
		pr_err("get NODE_DATA from nid failed, nid=%d\n", nid);
		mem_cgroup_put(memcg);
		return -EPERM;
	}

	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	src = &lruvec->lists[LRU_INACTIVE_FILE];
	pr_info("scan folios from lruvec LRU_INACTIVE_FILE list, pid=%d nid=%d\n",
		pid, nid);

	if (list_empty(src)) {
		pr_warn("lruvec LRU_INACTIVE_FILE list is empty!\n");
		goto out;
	}

	spin_lock_irq(&lruvec->lru_lock);
	list_for_each_entry_reverse(folio, src, lru) {
		if (!folio_try_get(folio)) {
			continue;
		}

		if (folio_should_skip(folio)) {
			folio_put(folio);
			continue;
		}

		folios[nr_isolated] = folio;
		nr_isolated++;
		// check the page number of folio
		nr_pages += (unsigned int)folio_nr_pages(folio);
		if (nr_pages >= NR_PAGES_TO_MIGRATE) {
			break;
		}
		// check the number of folios
		if (nr_isolated >= *nr_folios) {
			break;
		}
	}
	spin_unlock_irq(&lruvec->lru_lock);
	pr_info("folio scan: found %u folios comprising %u pages\n",
		nr_isolated, nr_pages);
out:
	*nr_folios = nr_isolated;
	mem_cgroup_put(memcg);
	return 0;
}

static void create_migrate_success(int nid, unsigned int nr_folios,
				   unsigned int nr_succeeded)
{
	struct migrate_success *success_rate = NULL;
	success_rate = kzalloc(sizeof(struct migrate_success), GFP_KERNEL);
	if (success_rate == NULL) {
		pr_err("Failed to kzalloc migrate_success\n");
		return;
	}
	success_rate->des_nid = nid;
	success_rate->nr_folios = nr_folios;
	success_rate->nr_succeeded = nr_succeeded;
	if (idr_alloc(&nid_success_idr, success_rate, nid, nid + 1,
		      GFP_KERNEL) < 0) {
		kfree(success_rate);
		pr_err("idr alloc for migrate success failed\n");
	}
}

static void set_migrate_success_rate(int des_nid, unsigned int nr_folios,
				     unsigned int nr_succeeded)
{
	// The migration success rate needs to be recorded only for outgoing migration.
	struct migrate_success *success_rate = NULL;
	success_rate = idr_find(&nid_success_idr, des_nid);
	if (success_rate == NULL) {
		create_migrate_success(des_nid, nr_folios, nr_succeeded);
	} else {
		success_rate->nr_folios = nr_folios;
		success_rate->nr_succeeded = nr_succeeded;
	}
}

struct migration_target_control {
	int nid; /* preferred node id */
	nodemask_t *nmask;
	gfp_t gfp_mask;
};

struct folio *alloc_ucache_page(struct folio *folio, unsigned long node)
{
	unsigned int order = 0;
	struct migration_target_control mtc = {
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, .nid = node
	};

	if (folio_test_large(folio)) {
		/*
		 * clear __GFP_RECLAIM to make the migration callback
		 * consistent with regular THP allocations.
		 */
		mtc.gfp_mask &= ~__GFP_RECLAIM;
		mtc.gfp_mask |= GFP_TRANSHUGE;
		order = folio_order(folio);
	}

	return __folio_alloc(mtc.gfp_mask, order, mtc.nid, mtc.nmask);
}

int ucache_migrate_folios(int des_nid, struct folio **folios,
			  unsigned int nr_folios)
{
	int ret = 0;
	unsigned int nr_succeeded = 0;

	if (folios == NULL) {
		pr_err("folios is NULL\n");
		return -EINVAL;
	}

	if (nr_folios == 0) {
		pr_info("nr_folios is 0\n");
		set_migrate_success_rate(des_nid, nr_folios, nr_succeeded);
		return 0;
	}

	ret = isolate_and_migrate_folios(folios, nr_folios, alloc_ucache_page,
					 NULL, des_nid, MIGRATE_ASYNC,
					 &nr_succeeded);
	pr_info("folio migration: %u folios attempted, %u pages migrated\n",
		nr_folios, nr_succeeded);
	set_migrate_success_rate(des_nid, nr_folios, nr_succeeded);
	return ret;
}

struct migrate_success *get_migrate_success(int nid)
{
	return idr_find(&nid_success_idr, nid);
}
