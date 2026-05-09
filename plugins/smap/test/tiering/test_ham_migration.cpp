#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/list.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/pagewalk.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ham_migration.h"
#include "ham_tasks_mgr.h"

using namespace std;

extern "C" int init_numa_page_map(struct ham_migrate_task *mig_task,
    struct migration_param *arg);
extern "C" struct folio *alloc_folio_with_retry(int nid,
    struct ham_migrate_task *mig_task, struct migration_param *param);
extern "C" int create_numa_map(struct ham_migrate_task *mig_task,
    struct migration_param *param);
extern "C" int fill_folios_hugetlb(pte_t *pte, unsigned long hmask,
    unsigned long addr, unsigned long end, struct mm_walk *walk);
extern "C" void sort_hpm_list(struct list_head *head, unsigned int nr_hpm);
extern "C" int handle_ham_migration(struct list_head *hpm_list,
    unsigned int nr_hpm_max, struct folio *(*get_migration_folio)(struct ham_page_map *hpm),
    new_folio_t get_new_folio, free_folio_t put_new_folio);
extern "C" struct folio *get_folio_migrate_out(struct ham_page_map *hpm);
extern "C" struct folio *get_folio_migrate_back(struct ham_page_map *hpm);
extern "C" struct folio *get_new_folio(struct folio *folio, unsigned long private_data);
extern "C" void put_new_folio(struct folio *folio, unsigned long private_data);
extern "C" struct folio *get_new_folio_rollback(struct folio *folio, unsigned long private_data);
extern "C" void put_new_folio_rollback(struct folio *folio, unsigned long private_data);
extern "C" int ham_cache_clear(pid_t pid, struct ham_migrate_task *mig_task);
extern "C" int src_suspend_pgtable_maintain(struct ham_migrate_task *mig_task);
extern "C" struct folio *ham_alloc_huge_page_node(int nid);
extern "C" int flush_cache_by_pa(unsigned long start, unsigned long size, unsigned int op_type);
extern "C" int task_pgtable_within_pid_set_cacheable(pid_t pid, unsigned long start,
    unsigned long size, bool cacheable);

class HamMigrationTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

static int mock_isolate_success(struct folio **folios, unsigned int nr_folios,
    new_folio_t get_new_folio_cb, free_folio_t put_new_folio_cb,
    unsigned long private_data, enum migrate_mode mode, unsigned int *nr_succeeded)
{
    *nr_succeeded = nr_folios << (PMD_SHIFT - PAGE_SHIFT);
    return 0;
}

TEST_F(HamMigrationTest, InitNumaPageMapRejectsInvalidParams)
{
    struct ham_migrate_task mig_task = {};
    struct migration_param param = {};

    EXPECT_EQ(-ENOMEM, init_numa_page_map(&mig_task, &param));

    mig_task.numa_cnt = BATCH_NUM + 1;
    param.cnt = 1;
    EXPECT_EQ(-ENOMEM, init_numa_page_map(&mig_task, &param));
}

TEST_F(HamMigrationTest, AllocFolioWithRetrySuccessAndFailure)
{
    struct ham_migrate_task mig_task = {};
    struct migration_param param = {};
    struct hstate hstate = {};
    struct folio expected = {};

    mig_task.hstate = &hstate;
    hstate.nr_huge_pages_node[1] = 4;
    hstate.free_huge_pages_node[1] = 2;
    param.cnt = 1;
    param.ram_blocks[0].rmt_numa_id = 1;

    MOCKER(ham_alloc_huge_page_node).stubs()
        .will(returnValue((struct folio *)nullptr))
        .then(returnValue(&expected));
    EXPECT_EQ(&expected, alloc_folio_with_retry(1, &mig_task, &param));

    GlobalMockObject::verify();
    MOCKER(ham_alloc_huge_page_node).stubs().will(returnValue((struct folio *)nullptr));
    EXPECT_EQ(nullptr, alloc_folio_with_retry(1, &mig_task, &param));
}

TEST_F(HamMigrationTest, CreateNumaMapPopulatesSinglePage)
{
    struct ham_migrate_task mig_task = {};
    struct migration_param param = {};
    struct hstate hstate = {};
    struct folio folio = {};

    mig_task.pid = 100;
    mig_task.numa_cnt = 1;
    mig_task.hstate = &hstate;
    mig_task.status = HAM_TASK_ALLOW_MIGR;
    param.pid = 100;
    param.cnt = 1;
    param.ram_blocks[0].rmt_numa_id = 1;
    param.ram_blocks[0].size = PAGE_SIZE_2M;
    param.ram_blocks[0].hva_start = 0x100000;

    MOCKER(ham_alloc_huge_page_node).stubs().will(returnValue(&folio));

    EXPECT_EQ(0, create_numa_map(&mig_task, &param));
    ASSERT_NE(nullptr, mig_task.ram_maps);
    EXPECT_EQ(1U, mig_task.ram_maps[0].page_num);
    EXPECT_EQ(&folio, mig_task.ram_maps[0].hpms[0].dst_folio);

    vfree(mig_task.ram_maps[0].hpms);
    kfree(mig_task.ram_maps);
}

