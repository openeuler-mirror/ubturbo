/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SMAP page flags (freq counter & cumulative acc) helper测试代码
 */

#include "gtest/gtest.h"

#include "smap_page_flags.h"

using namespace std;

/*
 * The freq counter, freq sentinel and acc counter share the LAST_CPUPID
 * field of page->flags. These tests verify the acc semantics (accumulate,
 * decay, saturation, freed-page marker) and that acc never disturbs the
 * freq bits (and vice versa).
 */
class SmapPageFlagsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        /* only page->flags is consumed by the helpers under test */
        m_page.flags = 0;
    }

    void TearDown() override
    {
    }

    /* Directly preset the acc field bits. */
    void set_acc(u16 val)
    {
        m_page.flags &= ~SMAP_ACC_CNT_FIELD_MASK;
        m_page.flags |= (unsigned long)val << SMAP_ACC_CNT_SHIFT;
    }

    /* Directly preset the freq field bits. */
    void set_freq(u8 val)
    {
        m_page.flags &= ~SMAP_FREQ_FIELD_MASK;
        m_page.flags |= (unsigned long)val << SMAP_FREQ_VAL_SHIFT;
    }

    u16 raw_acc(void)
    {
        return raw_acc_of(&m_page);
    }

    u8 raw_freq(void)
    {
        return raw_freq_of(&m_page);
    }

    static u16 raw_acc_of(struct page *page)
    {
        return (u16)((page->flags >> SMAP_ACC_CNT_SHIFT) & SMAP_ACC_CNT_MASK);
    }

    static u8 raw_freq_of(struct page *page)
    {
        return (u8)((page->flags >> SMAP_FREQ_VAL_SHIFT) & SMAP_FREQ_VAL_MASK);
    }

    struct page m_page;
};

TEST_F(SmapPageFlagsTest, AccGetZero)
{
    EXPECT_EQ(0, smap_page_acc_get(&m_page));
}

TEST_F(SmapPageFlagsTest, AccGetValue)
{
    set_acc(20);
    EXPECT_EQ(20, smap_page_acc_get(&m_page));
}

/* Freed page resets LAST_CPUPID to all-1s: acc reads as 0. */
TEST_F(SmapPageFlagsTest, AccGetInvalid)
{
    set_acc(SMAP_ACC_CNT_INVALID);
    EXPECT_EQ(0, smap_page_acc_get(&m_page));
}

TEST_F(SmapPageFlagsTest, AccGetMax)
{
    set_acc(SMAP_ACC_CNT_MAX);
    EXPECT_EQ(SMAP_ACC_CNT_MAX, smap_page_acc_get(&m_page));
}

/* Non-decay round: N -> N + 1. */
TEST_F(SmapPageFlagsTest, AccInc)
{
    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(1, raw_acc());

    set_acc(20);
    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(21, raw_acc());
}

/* Decay round (hot page): N -> (N + 1) >> 1, may decay back to 0. */
TEST_F(SmapPageFlagsTest, AccIncDecay)
{
    set_acc(20);
    smap_page_acc_inc(&m_page, true);
    EXPECT_EQ(10, raw_acc());

    set_acc(SMAP_ACC_CNT_MAX);
    smap_page_acc_inc(&m_page, true);
    EXPECT_EQ((SMAP_ACC_CNT_MAX + 1) >> 1, raw_acc());

    /* long-idle hot page: history halves down to 0 */
    set_acc(1);
    smap_page_acc_inc(&m_page, true);
    EXPECT_EQ(1, raw_acc());

    set_acc(0);
    smap_page_acc_inc(&m_page, true);
    EXPECT_EQ(0, raw_acc());
}

/* Decay round (cold page): N -> N >> 1, may reach 0. */
TEST_F(SmapPageFlagsTest, AccDecay)
{
    set_acc(20);
    smap_page_acc_decay(&m_page);
    EXPECT_EQ(10, raw_acc());

    set_acc(1);
    smap_page_acc_decay(&m_page);
    EXPECT_EQ(0, raw_acc());

    /* already zero: stays zero */
    set_acc(0);
    smap_page_acc_decay(&m_page);
    EXPECT_EQ(0, raw_acc());

    /* freed-page marker: untouched */
    set_acc(SMAP_ACC_CNT_INVALID);
    smap_page_acc_decay(&m_page);
    EXPECT_EQ(SMAP_ACC_CNT_INVALID, raw_acc());

    /* freq counter survives a cold-page decay */
    set_freq(7);
    set_acc(20);
    smap_page_acc_decay(&m_page);
    EXPECT_EQ(10, raw_acc());
    EXPECT_EQ(7, raw_freq());
}

/* Freed-page marker restarts at SMAP_ACC_DECAY_CYCLE/2, decay flag is ignored.
 * The boost prevents new transient pages from being falsely classified as cold. */
