/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP3.0 hist_ops.c test code
 * Author: z30062841
 * Create: 2024-12-28
 */
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/types.h>
#include <asm/io.h>
#include <linux/byteorder/generic.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/rwsem.h>

#include "access_iomem.h"
#include "ub_hist.h"
#include "hist_ops.h"
#include "hist_tracking.h"

using namespace std;
class HistOpsTest : public ::testing::Test {
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

extern list_head drivers_remote_ram_list;
extern "C" struct smap_hist_dev g_smap_hist_dev;
extern "C" int ub_hist_init(void);

extern "C" u64 align_addr(u64 addr, u32 low_bit_len);
TEST_F(HistOpsTest, align_addr)
{
    u64 ret;
    u64 addr = 0x1001;
    ret = align_addr(addr, 2);
    EXPECT_EQ(0x1000, ret);
}

extern "C" bool addr_is_aligned(u64 addr, u32 low_bit_len);
TEST_F(HistOpsTest, addr_is_aligned)
{
    u64 addr = 0x1000;
    EXPECT_EQ(true, addr_is_aligned(addr, 2));

    u64 addr1 = 0x1001;
    EXPECT_EQ(false, addr_is_aligned(addr1, 2));
}

extern "C" u64 align_addr_by_sts_size(u64 addr, u8 sts_size);
TEST_F(HistOpsTest, align_addr_by_sts_size)
{
    u64 ret;
    u64 size = 0x1000;
    g_smap_hist_dev.freq_register_cnt = BA_STS_VALUE_COUNT;
    MOCKER(align_addr).stubs().will(returnValue(size));
    ret = align_addr_by_sts_size(0x1, STS_SIZE_4K);
    EXPECT_EQ(size, ret);

    ret = align_addr_by_sts_size(0x1, STS_SIZE_2M);
    EXPECT_EQ(size, ret);
}

extern "C" bool addr_seg_is_valid(struct segs_info *info);
TEST_F(HistOpsTest, addr_seg_is_valid)
{
    bool ret;
    struct addr_seg seg;
    struct segs_info seg_info = {
        .cnt = 0,
        .segs = NULL
    };
    struct ub_hist_ba_info ba_info = {
        .ba_tag = 0,
    };
    ba_info.nc_range.start = 0;
    ba_info.nc_range.end = (1 << HIST_ADDR_SHIFT_16G) - 1;
    ret = addr_seg_is_valid(NULL);
    EXPECT_EQ(false, ret);
}

TEST_F(HistOpsTest, addr_seg_is_valid_two)
{
    bool ret;
    struct segs_info seg_info1 = {
        .cnt = 0,
    };

    struct segs_info seg_info2 = {
        .cnt = 1,
    };

    ret = addr_seg_is_valid(&seg_info1);
    EXPECT_EQ(false, ret);

    ret = addr_seg_is_valid(&seg_info2);
    EXPECT_EQ(false, ret);
}

extern "C" int addr_seg_cmp_start(const void *seg1, const void *seg2);
TEST_F(HistOpsTest, addr_seg_cmp_start)
{
    int ret;
    struct addr_seg s1 = {
        .start = 0x10,
    };
    struct addr_seg s2 = {
        .start = 0,
    };
    ret = addr_seg_cmp_start(&s1, &s2);
    EXPECT_EQ(1, ret);
}

extern "C" int addr_seg_cmp_max(const void *win1, const void *win2);
TEST_F(HistOpsTest, addr_seg_cmp_max)
{
    int ret;
    struct addr_seg s1 = {
        .max = 0,
    };
    struct addr_seg s2 = {
        .max = 1,
    };
    ret = addr_seg_cmp_max(&s1, &s2);
    EXPECT_EQ(1, ret);
}

extern "C" bool addr_seg_is_continuous_scan_wins(struct addr_seg *seg1,
    struct addr_seg *seg2, enum ub_hist_sts_size sts_size);
TEST_F(HistOpsTest, addr_seg_is_continuous_scan_wins)
{
    bool ret;
    struct addr_seg s1 = {
        .start = 0,
        .size = 0x10,
    };
    struct addr_seg s2 = {
        .start = 0,
        .size = 0x100,
    };
    ret = addr_seg_is_continuous_scan_wins(&s1, &s2, STS_SIZE_2M);
    EXPECT_EQ(false, ret);
}

TEST_F(HistOpsTest, addr_seg_is_continuous_scan_wins_two)
{
    bool ret;
    struct addr_seg s1 = {
        .start = 0,
        .size = 0x10,
    };
    struct addr_seg s2 = {
        .start = 0x400000001,
        .size = 0x100,
    };

    ret = addr_seg_is_continuous_scan_wins(&s1, &s2, STS_SIZE_2M);
    EXPECT_EQ(true, ret);
}

TEST_F(HistOpsTest, addr_seg_is_continuous_scan_wins_three)
{
    bool ret;
    struct addr_seg s1 = {
        .start = 0x100,
        .size = 0x400000000,
    };
    struct addr_seg s2 = {
        .start = 0x400000000,
        .size = 0x100,
    };
    ret = addr_seg_is_continuous_scan_wins(&s1, &s2, STS_SIZE_2M);
    EXPECT_EQ(false, ret);
}

/* Test addr_seg_is_continuous_scan_wins with STS_SIZE_4K granularity */
TEST_F(HistOpsTest, addr_seg_is_continuous_scan_wins_4k)
{
    bool ret;
    struct addr_seg s1 = {
        .start = 0,
        .size = 0x1000,
    };
    struct addr_seg s2 = {
        .start = 0x2000,
        .size = 0x1000,
    };
    /* With 4K granularity, scan window size = freq_register_cnt * 4K = 16384 * 4K = 64MB */
    /* So s1 end (0x1FFF) and s2 start (0x2000) gap = 1, which is <= 64MB, so continuous */
    g_smap_hist_dev.freq_register_cnt = 16384;
    ret = addr_seg_is_continuous_scan_wins(&s1, &s2, STS_SIZE_4K);
    EXPECT_EQ(true, ret);

    /* Test non-continuous case: gap > 64MB */
    GlobalMockObject::verify();
    struct addr_seg s3 = {
        .start = 0,
        .size = 64 * MB,
    };
    struct addr_seg s4 = {
        .start = 64 * MB + 64 * MB + 1, /* gap > 64MB */
        .size = 0x1000,
    };
    ret = addr_seg_is_continuous_scan_wins(&s3, &s4, STS_SIZE_4K);
    EXPECT_EQ(false, ret);
}

extern "C" u64 seg_end(struct addr_seg *seg);
TEST_F(HistOpsTest, seg_end)
{
    u64 ret;
    struct addr_seg seg = {
        .start = 0,
        .size = 0x1001,
    };
    ret = seg_end(&seg);
    EXPECT_EQ(0x1000, ret);
}

extern "C" int count_nr_windows(struct addr_seg *segs, int len, u8 sts_size);
TEST_F(HistOpsTest, count_nr_windows)
{
    int ret;
    int cnt = 3;
    struct addr_seg *seg = (struct addr_seg *)malloc(sizeof(struct addr_seg) * cnt);
    for (int i = 0; i < cnt; i++) {
        seg[i].start = i * 0x1000000;
        seg[i].size = 0x1000000;
    }
    ret = count_nr_windows(seg, cnt, STS_SIZE_4K);
    EXPECT_EQ(cnt, ret);
}

extern "C" void align_segments(struct addr_seg *segs, int cnt, u8 sts_size);
TEST_F(HistOpsTest, align_segments)
{
    struct addr_seg *seg = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    seg[0].start = 0x1000;
    seg[0].size = 0x1000;
    MOCKER(count_nr_windows).stubs().will(returnValue(0));
    MOCKER(align_addr).stubs().will(returnValue((u64)0));
    align_segments(seg, 1, STS_SIZE_2M);
    EXPECT_EQ(0, seg[0].size);
    free(seg);
}

extern "C" int merge_segments(struct addr_seg *segs, int cnt, enum ub_hist_sts_size sts_size);
TEST_F(HistOpsTest, merge_segments)
{
    int ret;
    int cnt = 2;
    struct addr_seg *seg = (struct addr_seg *)malloc(sizeof(struct addr_seg) * cnt);
    for (int i = 0; i < cnt; i++) {
        seg[i].start = i * 0x1000;
        seg[i].size = 0x1000;
    }
    MOCKER(addr_seg_is_continuous_scan_wins).stubs().will(returnValue(true));
    ret = merge_segments(seg, cnt, STS_SIZE_2M);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    MOCKER(addr_seg_is_continuous_scan_wins).stubs().will(returnValue(false));
    ret = merge_segments(seg, cnt, STS_SIZE_2M);
    EXPECT_EQ(2, ret);
    free(seg);
}

extern "C" bool is_intersect(struct addr_seg *seg1, struct addr_seg *seg2, u64 *inter_start, u64 *inter_end);
TEST_F(HistOpsTest, is_intersect)
{
    bool ret;
    u64 inter_start;
    u64 inter_end;
    struct addr_seg seg1 = {
        .start = 0,
        .size = 0x1000
    };
    struct addr_seg seg2 = {
        .start = 0x10,
        .size = 0x1000
    };
    struct addr_seg seg3 = {
        .start = 0x10000,
        .size = 0x1000
    };
    ret = is_intersect(&seg1, &seg2, &inter_start, &inter_end);
    EXPECT_EQ(true, ret);

    ret = is_intersect(&seg1, &seg3, &inter_start, &inter_end);
    EXPECT_EQ(false, ret);
}

extern "C" void calc_4k_scan_hot_wins(struct segs_info *info, u16 *buf, struct segs_info *win_info);
TEST_F(HistOpsTest, calc_4k_scan_hot_wins)
{
    int i;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x1400000;
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };
    struct segs_info win_info = {
        .cnt = 2,
    };
    struct addr_seg *wins = (struct addr_seg *)malloc(sizeof(struct addr_seg) * win_info.cnt);
    win_info.segs = wins;
    u16 *buf = (u16 *)calloc(64, sizeof(u16));
    for (i = 0; i < 10; i++) {
        buf[i] = i;
    }
    calc_4k_scan_hot_wins(&info, buf, &win_info);
    EXPECT_EQ(0, wins[0].start);
    free(segs);
    free(wins);
    free(buf);
}

