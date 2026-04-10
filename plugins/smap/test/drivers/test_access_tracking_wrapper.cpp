/*
* Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
* Description: SMAP 6.6
*/
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/mm.h>

#include "access_tracking_wrapper.h"

using namespace std;

class AccessTrackingWrapperTest : public ::testing::Test {
protected:
	void SetUp() override
	{
        cout << "[Phase SetUp Begin]" << endl;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
	{
        cout << "[Phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }
};

extern "C" pte_t *drivers_smap__pte_offset_map_lock(struct mm_struct *mm,
    pmd_t *pmd, unsigned long addr, spinlock_t **ptlp);
TEST_F(AccessTrackingWrapperTest, PteOffestMapLock)
{
    pte_t *ret = drivers_smap__pte_offset_map_lock(nullptr, nullptr, 0, nullptr);
    EXPECT_EQ(nullptr, ret);
}

extern "C" pte_t *drivers_smap__pte_offset_map(pmd_t *pmd, unsigned long addr,
    pmd_t *pmdvalp);
TEST_F(AccessTrackingWrapperTest, PteOffsetMap)
{
    pmd_t pmd = {};
    pmd_t pmdval = {};

    pte_t *ret = drivers_smap__pte_offset_map(&pmd, 0, &pmdval);
    EXPECT_EQ(nullptr, ret);
    EXPECT_EQ(1UL, pmdval.pmd);
}

extern "C" unsigned long drivers_pmdp_get_lockless_start(void);
extern "C" void drivers_pmdp_get_lockless_end(unsigned long irqflags);
TEST_F(AccessTrackingWrapperTest, LocklessHelpers)
{
    unsigned long flags = drivers_pmdp_get_lockless_start();
    EXPECT_EQ(0UL, flags);
    drivers_pmdp_get_lockless_end(flags);
}

extern "C" unsigned long drivers_pfn_to_bitidx(const struct page *page, unsigned long pfn);
TEST_F(AccessTrackingWrapperTest, PfnToBitidx)
{
    EXPECT_EQ(0UL, drivers_pfn_to_bitidx(nullptr, 0));
}

extern "C" int num_contig_ptes(unsigned long size, size_t *pgsize);
TEST_F(AccessTrackingWrapperTest, NumContigPtes)
{
    size_t pgsize = 0;

    EXPECT_EQ(1, num_contig_ptes(PUD_SIZE, &pgsize));
    EXPECT_EQ(PUD_SIZE, pgsize);

    EXPECT_EQ(1, num_contig_ptes(PMD_SIZE, &pgsize));
    EXPECT_EQ(PMD_SIZE, pgsize);

    EXPECT_EQ(CONT_PMDS, num_contig_ptes(CONT_PMD_SIZE, &pgsize));
    EXPECT_EQ(PMD_SIZE, pgsize);

    EXPECT_EQ(CONT_PTES, num_contig_ptes(CONT_PTE_SIZE, &pgsize));
    EXPECT_EQ(PAGE_SIZE, pgsize);
}

TEST_F(AccessTrackingWrapperTest, SmapHugePtepGetEarlyReturn)
{
    pte_t pte = {};
    pte_t ret = smap_huge_ptep_get(&pte);

    EXPECT_EQ(1UL, ret.pte);
}
