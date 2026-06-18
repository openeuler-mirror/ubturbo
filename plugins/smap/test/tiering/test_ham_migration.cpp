#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

/* Forward declare mmput with C linkage before mm.h includes it,
   ensuring mmput uses C linkage (matching its C definition in fork.c). */
struct mm_struct;
extern "C" void mmput(struct mm_struct *);

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
extern "C" int src_pgtable_maintain_rollback(struct ham_migrate_task *mig_task);
extern "C" int dst_resume_pgtable_maintain(struct ham_migrate_task *mig_task);
extern "C" unsigned int queue_qualified_pages(struct ham_migrate_task *mig_task,
    bool migrated, struct list_head *ret_hpm_list);
extern "C" unsigned int construct_page_list(struct list_head *hpm_list,
    struct folio *(*get_migration_folio)(struct ham_page_map *hpm), struct folio **folios);
extern "C" int get_folios_freqs(struct ham_migrate_task *mig_task);
extern "C" long ioctl_start_migration(unsigned long arg);
extern "C" long ioctl_migration_pages(unsigned long arg);
extern "C" long ioctl_rollback_pages(unsigned long arg);
extern "C" long ioctl_stop_migration(unsigned long arg);
extern "C" long ioctl_modify_pagetable(unsigned long arg);
extern "C" long ioctl_cache_clear(unsigned long arg);
extern "C" long ham_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
extern "C" int ham_open(struct inode *inode, struct file *file);
extern "C" int ham_release(struct inode *inode, struct file *file);
extern "C" ssize_t ham_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
extern "C" ssize_t ham_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos);
extern "C" ssize_t global_cache_mnt_show(struct device *dev,
    struct device_attribute *attr, char *buf);
extern "C" ssize_t global_cache_mnt_store(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t count);
extern "C" struct folio *ham_alloc_huge_page_node(int nid);
extern "C" int flush_cache_by_pa(unsigned long start, unsigned long size, unsigned int op_type);
extern "C" int task_pgtable_within_pid_set_cacheable(pid_t pid, unsigned long start,
    unsigned long size, bool cacheable);
extern "C" int kernel_pgtable_within_pid_set_valid(pid_t pid, unsigned long start,
    unsigned long size, bool valid);
extern "C" int set_pid_pgtable_cacheable(pid_t pid, unsigned long start, unsigned long size);
extern "C" int get_ham_pages_freqs(pid_t pid, struct freq_info **freq_info_array,
    uint64_t *freq_info_num);
extern "C" unsigned long copy_from_user(void *to, const void *from, unsigned long n);
extern "C" int config_system_huge_page(unsigned long huge_page_number,
    unsigned int node_id, struct hstate *h);
extern "C" int compare_page_list(const void *a, const void *b);
extern "C" int compare_page_freq(const void *a, const void *b);
extern "C" int fill_task_pages(struct ham_migrate_task *mig_task);
extern "C" int check_migration_param(struct migration_param param, struct hstate **h);
extern "C" bool pid_legally(pid_t pid);
extern "C" int check_rmt_numa_info(struct ram_block_info *rbi);
extern "C" struct ham_migrate_task *init_migrate_task(struct migration_param *arg,
    struct hstate *h);
extern "C" struct file *filp_open(const char *filename, int flags, umode_t mode);
extern "C" int filp_close(struct file *filp, fl_owner_t id);
extern "C" struct mm_struct *find_get_mm_by_vpid(pid_t pid);
extern "C" int walk_page_range(struct mm_struct *mm, unsigned long start,
    unsigned long end, const struct mm_walk_ops *ops, void *private_data);
extern "C" struct vm_area_struct *find_vma(struct mm_struct *mm_s, unsigned long vaddr);
extern "C" struct hstate *hstate_vma(struct vm_area_struct *vma);
extern "C" unsigned long huge_page_size(struct hstate *h);
extern "C" void mmap_read_lock(struct mm_struct *mm);
extern "C" void mmap_read_unlock(struct mm_struct *mm);
extern "C" void msleep(unsigned int msecs);

class HamMigrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[HamMigrationTest SetUp Begin]" << endl;
    }
    void TearDown() override
    {
        cout << "[HamMigrationTest TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[HamMigrationTest TearDown End]" << endl;
    }
};

static int mock_isolate_success(struct folio **folios, unsigned int nr_folios,
    new_folio_t get_new_folio_cb, free_folio_t put_new_folio_cb,
    unsigned long private_data, enum migrate_mode mode, unsigned int *nr_succeeded)
{
    *nr_succeeded = nr_folios << (PMD_SHIFT - PAGE_SHIFT);
    return 0;
}

static struct folio *mock_get_null_folio(struct ham_page_map *hpm)
{
    (void)hpm;
    return nullptr;
}

static unsigned long mock_copy_from_user_set_param(void *to, const void *from,
    unsigned long n)
{
    if (n == sizeof(struct migration_param)) {
        struct migration_param *p = (struct migration_param *)to;
        memset(p, 0, sizeof(struct migration_param));
        p->pid = 100;
        p->cnt = 1;
        p->ram_blocks[0].rmt_numa_id = NUMA_NO_NODE;
        p->ram_blocks[0].size = PAGE_SIZE_2M;
        p->ram_blocks[0].hva_start = 0x100000;
    } else if (n == sizeof(pid_t)) {
        pid_t *pid_ptr = (pid_t *)to;
        *pid_ptr = 100;
    } else if (n == sizeof(maintain_info)) {
        maintain_info *mi = (maintain_info *)to;
        memset(mi, 0, sizeof(maintain_info));
        mi->pid = 100;
        mi->cacheable = true;
    }
    return 0;
}

static unsigned long mock_copy_from_user_remote_node(void *to, const void *from,
    unsigned long n)
{
    if (n == sizeof(struct migration_param)) {
        struct migration_param *p = (struct migration_param *)to;
        memset(p, 0, sizeof(struct migration_param));
        p->pid = 100;
        p->cnt = 1;
        p->ram_blocks[0].rmt_numa_id = 1;
        p->ram_blocks[0].size = PAGE_SIZE_2M;
        p->ram_blocks[0].hva_start = 0x100000;
    }
    return 0;
}

static unsigned long mock_copy_from_user_non_cacheable(void *to, const void *from,
    unsigned long n)
{
    if (n == sizeof(maintain_info)) {
        maintain_info *mi = (maintain_info *)to;
        memset(mi, 0, sizeof(maintain_info));
        mi->pid = 100;
        mi->cacheable = false;
    }
    return 0;
}

static struct ham_migrate_task g_mock_mig_task;