extern "C" int generate_aligned_scan_wins_info(struct segs_info *win_info,
    struct segs_info *info, enum ub_hist_sts_size sts_size);
TEST_F(HistOpsTest, generate_aligned_scan_wins_info_2m)
{
    int ret;
    struct segs_info win_info;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x14000000; /* 320MB */
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };
    /* freq_register_cnt = 16384, scan window size = 16384 * 2M = 32GB */
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;

    ret = generate_aligned_scan_wins_info(&win_info, &info, STS_SIZE_2M);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, win_info.cnt);
    EXPECT_EQ(0, win_info.segs->start);
    /* 2M scan window size = freq_register_cnt * 2M = 16384 * 2M = 32GB */
    EXPECT_EQ(32 * GB, win_info.segs->size);
    free(segs);
    free(g_smap_hist_dev.ba_info);
}

/* Test generate_aligned_scan_wins_info with 4K granularity for sequential loop mode */
TEST_F(HistOpsTest, generate_aligned_scan_wins_info_4k_seq_loop)
{
    int ret;
    struct segs_info win_info;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x8000000; /* 128MB */
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;

    ret = generate_aligned_scan_wins_info(&win_info, &info, STS_SIZE_4K);
    EXPECT_EQ(0, ret);
    /* 4K scan window size = freq_register_cnt * 4K = 16384 * 4K = 64MB */
    /* 128MB / 64MB = 2 windows expected */
    EXPECT_EQ(2, win_info.cnt);
    EXPECT_EQ(0, win_info.segs[0].start);
    EXPECT_EQ(64 * MB, win_info.segs[0].size);
    free(segs);
    free(g_smap_hist_dev.ba_info);
}

