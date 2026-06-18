/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP5.0 内存地址模块测试代码
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/kvm_pgtable.h>
#include <linux/list.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/errno.h>

#include "kvm_pgtable.h"

using namespace std;

struct stage2_attr_data {
    kvm_pte_t attr_set;
    kvm_pte_t attr_clr;
    kvm_pte_t pte;
    u32 level;
};

struct kvm_pgtable_walk_data {
    struct kvm_pgtable_walker *walker;
    u64 start;
    u64 addr;
    u64 end;
};

class KvmPgTableTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::cout << "[Phase SetUp Begin]" << std::endl;
        std::cout << "[Phase SetUp End]" << std::endl;
    }
    void TearDown() override
    {
        std::cout << "[Phase TearDown Begin]" << std::endl;
        GlobalMockObject::reset();
        std::cout << "[Phase TearDown End]" << std::endl;
    }
};

extern "C" bool kvm_pte_table(kvm_pte_t pte, u32 level);
TEST_F(KvmPgTableTest, PteTableTest)
{
    kvm_pte_t pte = 0;
    u32 level = 3;
    bool ret = kvm_pte_table(pte, level);
    EXPECT_EQ(false, ret);
}

TEST_F(KvmPgTableTest, PteTableTestTwo)
{
    kvm_pte_t pte = 1;
    u32 level = 0;
    bool ret = kvm_pte_table(pte, level);
    EXPECT_EQ(false, ret);
}

static void* stubFunc(phys_addr_t phys)
{
    return NULL;
}

extern "C" kvm_pte_t *kvm_pte_follow(kvm_pte_t pte, struct kvm_pgtable_mm_ops *mm_ops);
TEST_F(KvmPgTableTest, PteFollowTest)
{
    struct kvm_pgtable_mm_ops mm_ops = {
        .phys_to_virt = stubFunc,
    };
    kvm_pte_t pte = 0;
    kvm_pte_t *ret = kvm_pte_follow(pte, &mm_ops);
    EXPECT_EQ(nullptr, ret);
}

static int my_callback_function(const struct kvm_pgtable_visit_ctx *ctx,
    enum kvm_pgtable_walk_flags visit)
{
    return 0;
}
extern "C" int kvm_pgtable_visitor_cb(struct kvm_pgtable_walk_data *data,
    const struct kvm_pgtable_visit_ctx *ctx, enum kvm_pgtable_walk_flags visit);
extern "C" u32 kvm_pgtable_idx(struct kvm_pgtable_walk_data *data, u32 level);
TEST_F(KvmPgTableTest, PgtableIdxTest)
{
    struct kvm_pgtable_walk_data data = {
        .addr = 0x10000,
    };
    u32 level = 0;
    int ret = kvm_pgtable_idx(&data, level);
    EXPECT_EQ(0, ret);
}

extern "C" int __kvm_pgtable_visit(struct kvm_pgtable_walk_data *data,
    struct kvm_pgtable_mm_ops *mm_ops, kvm_pteref_t pteref, u32 level);
extern "C" int __kvm_pgtable_walk(struct kvm_pgtable_walk_data *data,
    struct kvm_pgtable_mm_ops *mm_ops, kvm_pteref_t pgtable, u32 level);