static struct ham_migrate_task *mock_init_migrate_task_success(
    struct migration_param *arg, struct hstate *h)
{
    memset(&g_mock_mig_task, 0, sizeof(struct ham_migrate_task));
    g_mock_mig_task.pid = arg->pid;
    g_mock_mig_task.numa_cnt = arg->cnt;
    g_mock_mig_task.hstate = h;
    g_mock_mig_task.status = HAM_TASK_OCCUPIED;
    return &g_mock_mig_task;
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

TEST_F(HamMigrationTest, QueueQualifiedPagesFiltersByState)
{
    struct ham_migrate_task mig_task = {};
    struct ram_block_map map = {};
    struct ham_page_map hpms[3] = {};
    struct list_head hpm_list;

    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &map;
    map.page_num = 3;
    map.hpms = hpms;
    for (int i = 0; i < 3; i++) {
        INIT_LIST_HEAD(&hpms[i].list);
    }
    hpm_set_present(&hpms[1]);
    hpm_set_present(&hpms[2]);
    hpm_set_migrate(&hpms[2]);

    INIT_LIST_HEAD(&hpm_list);
    EXPECT_EQ(1U, queue_qualified_pages(&mig_task, false, &hpm_list));
    EXPECT_EQ(&hpms[1].list, hpm_list.next);

    INIT_LIST_HEAD(&hpm_list);
    EXPECT_EQ(1U, queue_qualified_pages(&mig_task, true, &hpm_list));
    EXPECT_EQ(&hpms[2].list, hpm_list.next);
}

TEST_F(HamMigrationTest, ConstructPageListSkipsInvalidInput)
{
    struct list_head hpm_list;
    struct ham_page_map hpm = {};
    struct folio *folios[1] = {};

    INIT_LIST_HEAD(&hpm_list);
    INIT_LIST_HEAD(&hpm.list);
    list_add_tail(&hpm.list, &hpm_list);

    EXPECT_EQ(0U, construct_page_list(&hpm_list, nullptr, folios));
    EXPECT_EQ(0U, construct_page_list(&hpm_list, mock_get_null_folio, folios));
}

TEST_F(HamMigrationTest, MigrateHelpersNoMatch)
{
    struct list_head hpm_list;
    struct ham_page_map hpm = {};
    struct folio src = {};
    struct folio dst = {};
    struct folio other = {};

    INIT_LIST_HEAD(&hpm_list);
    INIT_LIST_HEAD(&hpm.list);
    hpm.src_folio = &src;
    hpm.dst_folio = &dst;
    hpm.src_numa_id = 1;
    hpm_set_present(&hpm);
    list_add_tail(&hpm.list, &hpm_list);

    EXPECT_EQ(nullptr, get_new_folio(&other, (unsigned long)&hpm_list));
    put_new_folio(&other, (unsigned long)&hpm_list);

    hpm_set_migrate(&hpm);
    EXPECT_EQ(nullptr, get_new_folio_rollback(&other, (unsigned long)&hpm_list));
    put_new_folio_rollback(&other, (unsigned long)&hpm_list);
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

TEST_F(HamMigrationTest, PgtableRollbackAndResume)
{
    struct ham_migrate_task mig_task = {};
    struct ram_block_map maps[2] = {};

    mig_task.pid = 99;
    mig_task.numa_cnt = 2;
    mig_task.ram_maps = maps;
    maps[0].cacheable = true;
    maps[0].hva_start = 0x100000;
    maps[0].size = PAGE_SIZE_2M;
    maps[1].cacheable = false;
    maps[1].hva_start = 0x300000;
    maps[1].size = PAGE_SIZE_2M;

    MOCKER(set_pid_pgtable_cacheable).expects(once()).will(returnValue(0));
    EXPECT_EQ(0, src_pgtable_maintain_rollback(&mig_task));
    EXPECT_TRUE(maps[1].cacheable);

    GlobalMockObject::verify();
    maps[1].cacheable = false;
    MOCKER(set_pid_pgtable_cacheable).expects(once()).will(returnValue(-EFAULT));
    EXPECT_EQ(-EFAULT, src_pgtable_maintain_rollback(&mig_task));
    EXPECT_TRUE(maps[1].cacheable);

    GlobalMockObject::verify();
    mig_task.numa_cnt = 1;
    MOCKER(task_pgtable_within_pid_set_cacheable).expects(once()).will(returnValue(0));
    MOCKER(kernel_pgtable_within_pid_set_valid).expects(once()).will(returnValue(0));
    EXPECT_EQ(0, dst_resume_pgtable_maintain(&mig_task));

    GlobalMockObject::verify();
    MOCKER(task_pgtable_within_pid_set_cacheable).expects(once()).will(returnValue(-EINVAL));
    EXPECT_EQ(-EINVAL, dst_resume_pgtable_maintain(&mig_task));

    GlobalMockObject::verify();
    MOCKER(task_pgtable_within_pid_set_cacheable).expects(once()).will(returnValue(0));
    MOCKER(kernel_pgtable_within_pid_set_valid).expects(once()).will(returnValue(-EIO));
    EXPECT_EQ(-EIO, dst_resume_pgtable_maintain(&mig_task));
}

static int mock_get_ham_pages_freqs_error(pid_t pid, struct freq_info **freq_info_array,
    uint64_t *freq_info_num)
{
    (void)pid;
    (void)freq_info_array;
    (void)freq_info_num;
    return -EINVAL;
}

static int mock_get_ham_pages_freqs_null(pid_t pid, struct freq_info **freq_info_array,
    uint64_t *freq_info_num)
{
    (void)pid;
    *freq_info_array = nullptr;
    *freq_info_num = 0;
    return 0;
}

TEST_F(HamMigrationTest, GetFoliosFreqsFailurePaths)
{
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 99;
    MOCKER(get_ham_pages_freqs).expects(once()).will(invoke(mock_get_ham_pages_freqs_error));
    EXPECT_EQ(-EINVAL, get_folios_freqs(&mig_task));

    GlobalMockObject::verify();
    MOCKER(get_ham_pages_freqs).expects(once()).will(invoke(mock_get_ham_pages_freqs_null));
    EXPECT_EQ(-EINVAL, get_folios_freqs(&mig_task));
}

TEST_F(HamMigrationTest, IoctlCopyFromUserFailures)
{
    MOCKER(copy_from_user).stubs().will(returnValue(1UL));

    EXPECT_EQ(-EFAULT, ioctl_start_migration(0));
    EXPECT_EQ(-EFAULT, ioctl_migration_pages(0));
    EXPECT_EQ(-EFAULT, ioctl_rollback_pages(0));
    EXPECT_EQ(-EFAULT, ioctl_stop_migration(0));
    EXPECT_EQ(-EFAULT, ioctl_modify_pagetable(0));
    EXPECT_EQ(-EFAULT, ioctl_cache_clear(0));
}

TEST_F(HamMigrationTest, HamIoctlDispatchesCopyFailureAndRejectsUnknownCmd)
{
    struct file file = {};

    MOCKER(copy_from_user).stubs().will(returnValue(1UL));

    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_START_MIGRATION, 0));
    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_MIGRATE_PAGES, 0));
    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_ROLLBACK_PAGES, 0));
    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_STOP_MIGRATION, 0));
    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_MODIFY_PAGETABLE, 0));
    EXPECT_EQ(-EFAULT, ham_ioctl(&file, HAM_CACHE_CLEAR, 0));
    EXPECT_EQ(-EINVAL, ham_ioctl(&file, 0xffffffff, 0));
}