TEST_F(SmapPageFlagsTest, AccIncInvalid)
{
    set_acc(SMAP_ACC_CNT_INVALID);
    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(SMAP_ACC_DECAY_CYCLE / 2, raw_acc());

    set_acc(SMAP_ACC_CNT_INVALID);
    smap_page_acc_inc(&m_page, true);
    EXPECT_EQ(SMAP_ACC_DECAY_CYCLE / 2, raw_acc());
}

/* Counter saturates at SMAP_ACC_CNT_MAX. */
TEST_F(SmapPageFlagsTest, AccIncSaturate)
{
    set_acc(SMAP_ACC_CNT_MAX);
    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(SMAP_ACC_CNT_MAX, raw_acc());

    set_acc(SMAP_ACC_CNT_MAX - 1);
    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(SMAP_ACC_CNT_MAX, raw_acc());
}

/* acc and freq live in the same LAST_CPUPID field: never disturb each other. */
TEST_F(SmapPageFlagsTest, AccFreqIndependence)
{
    set_freq(5);
    set_acc(20);

    smap_page_acc_inc(&m_page, false);
    EXPECT_EQ(5, raw_freq());
    EXPECT_EQ(21, raw_acc());

    smap_page_freq_inc(&m_page);
    EXPECT_EQ(6, raw_freq());
    EXPECT_EQ(21, raw_acc());

    /* freq read-clear only sets the freq sentinel, acc untouched */
    EXPECT_EQ(6, smap_page_freq_read_clear(&m_page));
    EXPECT_EQ(21, raw_acc());
    EXPECT_TRUE(m_page.flags & SMAP_FREQ_SENT_BIT);

    /* freq reset from sentinel state must not clobber acc */
    smap_page_freq_inc(&m_page);
    EXPECT_EQ(1, raw_freq());
    EXPECT_EQ(21, raw_acc());
}

/* ---- migrate_folio_actc: acc inheritance across migration ---- */

TEST_F(SmapPageFlagsTest, MigrateFolioActcInherit)
{
    struct folio from = {};
    struct folio to = {};

    from.page.flags = (unsigned long)20 << SMAP_ACC_CNT_SHIFT;

    migrate_folio_actc(&from, &to);
    EXPECT_EQ(20, smap_page_acc_get(&to.page));
    /* source keeps its own acc until it is actually freed */
    EXPECT_EQ(20, smap_page_acc_get(&from.page));
}

/* Only the acc field is rewritten; freq counter and sentinel survive. */
TEST_F(SmapPageFlagsTest, MigrateFolioActcPreservesFreq)
{
    struct folio from = {};
    struct folio to = {};

    from.page.flags = (unsigned long)20 << SMAP_ACC_CNT_SHIFT;
    to.page.flags = (unsigned long)7 << SMAP_FREQ_VAL_SHIFT |
                    SMAP_FREQ_SENT_BIT;

    migrate_folio_actc(&from, &to);
    EXPECT_EQ(20, raw_acc_of(&to.page));
    EXPECT_EQ(7, raw_freq_of(&to.page));
    EXPECT_TRUE(to.page.flags & SMAP_FREQ_SENT_BIT);
}

/* A freshly allocated page may carry the freed-page marker: it is replaced. */
TEST_F(SmapPageFlagsTest, MigrateFolioActcReplacesInvalid)
{
    struct folio from = {};
    struct folio to = {};

    from.page.flags = (unsigned long)20 << SMAP_ACC_CNT_SHIFT;
    to.page.flags = (unsigned long)SMAP_ACC_CNT_INVALID << SMAP_ACC_CNT_SHIFT;

    migrate_folio_actc(&from, &to);
    EXPECT_EQ(20, smap_page_acc_get(&to.page));
}

/* Cold source (acc == 0): nothing to inherit, destination untouched. */
TEST_F(SmapPageFlagsTest, MigrateFolioActcZeroSource)
{
    struct folio from = {};
    struct folio to = {};

    from.page.flags = 0;
    to.page.flags = (unsigned long)SMAP_ACC_CNT_INVALID << SMAP_ACC_CNT_SHIFT;

    migrate_folio_actc(&from, &to);
    EXPECT_EQ((unsigned long)SMAP_ACC_CNT_INVALID << SMAP_ACC_CNT_SHIFT,
              to.page.flags);
}

/* NULL arguments (e.g. allocation failure) are ignored. */
TEST_F(SmapPageFlagsTest, MigrateFolioActcNullArgs)
{
    struct folio from = {};
    struct folio to = {};

    from.page.flags = (unsigned long)20 << SMAP_ACC_CNT_SHIFT;

    migrate_folio_actc(nullptr, &to);
    EXPECT_EQ(0, to.page.flags);
    migrate_folio_actc(&from, nullptr);
    EXPECT_EQ((unsigned long)20 << SMAP_ACC_CNT_SHIFT, from.page.flags);
    migrate_folio_actc(nullptr, nullptr);
}