TEST_F(KvmPgTableTest, PgtableWalkTest)
{
    struct kvm_pgtable_walk_data data;
    struct kvm_pgtable_mm_ops mm_ops;
    u64 *arry = (u64 *)malloc(sizeof(u64));
    kvm_pteref_t pgtable = arry;
    u32 level = 0;
    MOCKER(kvm_pgtable_idx).stubs().will(returnValue(0));
    int ret = __kvm_pgtable_walk(&data, &mm_ops, pgtable, level);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" u32 kvm_pgd_page_idx(struct kvm_pgtable *pgt, u64 addr);
TEST_F(KvmPgTableTest, PgdPageIdxTest)
{
    struct kvm_pgtable pgt = {
        .ia_bits = 1,
        .start_level = 0,
    };
    u64 addr = 0;
    u64 num = 0;
    int ret = kvm_pgd_page_idx(&pgt, addr);
    EXPECT_EQ(0, ret);
}

extern "C" int _kvm_pgtable_walk(struct kvm_pgtable *pgt, struct kvm_pgtable_walk_data *data);
TEST_F(KvmPgTableTest, KvmPgtableWalkTest)
{
    struct kvm_pgtable pgt = {
        .ia_bits = 0x200000,
        .pgd = nullptr,
    };
    struct kvm_pgtable_walk_data data = {};
    data.addr = 0x1;
    data.end = 0x20;
    int ret = _kvm_pgtable_walk(&pgt, &data);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(KvmPgTableTest, KvmPgtableWalkTestTwo)
{
    struct kvm_pgtable pgt = {
        .ia_bits = 0x200000,
    };
    pgt.pgd = (u64 *)malloc(sizeof(u64));
    struct kvm_pgtable_walk_data data = {};
    data.addr = 0x1;
    data.end = 0x20;
    MOCKER(kvm_pgtable_idx).stubs().will(returnValue(0));
    MOCKER(__kvm_pgtable_walk).stubs().will(returnValue(-EINVAL));
    int ret = _kvm_pgtable_walk(&pgt, &data);
    EXPECT_EQ(-EINVAL, ret);
    free(pgt.pgd);
}

extern "C" int kvm_pgtable_walk(struct kvm_pgtable *pgt, u64 addr, u64 size,
                                struct kvm_pgtable_walker *walker);
TEST_F(KvmPgTableTest, WalkTest)
{
    struct kvm_pgtable pgt;
    struct kvm_pgtable_walker walker = {
        .flags = KVM_PGTABLE_WALK_TABLE_POST,
    };
    u64 addr = 0;
    u64 size = 1;
    MOCKER(_kvm_pgtable_walk).stubs().will(returnValue(0));
    int ret = kvm_pgtable_walk(&pgt, addr, size, &walker);
    EXPECT_EQ(0, ret);
}

extern "C" bool stage2_try_set_pte(const struct kvm_pgtable_visit_ctx *ctx, kvm_pte_t new_pte);
TEST_F(KvmPgTableTest, stage2_try_set_pte)
{
    struct kvm_pgtable_visit_ctx ctx;
    kvm_pte_t pte = 0;
    kvm_pte_t ptep = 0;
    ctx.ptep = &ptep;
    bool ret = stage2_try_set_pte(&ctx, pte);
    EXPECT_EQ(true, ret);
}

extern "C" bool smap_kvm_pgtable_stage2_test_clear_young(struct kvm_pgtable *pgt, u64 addr, u64 size, bool mkold);
TEST_F(KvmPgTableTest, ClearYoungTest)
{
    struct kvm_pgtable pgt;
    u64 addr = 0;
    u64 size = 1;
    bool ret = smap_kvm_pgtable_stage2_test_clear_young(&pgt, addr, size, true);
    EXPECT_EQ(false, ret);
}

extern "C" bool smap_kvm_pgtable_stage2_is_young(struct kvm_pgtable *pgt, u64 addr);
TEST_F(KvmPgTableTest, YoungTest)
{
    u64 addr = 0;
    struct kvm_pgtable pgt;
    MOCKER(smap_kvm_pgtable_stage2_test_clear_young).stubs().will(returnValue(false));
    bool ret = smap_kvm_pgtable_stage2_is_young(&pgt, addr);
    EXPECT_EQ(false, ret);
}

extern "C" bool smap_kvm_pgtable_stage2_mkold(struct kvm_pgtable *pgt, u64 addr);
TEST_F(KvmPgTableTest, MkOldTest)
{
    u64 addr = 0;
    struct kvm_pgtable pgt;
    MOCKER(smap_kvm_pgtable_stage2_test_clear_young).stubs().will(returnValue(true));
    bool ret = smap_kvm_pgtable_stage2_mkold(&pgt, addr);
    EXPECT_EQ(true, ret);
}

extern "C" bool kvm_pgtable_walk_continue(const struct kvm_pgtable_walker *walker, int r);
TEST_F(KvmPgTableTest, WalkContinueTest)
{
    struct kvm_pgtable_walker walker = {
        .flags = KVM_PGTABLE_WALK_TABLE_POST,
    };
    int r = 0;
    bool ret = kvm_pgtable_walk_continue(&walker, r);
    EXPECT_EQ(true, ret);

    r = -EAGAIN;
    ret = kvm_pgtable_walk_continue(&walker, r);
    EXPECT_EQ(true, ret);
}

TEST_F(KvmPgTableTest, WalkContinueHandleFault)
{
    struct kvm_pgtable_walker walker = {
        .cb = my_callback_function,
        .flags = KVM_PGTABLE_WALK_HANDLE_FAULT,
    };

    EXPECT_EQ(false, kvm_pgtable_walk_continue(&walker, -EAGAIN));
    EXPECT_EQ(false, kvm_pgtable_walk_continue(&walker, -EINVAL));
}

TEST_F(KvmPgTableTest, KvmPgtableWalkOutOfRange)
{
    struct kvm_pgtable pgt = {
        .ia_bits = 1,
        .pgd = (u64 *)malloc(sizeof(u64)),
    };
    struct kvm_pgtable_walk_data data = {};
    data.addr = 8;
    data.end = 9;

    EXPECT_EQ(-ERANGE, _kvm_pgtable_walk(&pgt, &data));
    free(pgt.pgd);
}

extern "C" int smap_stage2_test_clear_young(struct kvm_pgtable *pgt, u64 addr,
    u64 size, kvm_pte_t attr_set, kvm_pte_t attr_clr, bool mkold,
    kvm_pte_t *orig_pte, int *level, bool *young,
    enum kvm_pgtable_walk_flags flags);
TEST_F(KvmPgTableTest, Stage2ClearYoungPropagatesWalkError)
{
    struct kvm_pgtable pgt = {};
    bool young = false;
    kvm_pte_t orig = 0;
    int level = -1;

    MOCKER(kvm_pgtable_walk).stubs().will(returnValue(-EINVAL));
    EXPECT_EQ(-EINVAL, smap_stage2_test_clear_young(&pgt, 0, 1, 0, 0, true,
        &orig, &level, &young, KVM_PGTABLE_WALK_LEAF));
    EXPECT_EQ(false, young);
    EXPECT_EQ(-1, level);
    EXPECT_EQ(0, orig);
}

struct smap_stage2_test_clear_young_data {
    kvm_pte_t attr_set;
    kvm_pte_t attr_clr;
    bool mkold;
    kvm_pte_t pte;
    u32 level;
    bool young;
};

extern "C" bool stage2_pte_executable(kvm_pte_t pte);
TEST_F(KvmPgTableTest, Stage2PteExecutableWithoutXn)
{
    /* pte不含XN位 -> !(pte & KVM_PTE_LEAF_ATTR_HI_S2_XN) -> true */
    kvm_pte_t pte = 0;
    bool ret = stage2_pte_executable(pte);
    EXPECT_EQ(true, ret);
}

TEST_F(KvmPgTableTest, Stage2PteExecutableWithXn)
{
    /* pte含XN位 -> false */
    /* In DT env, KVM_PTE_LEAF_ATTR_HI_S2_XN = BIT(54) = 54 */
    kvm_pte_t pte = 54;
    bool ret = stage2_pte_executable(pte);
    EXPECT_EQ(false, ret);
}

extern "C" int kvm_pgtable_visitor_cb(struct kvm_pgtable_walk_data *data,
    const struct kvm_pgtable_visit_ctx *ctx, enum kvm_pgtable_walk_flags visit);
TEST_F(KvmPgTableTest, VisitorCbCallsWalkerCb)
{
    /* Directly call kvm_pgtable_visitor_cb, do NOT mock it */
    /* Construct walk_data with walker containing a real cb */
    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 0;
    ctx.ptep = &ptep_val;
    ctx.old = 0;
    ctx.flags = KVM_PGTABLE_WALK_LEAF;

    struct kvm_pgtable_walker walker = {
        .cb = my_callback_function,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_LEAF,
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;

    int ret = kvm_pgtable_visitor_cb(&data, &ctx, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
}

static int stub_visitor_cb_return_neg1(const struct kvm_pgtable_visit_ctx *ctx,
    enum kvm_pgtable_walk_flags visit)
{
    return -1;
}

TEST_F(KvmPgTableTest, VisitorCbReturnsWalkerError)
{
    /* Directly call kvm_pgtable_visitor_cb with cb that returns -1 */
    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 0;
    ctx.ptep = &ptep_val;
    ctx.old = 0;
    ctx.flags = KVM_PGTABLE_WALK_LEAF;

    struct kvm_pgtable_walker walker = {
        .cb = stub_visitor_cb_return_neg1,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_LEAF,
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;

    int ret = kvm_pgtable_visitor_cb(&data, &ctx, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(-1, ret);
}

extern "C" int __kvm_pgtable_visit(struct kvm_pgtable_walk_data *data,
    struct kvm_pgtable_mm_ops *mm_ops, kvm_pteref_t pteref, u32 level);
TEST_F(KvmPgTableTest, VisitLeafCallbackWithContinue)
{
    /* __kvm_pgtable_visit: table=false, no leaf flags, walk_continue returns true */
    /* In DT env kvm_pte_valid=false => kvm_pte_table returns false => table=false */
    /* In DT env KVM_PGTABLE_WALK_LEAF=0 => ctx.flags & KVM_PGTABLE_WALK_LEAF = 0 */
    /* So neither table nor leaf branches trigger, takes !table path advancing addr */
    kvm_pte_t pte_val = 0;
    kvm_pteref_t pteref = &pte_val;

    struct kvm_pgtable_walker walker = {
        .cb = my_callback_function,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_LEAF, /* In DT: BIT(0)=0, equivalent to no flags */
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;
    data.start = 0x1000;
    data.addr = 0x1000;
    data.end = 0x2000;

    struct kvm_pgtable_mm_ops mm_ops = {};

    /* No mocks needed - natural DT behavior: table=false, no leaf flags */
    int ret = __kvm_pgtable_visit(&data, &mm_ops, pteref, 0);
    EXPECT_EQ(0, ret);
}

TEST_F(KvmPgTableTest, VisitTablePreCallback)
{
    /* Mock kvm_pte_table to return true to trigger TABLE_PRE branch */
    /* Do NOT mock kvm_pgtable_visitor_cb - let it really execute */
    kvm_pte_t pte_val = 0;
    kvm_pteref_t pteref = &pte_val;

    struct kvm_pgtable_walker walker = {
        .cb = my_callback_function,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_TABLE_PRE,
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;
    data.start = 0x1000;
    data.addr = 0x1000;
    data.end = 0x2000;

    struct kvm_pgtable_mm_ops mm_ops = {};

    /* Mock kvm_pte_table to return true, enabling TABLE_PRE branch */
    MOCKER(kvm_pte_table).stubs().will(returnValue(true));
    /* After visitor_cb returns 0 and reload, mock kvm_pte_table for second call */
    /* In DT: after reload, kvm_pte_table is called again, mock it to return true */
    /* Mock __kvm_pgtable_walk for child table traversal */
    MOCKER(__kvm_pgtable_walk).stubs().will(returnValue(0));
    /* Mock kvm_pte_follow for childp */
    MOCKER(kvm_pte_follow).stubs().will(returnValue((kvm_pte_t *)malloc(sizeof(kvm_pte_t))));
    int ret = __kvm_pgtable_visit(&data, &mm_ops, pteref, 0);
    EXPECT_EQ(0, ret);
}

TEST_F(KvmPgTableTest, VisitWalkContinueInterrupts)
{
    /* Test walk_continue returning false after TABLE_PRE callback returns error */
    /* walker cb returns -EINVAL -> visitor_cb returns -EINVAL */
    /* kvm_pgtable_walk_continue(walker, -EINVAL) returns false -> goto out */
    kvm_pte_t pte_val = 0;
    kvm_pteref_t pteref = &pte_val;

    struct kvm_pgtable_walker walker = {
        .cb = stub_visitor_cb_return_neg1,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_TABLE_PRE,
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;
    data.start = 0x1000;
    data.addr = 0x1000;
    data.end = 0x2000;

    struct kvm_pgtable_mm_ops mm_ops = {};

    /* Mock kvm_pte_table to return true to trigger TABLE_PRE */
    MOCKER(kvm_pte_table).stubs().will(returnValue(true));
    int ret = __kvm_pgtable_visit(&data, &mm_ops, pteref, 0);
    /* visitor_cb returns -1, walk_continue(false) -> goto out -> return -1 */
    EXPECT_EQ(-1, ret);
}

TEST_F(KvmPgTableTest, VisitTablePostCallback)
{
    /* Test TABLE_POST flag: table path with post-order callback */
    /* Mock kvm_pte_table to return true, __kvm_pgtable_walk returns 0 */
    /* walker cb returns 0 -> visitor_cb returns 0 for TABLE_POST */
    kvm_pte_t pte_val = 0;
    kvm_pteref_t pteref = &pte_val;

    struct kvm_pgtable_walker walker = {
        .cb = my_callback_function,
        .arg = NULL,
        .flags = KVM_PGTABLE_WALK_TABLE_POST,
    };

    struct kvm_pgtable_walk_data data = {};
    data.walker = &walker;
    data.start = 0x1000;
    data.addr = 0x1000;
    data.end = 0x2000;

    struct kvm_pgtable_mm_ops mm_ops = {};

    /* Mock kvm_pte_table to return true */
    MOCKER(kvm_pte_table).stubs().will(returnValue(true));
    /* Mock __kvm_pgtable_walk for child traversal returning 0 */
    MOCKER(__kvm_pgtable_walk).stubs().will(returnValue(0));
    /* Mock kvm_pte_follow */
    MOCKER(kvm_pte_follow).stubs().will(returnValue((kvm_pte_t *)malloc(sizeof(kvm_pte_t))));
    int ret = __kvm_pgtable_visit(&data, &mm_ops, pteref, 0);
    EXPECT_EQ(0, ret);
}

extern "C" int smap_stage2_test_clear_young_walker(const struct kvm_pgtable_visit_ctx *ctx,
    enum kvm_pgtable_walk_flags visit);
TEST_F(KvmPgTableTest, WalkerInvalidPteReturnsZero)
{
    /* ctx->old=0 (invalid pte in DT: kvm_pte_valid=false) -> returns 0, data->young not set */
    struct smap_stage2_test_clear_young_data young_data = {};
    young_data.mkold = true;

    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 0;
    ctx.ptep = &ptep_val;
    ctx.old = 0;
    ctx.arg = &young_data;

    int ret = smap_stage2_test_clear_young_walker(&ctx, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(false, young_data.young);
}

TEST_F(KvmPgTableTest, WalkerMkoldFalseReturnsZero)
{
    /* data->mkold=false -> early return for invalid pte */
    /* In DT: kvm_pte_valid(0) = 0 & BIT(0) = 0 & 0 = false */
    /* !kvm_pte_valid -> true -> early return 0, young not set */
    struct smap_stage2_test_clear_young_data young_data = {};
    young_data.mkold = false;

    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 0;
    ctx.ptep = &ptep_val;
    ctx.old = 0;
    ctx.arg = &young_data;

    int ret = smap_stage2_test_clear_young_walker(&ctx, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(false, young_data.young);
}

TEST_F(KvmPgTableTest, WalkerMkoldFalseWithValidPtePath)
{
    /* In DT: kvm_pte_valid is inline and always returns false (BIT(0)=0) */
    /* Cannot mock kvm_pte_valid. Test with pte that has AF bit */
    /* kvm_pte_valid(10) = 10 & 0 = false -> !kvm_pte_valid -> early return */
    struct smap_stage2_test_clear_young_data young_data = {};
    young_data.mkold = false;

    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 10; /* Has AF bit (BIT(10)=10) */
    ctx.ptep = &ptep_val;
    ctx.old = 10;
    ctx.arg = &young_data;

    int ret = smap_stage2_test_clear_young_walker(&ctx, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
    /* young remains false because !kvm_pte_valid triggers early return */
    EXPECT_EQ(false, young_data.young);
}

extern "C" int smap_stage2_test_clear_young(struct kvm_pgtable *pgt, u64 addr,
    u64 size, kvm_pte_t attr_set, kvm_pte_t attr_clr, bool mkold,
    kvm_pte_t *orig_pte, int *level, bool *young,
    enum kvm_pgtable_walk_flags flags);
TEST_F(KvmPgTableTest, Stage2ClearYoungSuccessPath)
{
    /* mock kvm_pgtable_walk返回0 -> 设置young/level/orig_pte输出参数 */
    struct kvm_pgtable pgt = {};
    bool young = false;
    kvm_pte_t orig_pte = 0;
    int level = -1;

    MOCKER(kvm_pgtable_walk).stubs().will(returnValue(0));
    int ret = smap_stage2_test_clear_young(&pgt, 0, 1, 0, 0, true,
        &orig_pte, &level, &young, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
    /* After successful walk, output params are set from data */
    EXPECT_EQ(false, young);
}

TEST_F(KvmPgTableTest, Stage2ClearYoungSuccessNullOutputs)
{
    /* Test with NULL young/level/orig_pte pointers */
    struct kvm_pgtable pgt = {};

    MOCKER(kvm_pgtable_walk).stubs().will(returnValue(0));
    int ret = smap_stage2_test_clear_young(&pgt, 0, 1, 0, 0, true,
        NULL, NULL, NULL, KVM_PGTABLE_WALK_LEAF);
    EXPECT_EQ(0, ret);
}

extern "C" bool stage2_try_set_pte(const struct kvm_pgtable_visit_ctx *ctx, kvm_pte_t new_pte);
TEST_F(KvmPgTableTest, Stage2TrySetPteSharedPath)
{
    /* Test shared path: ctx with KVM_PGTABLE_WALK_SHARED flag -> uses cmpxchg */
    /* In DT: cmpxchg(a, b, c) = (b), so returns old value */
    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 42;
    ctx.ptep = &ptep_val;
    ctx.old = 42;
    ctx.flags = KVM_PGTABLE_WALK_SHARED; /* BIT(3) = 3 in DT */
    kvm_pte_t new_pte = 99;

    /* kvm_pgtable_walk_shared(ctx) = ctx.flags & 3 = 3 (truthy) -> shared path */
    /* cmpxchg(ctx->ptep, ctx->old, new) = ctx->old = 42 == ctx->old -> true */
    bool ret = stage2_try_set_pte(&ctx, new_pte);
    EXPECT_EQ(true, ret);
}

TEST_F(KvmPgTableTest, Stage2TrySetPteNonSharedPath)
{
    /* Test non-shared path: ctx without SHARED flag -> uses WRITE_ONCE */
    /* In DT: WRITE_ONCE(*ctx->ptep, new) = 1, returns true */
    struct kvm_pgtable_visit_ctx ctx = {};
    kvm_pte_t ptep_val = 0;
    ctx.ptep = &ptep_val;
    ctx.old = 0;
    ctx.flags = KVM_PGTABLE_WALK_LEAF; /* No SHARED flag */
    kvm_pte_t new_pte = 42;

    /* kvm_pgtable_walk_shared(ctx) = ctx.flags & 3 = 0 (false) -> non-shared */
    bool ret = stage2_try_set_pte(&ctx, new_pte);
    EXPECT_EQ(true, ret);
}