TEST_F(HamMigrationTest, FileOpsAndGlobalCacheAttribute)
{
    struct inode inode = {};
    struct file file = {};
    loff_t pos = 0;
    char buf[8] = {};

    EXPECT_EQ(0, ham_open(&inode, &file));
    EXPECT_EQ(0, ham_release(&inode, &file));
    EXPECT_EQ(0, ham_read(&file, nullptr, 16, &pos));
    EXPECT_EQ(0, ham_write(&file, nullptr, 16, &pos));

    EXPECT_EQ((ssize_t)1, global_cache_mnt_store(nullptr, nullptr, "1", 1));
    EXPECT_GT(global_cache_mnt_show(nullptr, nullptr, buf), 0);
    EXPECT_STREQ("1\n", buf);

    memset(buf, 0, sizeof(buf));
    EXPECT_EQ((ssize_t)1, global_cache_mnt_store(nullptr, nullptr, "0", 1));
    EXPECT_GT(global_cache_mnt_show(nullptr, nullptr, buf), 0);
    EXPECT_STREQ("0\n", buf);

    EXPECT_EQ((ssize_t)3, global_cache_mnt_store(nullptr, nullptr, "bad", 3));
}

/* ==================== compare_page_list / compare_page_freq tests ==================== */

TEST_F(HamMigrationTest, ComparePageListAndFreq)
{
    cout << "  [ComparePageListAndFreq]" << endl;
    struct ham_page_map hpm_a = {};
    struct ham_page_map hpm_b = {};
    struct folio dst_a = {};
    struct folio dst_b = {};

    hpm_a.dst_folio = &dst_a;
    hpm_b.dst_folio = &dst_b;

    /* folio_pfn stub returns 0, so both PFN_PHYS values are equal */
    EXPECT_EQ(0, compare_page_list(&hpm_a, &hpm_b));

    /* compare_page_freq uses freq field directly */
    hpm_a.freq = 10;
    hpm_b.freq = 5;
    struct list_head *ptr_a = &hpm_a.list;
    struct list_head *ptr_b = &hpm_b.list;
    EXPECT_EQ(1, compare_page_freq(&ptr_a, &ptr_b));

    hpm_a.freq = 5;
    hpm_b.freq = 10;
    EXPECT_EQ(-1, compare_page_freq(&ptr_a, &ptr_b));

    hpm_a.freq = 7;
    hpm_b.freq = 7;
    EXPECT_EQ(0, compare_page_freq(&ptr_a, &ptr_b));
}