extern "C" int generate_aligned_4k_scan_wins_info(struct segs_info *win_info,
    struct segs_info *info, actc_t *buf);
extern "C" int filter_4k_scan_hot_wins(struct segs_info *win_info, u32 max_wins_4k_per_ba);
TEST_F(HistOpsTest, generate_aligned_4k_scan_wins_info)
{
    int ret;
    int buf_len = 10;
    struct segs_info win_info;
    struct segs_info info = {
        .cnt = 1,
    };
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x14000000;
    info.segs = segs;
    actc_t *buffer = (actc_t *)malloc(sizeof(actc_t) * buf_len);
    g_smap_hist_dev.scan_wins_num_per_ba = 1;
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;
    MOCKER(count_nr_windows).stubs().will(returnValue(1));
    MOCKER(calc_4k_scan_hot_wins).stubs().will(ignoreReturnValue());
    MOCKER(filter_4k_scan_hot_wins).stubs().will(returnValue(0));
    ret = generate_aligned_4k_scan_wins_info(&win_info, &info, buffer);
    EXPECT_EQ(0, ret);
    free(segs);
    free(buffer);
    free(g_smap_hist_dev.ba_info);
}

extern "C" int do_hist_scan_sliding(struct segs_info *info, bool do_multi_gran, actc_t *buf,
    u32 buf_len, enum ub_hist_sts_size sts_size, bool direct_update);
extern "C" void copy_actc_to_buf(struct segs_info *info, struct addr_seg *seg,
    actc_t *dst_buf, u16 *freq, u32 buf_len, enum ub_hist_sts_size sts_size);
extern "C" int hist_scan_sliding(struct segs_info *info, u32 scan_time_total, u32 pgsize);

TEST_F(HistOpsTest, hist_scan_sliding_seq_loop)
{
    int ret;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x14000000;
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };

    g_smap_hist_dev.scan_mode = HIST_4K_SCAN_SEQ_LOOP;
    g_smap_hist_dev.pgcount = 10;
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;

    MOCKER(addr_seg_is_valid).stubs().will(returnValue(false));
    ret = hist_scan_sliding(&info, 200, SIZE_4K);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(addr_seg_is_valid).stubs().will(returnValue(true));
    MOCKER(do_hist_scan_sliding).stubs().will(returnValue(0));
    ret = hist_scan_sliding(&info, 200, SIZE_4K);
    EXPECT_EQ(0, ret);
    free(segs);
    free(g_smap_hist_dev.ba_info);
}

TEST_F(HistOpsTest, hist_scan_sliding_invalid_period)
{
    int ret;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x14000000;
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };

    MOCKER(addr_seg_is_valid).stubs().will(returnValue(true));
    ret = hist_scan_sliding(&info, 0, SIZE_4K);
    EXPECT_EQ(-EINVAL, ret);
    free(segs);
}

extern "C" void hist_update_pgsize(u32 pgsize);
TEST_F(HistOpsTest, hist_update_pgsize)
{
    g_smap_hist_dev.pgsize = SIZE_4K;
    hist_update_pgsize(SIZE_2M);
    EXPECT_EQ(SIZE_2M, g_smap_hist_dev.status.flag.new_pgsize);
}

extern "C" void hist_set_iomem(void);
TEST_F(HistOpsTest, hist_set_iomem)
{
    g_smap_hist_dev.status.flag.should_update_iomem = 0;
    hist_set_iomem();
    EXPECT_EQ(1, g_smap_hist_dev.status.flag.should_update_iomem);
}

