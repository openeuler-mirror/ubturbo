/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP3.0 模块测试代码
 */
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/slab.h>
#include "smap_migrate_wrapper.h"

using namespace std;

extern "C" spinlock_t *fp_hugetlb_lock;

extern "C" struct hstate *(*fp_size_to_hstate)(unsigned long size);
extern "C" bool (*fp_isolate_huge_page)(struct page *page, struct list_head *list);
extern "C" unsigned long (*fp_sum_zone_node_page_state)(int node,
	enum zone_stat_item item);
extern "C" struct anon_vma *(*fp_page_lock_anon_vma_read)(struct page *page);
extern "C" unsigned long (*fp_kallsyms_lookup_name)(const char *);
extern "C" void lookup_kallsyms_lookup_name(void);
extern "C" int register_kprobe(struct kprobe *p);
extern "C" void unregister_kprobe(struct kprobe *p);

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 6, 0)

#else /* KERNEL_VERSION(5, 10, 0) */
extern "C" struct hstate (*fp_hstates)[];
extern "C" unsigned int *fp_default_hstate_idx;
extern "C" void (*fp_putback_movable_pages)(struct list_head *l);
extern "C" void (*fp_rmap_walk)(struct folio *folio, struct rmap_walk_control *rwc);
extern "C" int (*fp_migrate_pages)(struct list_head *from, new_page_t get_new_page,
	free_page_t put_new_page, unsigned long privat, enum migrate_mode mode, int reason);
#endif /* LINUX_VERSION_CODE */

class SmapMigrateWrapperTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        fp_kallsyms_lookup_name = nullptr;
        fp_migrate_pages = nullptr;
        fp_putback_movable_pages = nullptr;
        fp_isolate_folio_to_list = nullptr;
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }
};

static int fake_migrate_pages(struct list_head *from, new_folio_t get_new_folio,
    free_folio_t put_new_folio, unsigned long private_data,
    enum migrate_mode mode, int reason, unsigned int *ret_succeeded)
{
    return 0;
}

static void fake_putback_movable_pages(struct list_head *l)
{
}

static bool fake_isolate_folio_to_list(struct folio *folio, struct list_head *list)
{
    return true;
}

static unsigned long fake_lookup_success(const char *name)
{
    if (strcmp(name, "migrate_pages") == 0) {
        return reinterpret_cast<unsigned long>(fake_migrate_pages);
    }
    if (strcmp(name, "putback_movable_pages") == 0) {
        return reinterpret_cast<unsigned long>(fake_putback_movable_pages);
    }
    if (strcmp(name, "isolate_folio_to_list") == 0) {
        return reinterpret_cast<unsigned long>(fake_isolate_folio_to_list);
    }
    return 0;
}

static unsigned long fake_lookup_missing_one(const char *name)
{
    if (strcmp(name, "migrate_pages") == 0) {
        return reinterpret_cast<unsigned long>(fake_migrate_pages);
    }
    if (strcmp(name, "putback_movable_pages") == 0) {
        return reinterpret_cast<unsigned long>(fake_putback_movable_pages);
    }
    return 0;
}

static int mock_register_kprobe_success(struct kprobe *p)
{
    p->addr = reinterpret_cast<kprobe_opcode_t *>(fake_lookup_success);
    return 0;
}

static int mock_register_kprobe_missing_one(struct kprobe *p)
{
    p->addr = reinterpret_cast<kprobe_opcode_t *>(fake_lookup_missing_one);
    return 0;
}

extern "C" int pfn_to_bitidx(const struct page *page, unsigned long pfn);
TEST_F(SmapMigrateWrapperTest, PfnToBitidx)
{
    int ret = pfn_to_bitidx(nullptr, 0);
    EXPECT_EQ(0, ret);
}

extern "C" struct folio *dequeue_huge_page_nodemask(struct hstate *h, gfp_t gfp_mask,
	int nid, nodemask_t *nmask);

struct folio *smap_alloc_huge_page_node(struct folio *folio, int nid, bool is_mig_back);


TEST_F(SmapMigrateWrapperTest, SmapAllocHugePageNode)
{
    struct folio *new_folio = NULL;
    struct folio *old_folio = (struct folio *)kmalloc(sizeof(struct folio*), GFP_KERNEL);
    MOCKER(get_hugetlb_folio_nodemask)
        .stubs()
        .with(any())
        .will(returnValue((struct folio*)NULL));
    new_folio = smap_alloc_huge_page_node(old_folio, 0, false);
    EXPECT_EQ(NULL, new_folio);
}

TEST_F(SmapMigrateWrapperTest, LookupKallsymsLookupNameRegisterFail)
{
    MOCKER(register_kprobe).stubs().will(returnValue(-1));

    lookup_kallsyms_lookup_name();

    EXPECT_EQ(nullptr, fp_kallsyms_lookup_name);
}

TEST_F(SmapMigrateWrapperTest, LookupKallsymsLookupNameSuccess)
{
    MOCKER(register_kprobe).stubs().will(invoke(mock_register_kprobe_success));
    MOCKER(unregister_kprobe).stubs();

    lookup_kallsyms_lookup_name();

    ASSERT_NE(nullptr, fp_kallsyms_lookup_name);
    EXPECT_NE(0UL, fp_kallsyms_lookup_name("migrate_pages"));
}

TEST_F(SmapMigrateWrapperTest, SmapProcessSymbolsLookupFail)
{
    MOCKER(register_kprobe).stubs().will(returnValue(-1));

    EXPECT_EQ(-EFAULT, smap_process_symbols());
}

TEST_F(SmapMigrateWrapperTest, SmapProcessSymbolsSuccess)
{
    MOCKER(register_kprobe).stubs().will(invoke(mock_register_kprobe_success));
    MOCKER(unregister_kprobe).stubs();

    EXPECT_EQ(0, smap_process_symbols());
    EXPECT_EQ(reinterpret_cast<void *>(fake_migrate_pages),
        reinterpret_cast<void *>(fp_migrate_pages));
    EXPECT_EQ(reinterpret_cast<void *>(fake_putback_movable_pages),
        reinterpret_cast<void *>(fp_putback_movable_pages));
    EXPECT_EQ(reinterpret_cast<void *>(fake_isolate_folio_to_list),
        reinterpret_cast<void *>(fp_isolate_folio_to_list));
}

TEST_F(SmapMigrateWrapperTest, SmapProcessSymbolsMissingSymbol)
{
    MOCKER(register_kprobe).stubs().will(invoke(mock_register_kprobe_missing_one));
    MOCKER(unregister_kprobe).stubs();

    EXPECT_EQ(-EFAULT, smap_process_symbols());
}