/* ==================== fill_task_pages tests ==================== */

TEST_F(HamMigrationTest, FillTaskPagesMmNull)
{
    cout << "  [FillTaskPagesMmNull]" << endl;
    struct ham_migrate_task mig_task = {};
    mig_task.pid = 100;

    MOCKER(find_get_mm_by_vpid).stubs().will(returnValue((struct mm_struct *)nullptr));
}

TEST_F(HamMigrationTest, FillTaskPagesWalkError)
{
    cout << "  [FillTaskPagesWalkError]" << endl;
    struct ham_migrate_task mig_task = {};
    struct mm_struct mm_val = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.hva_start = 0x100000;
    rmap.size = PAGE_SIZE_2M;

    MOCKER(find_get_mm_by_vpid).stubs().will(returnValue(&mm_val));
    MOCKER(mmap_read_lock).stubs();
    MOCKER(walk_page_range).stubs().will(returnValue(-EINVAL));
    MOCKER(mmap_read_unlock).stubs();
    MOCKER(mmput).stubs();

    EXPECT_EQ(-EINVAL, fill_task_pages(&mig_task));
}

TEST_F(HamMigrationTest, FillTaskPagesSuccess)
{
    cout << "  [FillTaskPagesSuccess]" << endl;
    struct ham_migrate_task mig_task = {};
    struct mm_struct mm_val = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.hva_start = 0x100000;
    rmap.size = PAGE_SIZE_2M;

    MOCKER(find_get_mm_by_vpid).stubs().will(returnValue(&mm_val));
    MOCKER(mmap_read_lock).stubs();
    MOCKER(walk_page_range).stubs().will(returnValue(0));
    MOCKER(mmap_read_unlock).stubs();
    MOCKER(mmput).stubs();

    EXPECT_EQ(0, fill_task_pages(&mig_task));
}

/* ==================== check_migration_param tests ==================== */

TEST_F(HamMigrationTest, CheckMigrationParamIllegalPid)
{
    cout << "  [CheckMigrationParamIllegalPid]" << endl;
    struct migration_param param = {};
    struct hstate *h = nullptr;

    param.pid = 100;
    param.cnt = 1;

    MOCKER(pid_legally).stubs().will(returnValue(false));
    EXPECT_EQ(-EINVAL, check_migration_param(param, &h));
}

TEST_F(HamMigrationTest, CheckMigrationParamCntZero)
{
    cout << "  [CheckMigrationParamCntZero]" << endl;
    struct migration_param param = {};
    struct hstate *h = nullptr;

    param.pid = 100;
    param.cnt = 0;

    MOCKER(pid_legally).stubs().will(returnValue(true));
    EXPECT_EQ(-EINVAL, check_migration_param(param, &h));
}

TEST_F(HamMigrationTest, CheckMigrationParamCntExceedsBatchNum)
{
    cout << "  [CheckMigrationParamCntExceedsBatchNum]" << endl;
    struct migration_param param = {};
    struct hstate *h = nullptr;

    param.pid = 100;
    param.cnt = BATCH_NUM + 1;

    MOCKER(pid_legally).stubs().will(returnValue(true));
    EXPECT_EQ(-EINVAL, check_migration_param(param, &h));
}

TEST_F(HamMigrationTest, CheckMigrationParamMmNull)
{
    cout << "  [CheckMigrationParamMmNull]" << endl;
    struct migration_param param = {};
    struct hstate *h = nullptr;

    param.pid = 100;
    param.cnt = 1;

    MOCKER(pid_legally).stubs().will(returnValue(true));
    MOCKER(find_get_mm_by_vpid).stubs().will(returnValue((struct mm_struct *)nullptr));
    EXPECT_EQ(-EINVAL, check_migration_param(param, &h));
}

/* ==================== ioctl_start_migration tests ==================== */