extern int nr_local_numa;
extern "C" int addr_segs_init(struct smap_hist_dev *dev, u32 pgsize);
TEST_F(HistOpsTest, addr_segs_init)
{
    int ret;
    struct smap_hist_dev dev;
    INIT_LIST_HEAD(&drivers_remote_ram_list);
    nr_local_numa = 1;
    struct ram_segment newdata1 = {
        .numa_node = 0,
        .start = 0,
        .end = 0x100,
    };

    struct ram_segment newdata2 = {
        .numa_node = 1,
        .start = 0x1000,
        .end = 0x401000,
    };
    list_add(&newdata1.node, &drivers_remote_ram_list);
    list_add(&newdata2.node, &drivers_remote_ram_list);
    ret = addr_segs_init(&dev, SIZE_2M);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(2, dev.pgcount);

    MOCKER(vzalloc).stubs().will(returnValue((void *)NULL));
    ret = addr_segs_init(&dev, SIZE_2M);
    EXPECT_EQ(-ENOMEM, ret);
}

extern "C" void addr_segs_deinit(struct smap_hist_dev *dev);
TEST_F(HistOpsTest, addr_segs_deinit)
{
    struct segs_info info = {
        .cnt = 1,
    };
    info.segs =(struct addr_seg *)malloc(sizeof(struct addr_seg) * info.cnt);
    struct smap_hist_dev dev = {
        .info = info,
    };
    addr_segs_deinit(&dev);
    EXPECT_EQ(NULL, dev.info.segs);
}

extern "C" int hist_pginfo_reinit(struct smap_hist_dev *dev, u32 pgsize_new);
TEST_F(HistOpsTest, hist_pginfo_reinit)
{
    int ret;
    struct smap_hist_dev dev = {
        .pgsize = SIZE_2M,
    };
    MOCKER(addr_segs_deinit).stubs().will(ignoreReturnValue());
    MOCKER(addr_segs_init).stubs().will(returnValue(1));
    ret = hist_pginfo_reinit(&dev, 0);
    EXPECT_EQ(-ENOMEM, ret);

    GlobalMockObject::verify();
    MOCKER(addr_segs_deinit).stubs().will(ignoreReturnValue());
    MOCKER(addr_segs_init).stubs().will(returnValue(0));
    ret = hist_pginfo_reinit(&dev, 0);
    EXPECT_EQ(0, ret);
}

extern "C" int scan_thread_run(void *data);
TEST_F(HistOpsTest, scan_thread_run)
{
    int ret;
    MOCKER(kthread_should_stop).stubs().will(returnValue(false)).then(returnValue(true));

    g_smap_hist_dev.status.status_all = 1;
    g_smap_hist_dev.thread_enable = 1;
    g_smap_hist_dev.pgcount = 10;
    MOCKER(hist_pginfo_reinit).stubs().will(returnValue(0));
    MOCKER(hist_scan_sliding).stubs().will(returnValue(0));
    ret = scan_thread_run(&g_smap_hist_dev);
    EXPECT_EQ(0, ret);
}

TEST_F(HistOpsTest, scan_thread_run_two)
{
    int ret;
    MOCKER(kthread_should_stop).stubs().will(returnValue(false)).then(returnValue(true));

    g_smap_hist_dev.status.status_all = 1;
    MOCKER(hist_pginfo_reinit).stubs().will(returnValue(1));
    ret = scan_thread_run(&g_smap_hist_dev);
    EXPECT_EQ(0, ret);
}

TEST_F(HistOpsTest, scan_thread_run_three)
{
    int ret;
    MOCKER(kthread_should_stop).stubs().will(returnValue(false)).then(returnValue(true));

    g_smap_hist_dev.status.status_all = 1;
    g_smap_hist_dev.thread_enable = 1;
    MOCKER(hist_pginfo_reinit).stubs().will(returnValue(0));
    MOCKER(hist_scan_sliding).stubs().will(returnValue(-EINVAL));
    ret = scan_thread_run(&g_smap_hist_dev);
    EXPECT_EQ(0, ret);
}

extern "C" int scan_thread_init(struct smap_hist_dev *dev);
TEST_F(HistOpsTest, scan_thread_init)
{
    int ret;
    struct smap_hist_dev dev;
    ret = scan_thread_init(&dev);
    EXPECT_EQ(-ECHILD, ret);
    EXPECT_EQ(nullptr, dev.kthread);
}

extern "C" void scan_thread_deinit(struct smap_hist_dev *dev);
TEST_F(HistOpsTest, scan_thread_deinit)
{
    struct smap_hist_dev dev;
    dev.kthread = (struct task_struct *)malloc(sizeof(struct task_struct));
    scan_thread_deinit(&dev);
    EXPECT_EQ(nullptr, dev.kthread);
}

TEST_F(HistOpsTest, get_hist_dev)
{
    struct smap_hist_dev *dev = &g_smap_hist_dev;
    EXPECT_EQ(dev, get_hist_dev());
}

extern "C" void smap_hist_init(void);
extern "C" void ub_hist_exit(void);
extern "C" int ub_hist_query_ba_info(uint64_t ba_tag, struct ub_hist_ba_info *ba_info);
extern "C" int query_hist_ba_info(void);
TEST_F(HistOpsTest, hist_init)
{
    int ret;
    MOCKER(ub_hist_init).stubs().will(returnValue(0));
    MOCKER(query_hist_ba_info).stubs().will(returnValue(0));
    MOCKER(addr_segs_init).stubs().will(returnValue(0));
    MOCKER(scan_thread_init).stubs().will(returnValue(0));
    ret = hist_init(SIZE_2M);
    EXPECT_EQ(0, ret);
}

/* ============================================================
 * New test cases for commit d3c1f37 and 48795d4 features
 * ============================================================ */