TEST_F(HamMigrationTest, FillFoliosHugetlbCoversBranches)
{
    struct ram_block_map map = {};
    struct mm_walk walk = {};
    pte_t pte = {};

    map.hva_start = 0x100000;
    map.page_num = 1;
    map.rmt_numa_id = 1;
    map.hpms = (struct ham_page_map *)calloc(1, sizeof(struct ham_page_map));
    walk.private_data = &map;

    EXPECT_EQ(-EINVAL, fill_folios_hugetlb(&pte, 0, map.hva_start + PAGE_SIZE_2M,
        map.hva_start + PAGE_SIZE_2M, &walk));

    EXPECT_EQ(0, fill_folios_hugetlb(&pte, 0, map.hva_start, map.hva_start, &walk));

    pte = __pte(1UL << PAGE_SHIFT);
    MOCKER(page_to_nid).stubs().will(returnValue(1));
    EXPECT_EQ(0, fill_folios_hugetlb(&pte, 0, map.hva_start, map.hva_start, &walk));

    GlobalMockObject::verify();
    MOCKER(page_to_nid).stubs().will(returnValue(0));
    EXPECT_EQ(0, fill_folios_hugetlb(&pte, 0, map.hva_start, map.hva_start, &walk));
    EXPECT_TRUE(hpm_test_present(&map.hpms[0]));
    EXPECT_EQ(0, map.hpms[0].src_numa_id);

    free(map.hpms);
}

TEST_F(HamMigrationTest, SortAndMigrateHelpers)
{
    struct list_head hpm_list;
    struct ham_page_map hpm = {};
    struct folio src = {};
    struct folio dst = {};

    INIT_LIST_HEAD(&hpm_list);
    sort_hpm_list(&hpm_list, 0);
    EXPECT_TRUE(list_empty(&hpm_list));

    INIT_LIST_HEAD(&hpm.list);
    hpm.src_folio = &src;
    hpm.dst_folio = &dst;
    hpm.freq = 3;
    hpm_set_present(&hpm);
    list_add_tail(&hpm.list, &hpm_list);

    EXPECT_EQ(&src, get_folio_migrate_out(&hpm));
    EXPECT_EQ(&dst, get_folio_migrate_back(&hpm));
    EXPECT_EQ(&dst, get_new_folio(&src, (unsigned long)&hpm_list));
    EXPECT_TRUE(hpm_test_migrate(&hpm));
    put_new_folio(&dst, (unsigned long)&hpm_list);
    EXPECT_FALSE(hpm_test_migrate(&hpm));
}

TEST_F(HamMigrationTest, HandleMigrationAndRollbackHelpers)
{
    struct list_head hpm_list;
    struct ham_page_map hpm = {};
    struct folio src = {};
    struct folio dst = {};
    struct folio rollback = {};

    INIT_LIST_HEAD(&hpm_list);
    EXPECT_EQ(0, handle_ham_migration(&hpm_list, 1, get_folio_migrate_out,
        get_new_folio, put_new_folio));

    INIT_LIST_HEAD(&hpm.list);
    hpm.src_folio = &src;
    hpm.dst_folio = &dst;
    hpm_set_present(&hpm);
    list_add_tail(&hpm.list, &hpm_list);

    MOCKER(isolate_and_migrate_folios).stubs().will(invoke(mock_isolate_success));
    EXPECT_EQ(0, handle_ham_migration(&hpm_list, 1, get_folio_migrate_out,
        get_new_folio, put_new_folio));

    hpm_set_migrate(&hpm);
    hpm.src_numa_id = 2;
    MOCKER(ham_alloc_huge_page_node).stubs().will(returnValue(&rollback));
    EXPECT_EQ(&rollback, get_new_folio_rollback(&dst, (unsigned long)&hpm_list));
    EXPECT_TRUE(hpm_test_rollback(&hpm));
    put_new_folio_rollback(&rollback, (unsigned long)&hpm_list);
    EXPECT_FALSE(hpm_test_rollback(&hpm));
}

TEST_F(HamMigrationTest, CacheAndPgtableMaintain)
{
    struct ham_migrate_task mig_task = {};
    struct ram_block_map map = {};

    mig_task.pid = 99;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &map;
    map.rmt_numa_start = 0x200000;
    map.hva_start = 0x400000;
    map.size = PAGE_SIZE_2M;

    MOCKER(flush_cache_by_pa).stubs().will(returnValue(0));
    EXPECT_EQ(0, ham_cache_clear(99, &mig_task));

    GlobalMockObject::verify();
    MOCKER(flush_cache_by_pa).stubs().will(returnValue(-1));
    EXPECT_EQ(-1, ham_cache_clear(99, &mig_task));

    GlobalMockObject::verify();
    MOCKER(task_pgtable_within_pid_set_cacheable).stubs().will(returnValue(0));
    EXPECT_EQ(0, src_suspend_pgtable_maintain(&mig_task));
    EXPECT_FALSE(map.cacheable);

    GlobalMockObject::verify();
    map.cacheable = true;
    MOCKER(task_pgtable_within_pid_set_cacheable).stubs().will(returnValue(-1));
    EXPECT_EQ(-1, src_suspend_pgtable_maintain(&mig_task));
}