TEST_F(HamMigrationTest, IoctlStartMigrationParamCheckFails)
{
    cout << "  [IoctlStartMigrationParamCheckFails]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(check_migration_param).stubs().will(returnValue(-EINVAL));

    EXPECT_EQ(-EINVAL, ioctl_start_migration(0));
}

TEST_F(HamMigrationTest, IoctlStartMigrationInitTaskNull)
{
    cout << "  [IoctlStartMigrationInitTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(check_migration_param).stubs().will(returnValue(0));
    MOCKER(init_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-ENOMEM, ioctl_start_migration(0));
}

TEST_F(HamMigrationTest, IoctlStartMigrationNumaNoNodeSuccess)
{
    cout << "  [IoctlStartMigrationNumaNoNodeSuccess]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(check_migration_param).stubs().will(returnValue(0));
    MOCKER(init_migrate_task).stubs().will(invoke(mock_init_migrate_task_success));
    MOCKER(create_numa_map).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_start_migration(0));
}

TEST_F(HamMigrationTest, IoctlStartMigrationConfigHugePageFails)
{
    cout << "  [IoctlStartMigrationConfigHugePageFails]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_remote_node));
    MOCKER(check_migration_param).stubs().will(returnValue(0));
    MOCKER(init_migrate_task).stubs().will(invoke(mock_init_migrate_task_success));
    MOCKER(config_system_huge_page).stubs().will(returnValue(-EFAULT));
    MOCKER(release_migrate_task).stubs();

    EXPECT_EQ(-EFAULT, ioctl_start_migration(0));
}

TEST_F(HamMigrationTest, IoctlStartMigrationCreateMapFails)
{
    cout << "  [IoctlStartMigrationCreateMapFails]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(check_migration_param).stubs().will(returnValue(0));
    MOCKER(init_migrate_task).stubs().will(invoke(mock_init_migrate_task_success));
    MOCKER(create_numa_map).stubs().will(returnValue(-ENOMEM));
    MOCKER(release_migrate_task).stubs();

    EXPECT_EQ(-ENOMEM, ioctl_start_migration(0));
}

/* ==================== ioctl_migration_pages tests ==================== */

TEST_F(HamMigrationTest, IoctlMigrationPagesTaskNull)
{
    cout << "  [IoctlMigrationPagesTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-EINVAL, ioctl_migration_pages(0));
}

TEST_F(HamMigrationTest, IoctlMigrationPagesStatusCheckFails)
{
    cout << "  [IoctlMigrationPagesStatusCheckFails]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_OCCUPIED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(-EINVAL));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_migration_pages(0));
}

TEST_F(HamMigrationTest, IoctlMigrationPagesFillPagesFails)
{
    cout << "  [IoctlMigrationPagesFillPagesFails]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(fill_task_pages).stubs().will(returnValue(-EINVAL));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_migration_pages(0));
}

TEST_F(HamMigrationTest, IoctlMigrationPagesNoQualifiedPages)
{
    cout << "  [IoctlMigrationPagesNoQualifiedPages]" << endl;
    struct ham_migrate_task mig_task = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.page_num = 0;
    rmap.hpms = nullptr;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(fill_task_pages).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    long ret = ioctl_migration_pages(0);
    EXPECT_EQ(0, ret);
}

/* ==================== ioctl_stop_migration tests ==================== */

TEST_F(HamMigrationTest, IoctlStopMigrationTaskNull)
{
    cout << "  [IoctlStopMigrationTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-EINVAL, ioctl_stop_migration(0));
}

TEST_F(HamMigrationTest, IoctlStopMigrationStatusCheckFails)
{
    cout << "  [IoctlStopMigrationStatusCheckFails]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_OCCUPIED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(-EINVAL));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_stop_migration(0));
}

TEST_F(HamMigrationTest, IoctlStopMigrationNoAllowMigr)
{
    cout << "  [IoctlStopMigrationNoAllowMigr]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(release_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_stop_migration(0));
}

TEST_F(HamMigrationTest, IoctlStopMigrationSuccess)
{
    cout << "  [IoctlStopMigrationSuccess]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED | HAM_TASK_ALLOW_MIGR;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_stop_migration(0));
}