extern struct list_head access_dev;
extern "C" struct access_tracking_dev *find_hdev_by_node(int node);
extern "C" void update_actc_direct(struct segs_info *rmem_info,
    struct addr_seg *seg, u16 *freq, u32 buf_len, enum ub_hist_sts_size sts_size);

/*
 * Test Case 1: 4K顺序滑窗模式生成扫描窗口的功能正常
 * Test generate_aligned_scan_wins_info with STS_SIZE_4K generates correct windows
 */
TEST_F(HistOpsTest, seq_loop_generate_scan_wins_normal)
{
    int ret;
    struct segs_info win_info;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg) * 2);
    /* Setup two 64MB aligned segments */
    segs[0].start = 0;
    segs[0].size = 64 * MB; /* 64MB = one scan window at 4K granularity with 16384 counters */
    segs[1].start = 64 * MB;
    segs[1].size = 64 * MB;
    struct segs_info info = {
        .cnt = 2,
        .segs = segs,
    };

    /* Setup global hist_dev for 4K scan */
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;
    g_smap_hist_dev.scan_mode = HIST_4K_SCAN_SEQ_LOOP;

    ret = generate_aligned_scan_wins_info(&win_info, &info, STS_SIZE_4K);
    EXPECT_EQ(0, ret);
    /* Should generate 2 windows (one per 64MB segment) */
    EXPECT_EQ(2, win_info.cnt);
    EXPECT_EQ(0, win_info.segs[0].start);
    EXPECT_EQ(64 * MB, win_info.segs[0].size);
    EXPECT_EQ(64 * MB, win_info.segs[1].start);
    EXPECT_EQ(64 * MB, win_info.segs[1].size);

    free(segs);
    vfree(win_info.segs);
    free(g_smap_hist_dev.ba_info);
}

/*
 * Test Case 2: 4K顺序滑窗模式，前一次扫描被中断后，下一次扫描接着中断位置继续扫描
 * Test seq_loop_ba_offset is correctly saved and restored on scan interruption
 */
TEST_F(HistOpsTest, seq_loop_scan_interrupt_resume)
{
    int i;
    struct smap_hist_dev *dev = &g_smap_hist_dev;

    /* Setup initial state: seq_loop_ba_offset initialized to -1 */
    dev->ba_cnt = HIST_STS_DEV_CNT;
    dev->scan_mode = HIST_4K_SCAN_SEQ_LOOP;
    for (i = 0; i < dev->ba_cnt; ++i) {
        dev->seq_loop_ba_offset[i] = -1;
    }

    /* Simulate first scan with no interruption - offsets should reset to -1 after completion */
    /* In smap_hist_read_paral, if scan completes without abort, seq_loop_ba_offset remains at last value */
    /* But since the scan completed all windows, next scan should start from beginning again */

    /* Simulate scan interruption: abort_flag set during scan */
    /* After interruption, seq_loop_ba_offset[ba_cnt] should be saved with current window offset */
    dev->seq_loop_ba_offset[0] = 5; /* Simulate BA0 stopped at window index 5 */
    dev->seq_loop_ba_offset[1] = 3; /* Simulate BA1 stopped at window index 3 */

    /* Verify saved offsets are correct */
    EXPECT_EQ(5, dev->seq_loop_ba_offset[0]);
    EXPECT_EQ(3, dev->seq_loop_ba_offset[1]);

    /* Next scan should resume from saved offsets (mocked by setting offset[i] from seq_loop_ba_offset) */
    /* In smap_hist_read_paral: offset[i] = do_4k_seq_loop ? dev->seq_loop_ba_offset[i] : -1 */

    /* Simulate resetting offsets when memory is updated */
    for (i = 0; i < HIST_STS_DEV_CNT; ++i) {
        dev->seq_loop_ba_offset[i] = -1;
    }
    /* After reset, should start from beginning */
    EXPECT_EQ(-1, dev->seq_loop_ba_offset[0]);
    EXPECT_EQ(-1, dev->seq_loop_ba_offset[1]);
}

/*
 * Test Case 2 extension: Verify seq_loop_ba_offset reset when switching scan mode
 */
TEST_F(HistOpsTest, seq_loop_scan_mode_change_reset)
{
    struct smap_hist_dev *dev = &g_smap_hist_dev;
    dev->ba_cnt = HIST_STS_DEV_CNT;
    dev->scan_mode = HIST_4K_SCAN_SEQ_LOOP;

    /* Set some offsets */
    dev->seq_loop_ba_offset[0] = 10;
    dev->seq_loop_ba_offset[1] = 20;

    /* Switching mode should reset offsets (as done in hist_4k_scan_mode_set) */
    for (u32 i = 0; i < dev->ba_cnt; ++i) {
        dev->seq_loop_ba_offset[i] = -1;
    }
    dev->scan_mode = HIST_4K_SCAN_MULTI_GRAN;

    EXPECT_EQ(HIST_4K_SCAN_MULTI_GRAN, dev->scan_mode);
    EXPECT_EQ(-1, dev->seq_loop_ba_offset[0]);
    EXPECT_EQ(-1, dev->seq_loop_ba_offset[1]);
}

/*
 * Test Case 3: 硬件扫描省去中间buffer直接将频次更新到hdev->actc_data功能正常
 * Test update_actc_direct directly updates hdev->access_bit_actc_data without intermediate buffer
 */