/* ==================== ioctl_modify_pagetable tests ==================== */

TEST_F(HamMigrationTest, IoctlModifyPagetableTaskNull)
{
    cout << "  [IoctlModifyPagetableTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-EINVAL, ioctl_modify_pagetable(0));
}

TEST_F(HamMigrationTest, IoctlModifyPagetableStatusFails)
{
    cout << "  [IoctlModifyPagetableStatusFails]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(-EINVAL));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_modify_pagetable(0));
}

TEST_F(HamMigrationTest, IoctlModifyPagetableCacheableSuccess)
{
    cout << "  [IoctlModifyPagetableCacheableSuccess]" << endl;
    struct ham_migrate_task mig_task = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.hva_start = 0x100000;
    rmap.size = PAGE_SIZE_2M;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(task_pgtable_within_pid_set_cacheable).stubs().will(returnValue(0));
    MOCKER(kernel_pgtable_within_pid_set_valid).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_modify_pagetable(0));
}

TEST_F(HamMigrationTest, IoctlModifyPagetableNonCacheableSuccess)
{
    cout << "  [IoctlModifyPagetableNonCacheableSuccess]" << endl;
    struct ham_migrate_task mig_task = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.hva_start = 0x100000;
    rmap.size = PAGE_SIZE_2M;

    /* kernel_pgtable_within_pa_set_cacheable is inline, calls
       set_linear_mapping_nc which is also inline and returns 0.
       task_pgtable_within_pid_set_cacheable must be mocked. */
    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_non_cacheable));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(task_pgtable_within_pid_set_cacheable).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_modify_pagetable(0));
}

/* ==================== ioctl_cache_clear tests ==================== */

TEST_F(HamMigrationTest, IoctlCacheClearTaskNull)
{
    cout << "  [IoctlCacheClearTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-EINVAL, ioctl_cache_clear(0));
}

TEST_F(HamMigrationTest, IoctlCacheClearStatusFails)
{
    cout << "  [IoctlCacheClearStatusFails]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(-EINVAL));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_cache_clear(0));
}

TEST_F(HamMigrationTest, IoctlCacheClearSuccess)
{
    cout << "  [IoctlCacheClearSuccess]" << endl;
    struct ham_migrate_task mig_task = {};
    struct ram_block_map rmap = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_MIGRATED;
    mig_task.numa_cnt = 1;
    mig_task.ram_maps = &rmap;
    rmap.rmt_numa_start = 0x200000;
    rmap.size = PAGE_SIZE_2M;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(flush_cache_by_pa).stubs().will(returnValue(0));
    MOCKER(put_migrate_task).stubs();

    EXPECT_EQ(0, ioctl_cache_clear(0));
}

/* ==================== ioctl_rollback_pages tests ==================== */

TEST_F(HamMigrationTest, IoctlRollbackPagesTaskNull)
{
    cout << "  [IoctlRollbackPagesTaskNull]" << endl;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue((struct ham_migrate_task *)nullptr));

    EXPECT_EQ(-EINVAL, ioctl_rollback_pages(0));
}

TEST_F(HamMigrationTest, IoctlRollbackPagesStatusNotMigrated)
{
    cout << "  [IoctlRollbackPagesStatusNotMigrated]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_INITED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(-EINVAL));
    MOCKER(release_migrate_task).stubs();

    EXPECT_EQ(-EINVAL, ioctl_rollback_pages(0));
}

TEST_F(HamMigrationTest, IoctlRollbackPagesFindVpidNull)
{
    cout << "  [IoctlRollbackPagesFindVpidNull]" << endl;
    struct ham_migrate_task mig_task = {};

    mig_task.pid = 100;
    mig_task.status = HAM_TASK_MIGRATED;

    MOCKER(copy_from_user).stubs().will(invoke(mock_copy_from_user_set_param));
    MOCKER(get_migrate_task).stubs().will(returnValue(&mig_task));
    MOCKER(check_task_status).stubs().will(returnValue(0));
    MOCKER(find_vpid).stubs().will(returnValue((struct pid *)nullptr));
    MOCKER(release_migrate_task).stubs();

    EXPECT_EQ(-EFAULT, ioctl_rollback_pages(0));
}

/* ==================== check_rmt_numa_info tests ==================== */

TEST_F(HamMigrationTest, CheckRmtNumaInfoNoNode)
{
    cout << "  [CheckRmtNumaInfoNoNode]" << endl;
    struct ram_block_info rbi = {};

    rbi.rmt_numa_id = NUMA_NO_NODE;
    EXPECT_EQ(0, check_rmt_numa_info(&rbi));
}

TEST_F(HamMigrationTest, CheckRmtNumaInfoInvalidNode)
{
    cout << "  [CheckRmtNumaInfoInvalidNode]" << endl;
    struct ram_block_info rbi = {};

    rbi.rmt_numa_id = -2;
    EXPECT_EQ(-EINVAL, check_rmt_numa_info(&rbi));

    GlobalMockObject::verify();

    rbi.rmt_numa_id = MAX_NUMNODES;
    EXPECT_EQ(-EINVAL, check_rmt_numa_info(&rbi));
}

/* ==================== HamMigrationParamTest ==================== */

class HamMigrationParamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[HamMigrationParamTest SetUp Begin]" << endl;
    }
    void TearDown() override
    {
        cout << "[HamMigrationParamTest TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[HamMigrationParamTest TearDown End]" << endl;
    }
};

TEST_F(HamMigrationParamTest, FillTaskPagesMmNull)
{
    struct ham_migrate_task mig_task = {};
    mig_task.pid = 999;
    MOCKER(find_get_mm_by_vpid).stubs().will(returnValue((struct mm_struct *)nullptr));
    int ret = fill_task_pages(&mig_task);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HamMigrationParamTest, CheckMigrationParamInvalidPid)
{
    struct migration_param param = {};
    struct hstate *h = nullptr;
    param.pid = -1;
    param.cnt = 1;
    MOCKER(pid_legally).stubs().will(returnValue(false));
    int ret = check_migration_param(param, &h);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HamMigrationParamTest, CheckMigrationParamCntZero)
{
    struct migration_param param = {};
    struct hstate *h = nullptr;
    param.pid = 100;
    param.cnt = 0;
    MOCKER(pid_legally).stubs().will(returnValue(true));
    int ret = check_migration_param(param, &h);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HamMigrationParamTest, ComparePageListLess)
{
    struct ham_page_map hpm_a = {};
    struct ham_page_map hpm_b = {};
    struct folio folio_a = {};
    struct folio folio_b = {};
    hpm_a.dst_folio = &folio_a;
    hpm_b.dst_folio = &folio_b;
    /* folio_pfn returns 0 in UT, so both PFN_PHYS values are equal */
    int ret = compare_page_list(&hpm_a, &hpm_b);
    EXPECT_EQ(0, ret);
}

TEST_F(HamMigrationParamTest, ComparePageFreqLess)
{
    struct ham_page_map hpm_a = {};
    struct ham_page_map hpm_b = {};
    hpm_a.freq = 100;
    hpm_b.freq = 200;
    struct list_head *ptr_a = &hpm_a.list;
    struct list_head *ptr_b = &hpm_b.list;
    int ret = compare_page_freq(&ptr_a, &ptr_b);
    EXPECT_EQ(-1, ret);
}

TEST_F(HamMigrationParamTest, PidLegallyFalse)
{
    MOCKER(pid_legally).stubs().will(returnValue(false));
    bool ret = pid_legally(-1);
    EXPECT_EQ(false, ret);
}

TEST_F(HamMigrationParamTest, CheckRmtNumaInfoNumaNoNode)
{
    struct ram_block_info rbi = {};
    rbi.rmt_numa_id = NUMA_NO_NODE;
    int ret = check_rmt_numa_info(&rbi);
    EXPECT_EQ(0, ret);
}

TEST_F(HamMigrationParamTest, CheckRmtNumaInfoOutOfRange)
{
    struct ram_block_info rbi = {};
    rbi.rmt_numa_id = MAX_NUMNODES;
    int ret = check_rmt_numa_info(&rbi);
    EXPECT_EQ(-EINVAL, ret);
}