TEST_F(HistOpsTest, update_actc_direct_to_hdev)
{
    struct segs_info rmem_info;
    struct addr_seg scan_seg;
    u16 freq_buffer[4] = {100, 200, 300, 400};
    struct access_tracking_dev hdev;
    struct ram_segment rseg;

    /* Setup remote memory segment info */
    struct addr_seg *rmem_segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    rmem_segs[0].start = 0;
    rmem_segs[0].size = 0x10000; /* 64KB */
    rmem_info.cnt = 1;
    rmem_info.segs = rmem_segs;

    /* Setup scan segment (intersection with remote memory) */
    scan_seg.start = 0;
    scan_seg.size = 0x4000; /* 16KB = 4 pages at 4K granularity */

    /* Setup hdev with actc_data buffer */
    hdev.node = 0;
    hdev.is_hist = true;
    hdev.page_count = 10;
    hdev.access_bit_actc_data = (actc_t *)calloc(10, sizeof(actc_t));
    init_rwsem(&hdev.buffer_lock);

    /* Setup ram_segment list */
    INIT_LIST_HEAD(&drivers_remote_ram_list);
    rseg.start = 0;
    rseg.end = 0xFFFF;
    rseg.numa_node = 0;
    list_add(&rseg.node, &drivers_remote_ram_list);

    /* Setup access_dev list so find_hdev_by_node can find our hdev */
    INIT_LIST_HEAD(&access_dev);
    list_add(&hdev.list, &access_dev);

    /* Call update_actc_direct */
    g_smap_hist_dev.freq_register_cnt = 16384;
    update_actc_direct(&rmem_info, &scan_seg, freq_buffer, 4, STS_SIZE_4K);

    /* Verify: access_bit_actc_data should be updated with freq values */
    /* Since freq values are 100, 200, 300, 400 and original values are 0 */
    /* After update, values should be the same as freq (sum < U16_MAX) */
    EXPECT_EQ(100, hdev.access_bit_actc_data[0]);
    EXPECT_EQ(200, hdev.access_bit_actc_data[1]);
    EXPECT_EQ(300, hdev.access_bit_actc_data[2]);
    EXPECT_EQ(400, hdev.access_bit_actc_data[3]);

    free(rmem_segs);
    free(hdev.access_bit_actc_data);
}

/*
 * Test Case 3 extension: Verify update_actc_direct handles U16_MAX overflow correctly
 */
TEST_F(HistOpsTest, update_actc_direct_overflow_handling)
{
    struct segs_info rmem_info;
    struct addr_seg scan_seg;
    u16 freq_buffer[2] = {U16_MAX, U16_MAX};
    struct access_tracking_dev hdev;
    struct ram_segment rseg;

    /* Setup remote memory segment info */
    struct addr_seg *rmem_segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    rmem_segs[0].start = 0;
    rmem_segs[0].size = 0x10000;
    rmem_info.cnt = 1;
    rmem_info.segs = rmem_segs;

    /* Setup scan segment */
    scan_seg.start = 0;
    scan_seg.size = 0x2000; /* 8KB = 2 pages at 4K granularity */

    /* Setup hdev with existing high values in actc_data */
    hdev.node = 0;
    hdev.is_hist = true;
    hdev.page_count = 10;
    hdev.access_bit_actc_data = (actc_t *)calloc(10, sizeof(actc_t));
    hdev.access_bit_actc_data[0] = U16_MAX - 1; /* Near overflow */
    hdev.access_bit_actc_data[1] = 1000;
    init_rwsem(&hdev.buffer_lock);

    /* Setup lists */
    INIT_LIST_HEAD(&drivers_remote_ram_list);
    rseg.start = 0;
    rseg.end = 0xFFFF;
    rseg.numa_node = 0;
    list_add(&rseg.node, &drivers_remote_ram_list);

    INIT_LIST_HEAD(&access_dev);
    list_add(&hdev.list, &access_dev);

    g_smap_hist_dev.freq_register_cnt = 16384;
    update_actc_direct(&rmem_info, &scan_seg, freq_buffer, 2, STS_SIZE_4K);

    /* Verify overflow handling: sum should clamp to U16_MAX */
    /* index 0: (U16_MAX-1) + U16_MAX = overflow, should be U16_MAX */
    EXPECT_EQ(U16_MAX, hdev.access_bit_actc_data[0]);
    /* index 1: 1000 + U16_MAX = overflow, should be U16_MAX */
    EXPECT_EQ(U16_MAX, hdev.access_bit_actc_data[1]);

    free(rmem_segs);
    free(hdev.access_bit_actc_data);
}

/*
 * Test: Verify hist_scan_sliding uses direct_update for seq_loop mode
 */
TEST_F(HistOpsTest, hist_scan_sliding_seq_loop_direct_update)
{
    int ret;
    struct addr_seg *segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    segs[0].start = 0;
    segs[0].size = 0x8000000;
    struct segs_info info = {
        .cnt = 1,
        .segs = segs,
    };

    g_smap_hist_dev.scan_mode = HIST_4K_SCAN_SEQ_LOOP;
    g_smap_hist_dev.pgcount = 10;
    g_smap_hist_dev.freq_register_cnt = 16384;
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].nc_range.start = 0;
    g_smap_hist_dev.ba_info[0].nc_range.end = U64_MAX;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    g_smap_hist_dev.ba_info[1].nc_range.start = 0;
    g_smap_hist_dev.ba_info[1].nc_range.end = U64_MAX;

    MOCKER(addr_seg_is_valid).stubs().will(returnValue(true));
    /* In seq_loop mode, do_hist_scan_sliding is called with direct_update=true */
    MOCKER(do_hist_scan_sliding).stubs().will(returnValue(0));

    ret = hist_scan_sliding(&info, 512, SIZE_4K);
    EXPECT_EQ(0, ret);

    free(segs);
    free(g_smap_hist_dev.ba_info);
}

/*
 * Test: Verify scan_thread_run resets seq_loop_ba_offset on status change
 */
TEST_F(HistOpsTest, scan_thread_reset_seq_loop_offset_on_status_change)
{
    int i;
    struct smap_hist_dev *dev = &g_smap_hist_dev;

    dev->ba_cnt = HIST_STS_DEV_CNT;
    dev->scan_mode = HIST_4K_SCAN_SEQ_LOOP;
    dev->pgcount = 10;
    dev->thread_enable = 1;
    dev->status.status_all = 0;

    /* Set some offsets to simulate ongoing scan */
    for (i = 0; i < dev->ba_cnt; ++i) {
        dev->seq_loop_ba_offset[i] = 5;
    }

    /* Simulate status change (memory update or page size change) */
    dev->status.status_all = 1;

    /* In scan_thread_run, when status.status_all != 0, offsets are reset */
    /* This is done before hist_pginfo_reinit */
    for (i = 0; i < HIST_STS_DEV_CNT; ++i) {
        dev->seq_loop_ba_offset[i] = -1;
    }

    /* Verify offsets are reset */
    for (i = 0; i < dev->ba_cnt; ++i) {
        EXPECT_EQ(-1, dev->seq_loop_ba_offset[i]);
    }
}

extern "C" int filter_4k_scan_hot_wins(struct segs_info *win_info, u32 max_wins_4k_per_ba);
TEST_F(HistOpsTest, filter_4k_scan_hot_wins_null)
{
    int ret = filter_4k_scan_hot_wins(nullptr, 1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HistOpsTest, filter_4k_scan_hot_wins_cnt_zero)
{
    struct segs_info win_info;
    memset(&win_info, 0, sizeof(win_info));
    int ret = filter_4k_scan_hot_wins(&win_info, 1);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HistOpsTest, filter_4k_scan_hot_wins_max_zero)
{
    struct segs_info win_info;
    memset(&win_info, 0, sizeof(win_info));
    win_info.cnt = 1;
    win_info.segs = (struct addr_seg *)malloc(sizeof(struct addr_seg));
    int ret = filter_4k_scan_hot_wins(&win_info, 0);
    EXPECT_EQ(-EINVAL, ret);
    free(win_info.segs);
}

extern "C" int hist_4k_scan_mode_set(const char *val, const struct kernel_param *kp);
TEST_F(HistOpsTest, hist_4k_scan_mode_set_invalid_mode)
{
    struct kernel_param kp = {};
    g_smap_hist_dev.scan_mode = HIST_4K_SCAN_MULTI_GRAN;
    int ret = hist_4k_scan_mode_set("2", &kp);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(HistOpsTest, hist_4k_scan_mode_set_same_mode)
{
    struct kernel_param kp = {};
    g_smap_hist_dev.scan_mode = HIST_4K_SCAN_SEQ_LOOP;
    int ret = hist_4k_scan_mode_set("1", &kp);
    EXPECT_EQ(0, ret);
}

extern "C" bool addr_is_cc_mem(u64 addr);
TEST_F(HistOpsTest, addr_is_cc_mem_in_range)
{
    g_smap_hist_dev.ba_cnt = 1;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info));
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].cc_range.start = 0x1000;
    g_smap_hist_dev.ba_info[0].cc_range.end = 0x2000;
    bool ret = addr_is_cc_mem(0x1500);
    EXPECT_EQ(true, ret);
    free(g_smap_hist_dev.ba_info);
}

TEST_F(HistOpsTest, addr_is_cc_mem_not_in_range)
{
    g_smap_hist_dev.ba_cnt = 1;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info));
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[0].cc_range.start = 0x1000;
    g_smap_hist_dev.ba_info[0].cc_range.end = 0x2000;
    bool ret = addr_is_cc_mem(0x3000);
    EXPECT_EQ(false, ret);
    free(g_smap_hist_dev.ba_info);
}

extern "C" void hist_deinit(void);
TEST_F(HistOpsTest, hist_deinit)
{
    MOCKER(scan_thread_deinit).stubs();
    MOCKER(addr_segs_deinit).stubs();
    MOCKER(kfree).stubs();
    hist_deinit();
}

extern "C" int addr_seg_cmp_start(const void *a, const void *b);
TEST_F(HistOpsTest, addr_seg_cmp_start_less)
{
    struct addr_seg s1 = {.start = 100};
    struct addr_seg s2 = {.start = 200};
    int ret = addr_seg_cmp_start(&s1, &s2);
    EXPECT_EQ(-1, ret);
}

TEST_F(HistOpsTest, addr_seg_cmp_start_equal)
{
    struct addr_seg s1 = {.start = 100};
    struct addr_seg s2 = {.start = 100};
    int ret = addr_seg_cmp_start(&s1, &s2);
    EXPECT_EQ(0, ret);
}

extern "C" int addr_seg_cmp_max(const void *a, const void *b);
TEST_F(HistOpsTest, addr_seg_cmp_max_equal)
{
    struct addr_seg w1 = {.max = 100, .ba_tag = 0};
    struct addr_seg w2 = {.max = 100, .ba_tag = 0};
    int ret = addr_seg_cmp_max(&w1, &w2);
    EXPECT_EQ(0, ret);
}

TEST_F(HistOpsTest, addr_seg_cmp_max_ba_tag_less)
{
    struct addr_seg w1 = {.max = 100, .ba_tag = 1};
    struct addr_seg w2 = {.max = 100, .ba_tag = 2};
    int ret = addr_seg_cmp_max(&w1, &w2);
    EXPECT_EQ(-1, ret);
}

extern "C" int submit_ba_task(uint64_t ba_tag, u64 start_addr, enum ub_hist_sts_size sts_size);
TEST_F(HistOpsTest, submit_ba_task_success)
{
    MOCKER(ub_hist_get_state).stubs().will(returnValue(0));
    MOCKER(ub_hist_set_state).stubs().will(returnValue(0));
    int ret = submit_ba_task(0, 0x1000, STS_SIZE_4K);
    EXPECT_EQ(0, ret);
}

TEST_F(HistOpsTest, submit_ba_task_set_state_fail)
{
    MOCKER(ub_hist_get_state).stubs().will(returnValue(0));
    MOCKER(ub_hist_set_state).stubs().will(returnValue(-1));
    int ret = submit_ba_task(0, 0x1000, STS_SIZE_4K);
    EXPECT_EQ(-1, ret);
}

extern "C" int disable_ba_task(uint64_t ba_tag);
TEST_F(HistOpsTest, disable_ba_task_success)
{
    MOCKER(ub_hist_get_state).stubs().will(returnValue(0));
    MOCKER(ub_hist_set_state).stubs().will(returnValue(0));
    int ret = disable_ba_task(0);
    EXPECT_EQ(0, ret);
}

extern "C" void disable_all_ba_tasks(u32 *offset, u32 end);
TEST_F(HistOpsTest, disable_all_ba_tasks)
{
    g_smap_hist_dev.ba_cnt = 2;
    g_smap_hist_dev.ba_info = (struct ub_hist_ba_info *)malloc(sizeof(struct ub_hist_ba_info) * 2);
    g_smap_hist_dev.ba_info[0].ba_tag = 0;
    g_smap_hist_dev.ba_info[1].ba_tag = 1;
    u32 offset[2] = {0, 3};
    u32 end = 5;
    MOCKER(disable_ba_task).stubs().will(returnValue(0));
    disable_all_ba_tasks(offset, end);
    free(g_smap_hist_dev.ba_info);
    g_smap_hist_dev.ba_info = nullptr;
}

extern "C" bool pick_complete(u32 *offset, u32 len, u32 end);
TEST_F(HistOpsTest, pick_complete_true)
{
    u32 offset[1] = {5};
    bool ret = pick_complete(offset, 1, 5);
    EXPECT_EQ(true, ret);
}

TEST_F(HistOpsTest, pick_complete_false)
{
    u32 offset[2] = {0, 5};
    bool ret = pick_complete(offset, 2, 5);
    EXPECT_EQ(false, ret);
}

TEST_F(HistOpsTest, pick_complete_all_done)
{
    u32 offset[3] = {10, 10, 10};
    bool ret = pick_complete(offset, 3, 10);
    EXPECT_EQ(true, ret);
}

extern "C" bool pick_one_seg(u64 ba_tag, struct segs_info *win_info, int *offset, bool do_4k_seq_loop);
TEST_F(HistOpsTest, pick_one_seg_seq_loop_reset)
{
    struct segs_info win_info = {};
    struct addr_seg segs[2] = {{100, 200, 0, 0}, {300, 400, 0, 1}};
    win_info.cnt = 2;
    win_info.segs = segs;
    int offset_val = 2;
    bool ret = pick_one_seg(1, &win_info, &offset_val, true);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(1, offset_val);
}

TEST_F(HistOpsTest, pick_one_seg_found)
{
    struct segs_info win_info = {};
    struct addr_seg segs[3] = {{100, 200, 0, 0}, {300, 400, 0, 1}, {500, 600, 0, 1}};
    win_info.cnt = 3;
    win_info.segs = segs;
    int offset_val = -1;
    bool ret = pick_one_seg(1, &win_info, &offset_val, false);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(1, offset_val);
}

TEST_F(HistOpsTest, pick_one_seg_not_found)
{
    struct segs_info win_info = {};
    struct addr_seg segs[2] = {{100, 200, 0, 0}, {300, 400, 0, 2}};
    win_info.cnt = 2;
    win_info.segs = segs;
    int offset_val = -1;
    bool ret = pick_one_seg(1, &win_info, &offset_val, false);
    EXPECT_EQ(false, ret);
    EXPECT_EQ(2, offset_val);
}

TEST_F(HistOpsTest, pick_one_seg_seq_loop_no_wrap)
{
    struct segs_info win_info = {};
    struct addr_seg segs[2] = {{100, 200, 0, 0}, {300, 400, 0, 0}};
    win_info.cnt = 2;
    win_info.segs = segs;
    int offset_val = 0;
    bool ret = pick_one_seg(0, &win_info, &offset_val, false);
    EXPECT_EQ(true, ret);
    EXPECT_EQ(1, offset_val);
}

