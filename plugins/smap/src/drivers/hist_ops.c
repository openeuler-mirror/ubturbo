// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP : hist_ops
 */
#include <asm/types.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>
#include <linux/mmzone.h>
#include <linux/pfn.h>
#include <linux/sort.h>
#include <linux/kthread.h>
#include <linux/minmax.h>
#include <linux/cpumask.h>
#include <linux/topology.h>

#include "access_iomem.h"
#include "access_acpi_mem.h"
#include "ub_hist.h"
#include "hist_ops.h"

#define HOT_WINDOW_RATIO (10)
#define SCAN_INTERVAL_RANGE_US 1000

static struct smap_hist_dev g_smap_hist_dev;

static inline u64 align_addr(u64 addr, u32 low_bit_len)
{
	return (addr >> low_bit_len) << low_bit_len;
}

static inline bool addr_is_aligned(u64 addr, u32 low_bit_len)
{
	return !(addr & ((1ULL << low_bit_len) - 1));
}

static inline u64 get_hist_scan_win_size(enum ub_hist_sts_size sts_size)
{
	u64 hist_win_size;
	u64 page_size;

	page_size = (sts_size == STS_SIZE_2M) ? SIZE_2M : SIZE_4K;
	hist_win_size = g_smap_hist_dev.freq_register_cnt * page_size;
	return hist_win_size;
}

static inline u32 get_hist_addr_shift(enum ub_hist_sts_size sts_size)
{
	u32 shift;
	u64 hist_win_size;

	hist_win_size = get_hist_scan_win_size(sts_size);
	for (shift = 0; hist_win_size > 1; shift++) {
		hist_win_size >>= 1;
	}
	return shift;
}

static inline u64 align_addr_by_sts_size(u64 addr,
					 enum ub_hist_sts_size sts_size)
{
	u32 shift = get_hist_addr_shift(sts_size);
	return align_addr(addr, shift);
}

static inline bool paddr_within_ba(u64 pa, struct ub_hist_ba_info *ba_info)
{
	if (ba_info->cc_range.start <= pa && pa < ba_info->cc_range.end)
		return true;
	if (ba_info->nc_range.start <= pa && pa < ba_info->nc_range.end)
		return true;
	return false;
}

static int calc_ba_tag(u64 start_addr, uint64_t *ba_tag)
{
	u64 i;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	for (i = 0; i < dev->ba_cnt; i++) {
		if (paddr_within_ba(start_addr, &dev->ba_info[i])) {
			*ba_tag = dev->ba_info[i].ba_tag;
			return 0;
		}
	}
	return -EINVAL;
}

static bool addr_seg_is_valid(struct segs_info *info)
{
	u32 i;
	u64 start, size, ba_tag;
	if (!info) {
		pr_err("null segment info passed to histogram tracking\n");
		return false;
	}
	if (!info->segs) {
		pr_err("null address segment passed to histogram tracking\n");
		return false;
	}
	for (i = 0; i < info->cnt; i++) {
		start = info->segs[i].start;
		size = info->segs[i].size;
		if (!addr_is_aligned(start, HIST_ADDR_SHIFT_32M) ||
		    !addr_is_aligned(start + size, HIST_ADDR_SHIFT_32M)) {
			pr_err("segment [%d] start address or size %llu is not in 32M-aligned\n",
			       i, size);
			return false;
		}

		if (calc_ba_tag(start, &ba_tag)) {
			pr_err("segment is not belong to remote memory\n");
			return false;
		}
	}
	return true;
}

static int addr_seg_cmp_start(const void *seg1, const void *seg2)
{
	struct addr_seg *s1 = (struct addr_seg *)seg1;
	struct addr_seg *s2 = (struct addr_seg *)seg2;
	if (s1->start > s2->start) {
		return 1;
	} else if (s1->start < s2->start) {
		return -1;
	} else {
		return 0;
	}
}

static int addr_seg_cmp_max(const void *win1, const void *win2)
{
	struct addr_seg *w1, *w2;
	w1 = (struct addr_seg *)win1;
	w2 = (struct addr_seg *)win2;
	if (w1->ba_tag == w2->ba_tag) {
		if (w1->max == w2->max)
			return 0;
		return (w1->max < w2->max) ? 1 : -1;
	}
	return (w1->ba_tag < w2->ba_tag) ? -1 : 1;
}

static inline u64 seg_end(struct addr_seg *seg)
{
	return seg->start + seg->size - 1;
}

static inline bool addr_seg_is_continuous_scan_wins(struct addr_seg *seg1,
						    struct addr_seg *seg2)
{
	u64 scan_win_size = get_hist_scan_win_size(STS_SIZE_2M);
	if (seg2->start - seg_end(seg1) <= scan_win_size)
		return true;

	return false;
}

static u32 count_nr_windows(struct addr_seg *segs, int cnt,
			    enum ub_hist_sts_size sts_size)
{
	u64 start, end;
	u32 total_scans = 0;
	int i;
	u32 shift = get_hist_addr_shift(sts_size);

	for (i = 0; i < cnt; i++) {
		start = segs[i].start >> shift;
		end = seg_end(&segs[i]) >> shift;
		total_scans += (end - start) + 1;
	}
	return total_scans;
}

static void align_segments(struct addr_seg *segs, int cnt,
			   enum ub_hist_sts_size sts_size)
{
	u32 hist_addr_shift = get_hist_addr_shift(sts_size);
	u64 hist_win_size = get_hist_scan_win_size(sts_size);
	int i;
	for (i = 0; i < cnt; i++) {
		u32 nr_wins = count_nr_windows(&segs[i], 1, sts_size);
		segs[i].start = align_addr(segs[i].start, hist_addr_shift);
		segs[i].size = hist_win_size * nr_wins;
	}
}

static int merge_segments(struct addr_seg *segs, int cnt)
{
	int i, merged_cnt = 0;
	for (i = 1; i < cnt; i++) {
		bool is_continuous = addr_seg_is_continuous_scan_wins(
			&segs[merged_cnt], &segs[i]);
		if (is_continuous) {
			segs[merged_cnt].size =
				max_t(u64, seg_end(&segs[merged_cnt]),
				      seg_end(&segs[i])) -
				segs[merged_cnt].start + 1;
		} else {
			merged_cnt++;
			segs[merged_cnt] = segs[i];
		}
	}
	return merged_cnt + 1;
}

static inline bool is_intersect(struct addr_seg *seg1, struct addr_seg *seg2,
				u64 *inter_start, u64 *inter_end)
{
	if (seg_end(seg1) < seg2->start || seg_end(seg2) < seg1->start) {
		return false;
	}
	*inter_start = (seg1->start > seg2->start) ? seg1->start : seg2->start;
	*inter_end = (seg_end(seg1) < seg_end(seg2)) ? seg_end(seg1) :
						       seg_end(seg2);

	return true;
}

static void calc_4k_scan_hot_wins(struct segs_info *info, u16 *buf,
				  struct segs_info *win_info)
{
	u32 seg_pages = 0, win_cnt = 0;
	unsigned int i;
	u64 shift = get_hist_scan_win_size(STS_SIZE_4K);
	for (i = 0; i < info->cnt; i++) {
		u64 start = info->segs[i].start;
		u64 ba_tag;
		calc_ba_tag(start, &ba_tag);
		while (start < seg_end(&info->segs[i])) {
			u16 max_val = 0, nr = shift / SIZE_2M;
			u32 offset = (start - info->segs[i].start) >>
				     HIST_ADDR_SHIFT_2M;
			u16 *buf_ptr = &buf[seg_pages + offset];
			while (nr--) {
				max_val = max_t(u16, max_val, *buf_ptr++);
			}
			if (win_cnt >= win_info->cnt) {
				pr_err("out-of-bounds when calc wins.[nr_wins:%u]\n",
				       win_info->cnt);
				start += shift;
				continue;
			}
			win_info->segs[win_cnt].start = start;
			win_info->segs[win_cnt].size = shift;
			win_info->segs[win_cnt].max = max_val;
			win_info->segs[win_cnt].ba_tag = ba_tag;
			win_cnt++;

			start += shift;
		}
		seg_pages += info->segs[i].size >> HIST_ADDR_SHIFT_2M;
	}
}

static int cut_into_2m_scan_wins(struct addr_seg *segs, int cnt,
				 struct addr_seg *aligned_segs, int aligned_cnt)
{
	int i;
	u32 nr_wins = 0;
	u64 shift = get_hist_scan_win_size(STS_SIZE_2M);
	for (i = 0; i < cnt; i++) {
		u64 start = segs[i].start;
		u64 size = segs[i].size;
		while (size > 0) {
			if (nr_wins >= aligned_cnt) {
				pr_err("out-of-bounds when creating segs.[i:%d, total:%d]\n",
				       nr_wins, aligned_cnt);
				return -EINVAL;
			}
			aligned_segs[nr_wins].start = start;
			aligned_segs[nr_wins].size = shift;
			calc_ba_tag(start, &aligned_segs[nr_wins].ba_tag);
			start += shift;
			size -= shift;
			nr_wins++;
		}
	}
	if (nr_wins == 0)
		return -EINVAL;
	return 0;
}

static int generate_aligned_2m_scan_wins_info(struct segs_info *win_info,
					      struct segs_info *info)
{
	int merged_cnt, ret;
	u32 total_wins;
	struct addr_seg *merged_segs, *aligned_segs;

	if (info->cnt == 0)
		return -EINVAL;

	merged_segs = vzalloc(info->cnt * sizeof(struct addr_seg));
	if (!merged_segs)
		return -ENOMEM;

	memcpy(merged_segs, info->segs, info->cnt * sizeof(struct addr_seg));
	merged_cnt = merge_segments(merged_segs, info->cnt);
	align_segments(merged_segs, merged_cnt, STS_SIZE_2M);
	total_wins = count_nr_windows(merged_segs, merged_cnt, STS_SIZE_2M);
	if (total_wins == 0) {
		vfree(merged_segs);
		return -EINVAL;
	}
	aligned_segs = vzalloc(total_wins * sizeof(struct addr_seg));
	if (!aligned_segs) {
		vfree(merged_segs);
		return -ENOMEM;
	}

	ret = cut_into_2m_scan_wins(merged_segs, merged_cnt, aligned_segs,
				    total_wins);
	if (ret) {
		vfree(aligned_segs);
	} else {
		win_info->cnt = total_wins;
		win_info->segs = aligned_segs;
	}
	vfree(merged_segs);
	return ret;
}

/**
 * Filter windows by BA tag and store qualified 4k windows back to win_info->segs
 * @win_info: Windows set information structure containing all 4k scan windows
 * @max_wins_4k_per_ba: Maximum 4k scan windows to keep per BA
 * Return 0 on success, negative error code on failure 
 */
static int filter_4k_scan_hot_wins(struct segs_info *win_info,
				   u32 max_wins_4k_per_ba)
{
	u64 current_ba_tag;
	u32 wins_count_sel_per_ba = 1;
	u32 parallel_scan_count = 1;
	u32 index = 1;
	u32 total_wins_sel = 1;
	u32 max_scan_wins_per_ba = 1;

	if (!win_info || !win_info->segs || win_info->cnt == 0 ||
	    max_wins_4k_per_ba == 0)
		return -EINVAL;
	current_ba_tag = win_info->segs[0].ba_tag;
	for (; index < win_info->cnt;) {
		/* If same BA tag and not reached max count */
		if (current_ba_tag == win_info->segs[index].ba_tag &&
		    wins_count_sel_per_ba < max_wins_4k_per_ba) {
			win_info->segs[total_wins_sel++] =
				win_info->segs[index++];
			wins_count_sel_per_ba++;
			max_scan_wins_per_ba = max_t(u32, max_scan_wins_per_ba,
						     wins_count_sel_per_ba);
			continue;
		}

		/* Skip all segments with current BA tag */
		while (index < win_info->cnt &&
		       current_ba_tag == win_info->segs[index].ba_tag) {
			index++;
		}

		/* Move to next BA tag if available, and reset windows selected count for new BA tag */
		if (index < win_info->cnt) {
			current_ba_tag = win_info->segs[index].ba_tag;
			wins_count_sel_per_ba = 0;
			/* Increment parallel scan counter when switching to new BA tag */
			parallel_scan_count++;
		}
	}

	pr_debug(
		"4k scan, paral_scan_cnt: %u, max_scan_wins_per_ba: %u, total_wins_num: %u",
		parallel_scan_count, max_scan_wins_per_ba, total_wins_sel);
	win_info->cnt = total_wins_sel;
	return 0;
}

static int generate_aligned_4k_scan_wins_info(struct segs_info *win_info,
					      struct segs_info *info, u16 *buf)
{
	struct addr_seg *wins_4k;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	int ret;
	int max_wins_4k_per_ba =
		HIST_THREAD_PERIOD / HIST_SCAN_DURATION_PER_WIN -
		dev->scan_wins_num_per_ba;
	u32 nr_wins_4k = count_nr_windows(info->segs, info->cnt, STS_SIZE_4K);
	if (nr_wins_4k == 0 || max_wins_4k_per_ba <= 0)
		return -EINVAL;

	wins_4k = vzalloc(nr_wins_4k * sizeof(struct addr_seg));
	if (!wins_4k)
		return -ENOMEM;

	win_info->cnt = nr_wins_4k;
	win_info->segs = wins_4k;
	/*
	 * Firstly, we look up each 32MB or 128MB long tracking window and calculate
	 * the max access count within its range
	 */
	calc_4k_scan_hot_wins(info, buf, win_info);
	/*
	 * Secondly, sort windows by ba_tag in ascending order,
	 * and for the same ba_tag, sort by frequency in descending order
	 */
	sort(win_info->segs, win_info->cnt, sizeof(struct addr_seg),
	     addr_seg_cmp_max, NULL);
	/*
	 * Finally, filter the hottest part of the windows,
	 * each BA is constrained to a maximum of max_wins_4k_per_ba windows
	 */
	ret = filter_4k_scan_hot_wins(win_info, max_wins_4k_per_ba);
	if (ret) {
		pr_err("filter_4k_scan_hot_wins failed.[ret:%d]\n", ret);
		return ret;
	}
	return 0;
}

static void copy_actc_to_buf(struct segs_info *info, struct addr_seg *seg,
			     u16 *dst_buf, u16 *freq, u32 buf_len,
			     enum ub_hist_sts_size sts_size)
{
	u32 shift = (sts_size == STS_SIZE_2M) ? HIST_ADDR_SHIFT_2M :
						HIST_ADDR_SHIFT_4K;
	u64 inter_start, inter_end, inter_pages, j;
	u64 seg_offset, hist_offset, seg_pages = 0;
	u16 *dst;
	unsigned int i;
	for (i = 0; i < info->cnt; i++) {
		if (is_intersect(&info->segs[i], seg, &inter_start,
				 &inter_end)) {
			hist_offset = (inter_start - seg->start) >> shift;
			seg_offset = (inter_start - info->segs[i].start) >>
				     shift;
			inter_pages = (inter_end - inter_start + 1) >> shift;
			if (seg_pages + seg_offset + inter_pages > buf_len) {
				pr_err("exceeded upper bound: %u when coping to ACTC buffer\n",
				       buf_len);
				continue;
			}
			dst = dst_buf + seg_pages + seg_offset;
			for (j = hist_offset; j < (hist_offset + inter_pages);
			     j++) {
				*dst++ = freq[j];
			}
		}
		seg_pages += info->segs[i].size >> shift;
	}
}

static void clear_actc_buf(void)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	if (dev->buf)
		memset(dev->buf, 0, dev->pgcount * sizeof(u16));
}

static bool addr_is_cc_mem(u64 addr)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	u32 i;
	for (i = 0; i < dev->ba_cnt; i++) {
		if (dev->ba_info[i].cc_range.start <= addr &&
		    addr < dev->ba_info[i].cc_range.end)
			return true;
	}
	return false;
}

static int submit_ba_task(uint64_t ba_tag, u64 start_addr,
			  enum ub_hist_sts_size sts_size)
{
	u64 base_addr;
	u32 shift;
	union hi_upa_smap_cfg_smap_cfg00 *smap_cfg00;
	struct ub_hist_ba_config cfg = { 0 };

	base_addr = align_addr_by_sts_size(start_addr, sts_size);
	shift = get_hist_addr_shift(STS_SIZE_4K);
	cfg.reg_offset = BA_CTRL_REG_OFFSET;
	ub_hist_get_state(&cfg, ba_tag);
	smap_cfg00 = (union hi_upa_smap_cfg_smap_cfg00 *)&cfg.reg_value;
	smap_cfg00->page_size = sts_size;
	smap_cfg00->sts_rd_en = true;
	smap_cfg00->sts_wr_en = addr_is_cc_mem(base_addr) ? false : true;
	smap_cfg00->sts_enable = true;
	smap_cfg00->sts_base_addr = (base_addr >> shift);

	return ub_hist_set_state(&cfg, ba_tag);
}

static inline void wait_ba_task(u64 ms)
{
	pr_debug("wait_ba_task, %llu ms\n", ms);
	u64 min_us = ms * USEC_PER_MSEC;
	u64 max_us = min_us + SCAN_INTERVAL_RANGE_US;
	usleep_range(min_us, max_us);
}

static inline int disable_ba_task(uint64_t ba_tag)
{
	struct ub_hist_ba_config cfg;
	union hi_upa_smap_cfg_smap_cfg00 *smap_cfg00;

	cfg.reg_offset = BA_CTRL_REG_OFFSET;
	ub_hist_get_state(&cfg, ba_tag);
	smap_cfg00 = (union hi_upa_smap_cfg_smap_cfg00 *)&cfg.reg_value;
	smap_cfg00->sts_enable = false;

	return ub_hist_set_state(&cfg, ba_tag);
}

static inline void disable_all_ba_tasks(u32 *offset, u32 end)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	u32 ba_idx = dev->ba_cnt;
	while (ba_idx--) {
		if (offset[ba_idx] <
		    end) /* skip the BA which has no scan task */
			disable_ba_task(dev->ba_info[ba_idx].ba_tag);
	}
}

static inline bool pick_one_seg(u64 ba_tag, struct segs_info *win_info,
				int *offset)
{
	int i;
	for (i = *offset + 1; i < win_info->cnt; i++) {
		if (ba_tag == win_info->segs[i].ba_tag) {
			*offset = i;
			return true;
		}
	}
	*offset = win_info->cnt;
	return false;
}

static inline bool pick_complete(u32 *offset, u32 len, u32 end)
{
	bool complete_flag = true;
	u32 i;
	for (i = 0; i < len; i++) {
		complete_flag &= (offset[i] == end);
	}
	return complete_flag;
}

static int get_hist_results(uint64_t ba_tag,
			    struct ub_hist_ba_result *ba_result)
{
	int ret;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	ba_result->ba_tag = ba_tag;
	ret = ub_hist_get_statistic_result(ba_result);
	if (ret) {
		pr_err("ub_hist_get_statistic_result error (%d)\n", ret);
		return ret;
	}
	if (dev->abort_flag) {
		pr_debug("hist scan paused\n");
		return -EAGAIN;
	}
	return 0;
}

static int smap_hist_read_paral(struct segs_info *win_info,
				struct segs_info *rmem_info,
				enum ub_hist_sts_size sts_size, u16 *buf,
				u32 scan_time, u32 buf_len)
{
	int ret = 0, i, ba_cnt;
	u64 ba_tag;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	struct ub_hist_ba_result *ba_result =
		kmalloc(sizeof(*ba_result), GFP_KERNEL);
	int *offset = kzalloc(sizeof(*offset) * dev->ba_cnt, GFP_KERNEL);
	if (!ba_result || !offset) {
		kfree(ba_result);
		kfree(offset);
		return -ENOMEM;
	}
	for (i = 0; i < dev->ba_cnt; ++i) {
		offset[i] = -1;
	}

	while (1) {
		ba_cnt = dev->ba_cnt;
		while (ba_cnt--) {
			ba_tag = dev->ba_info[ba_cnt].ba_tag;
			if (pick_one_seg(ba_tag, win_info, &offset[ba_cnt])) {
				submit_ba_task(
					ba_tag,
					win_info->segs[offset[ba_cnt]].start,
					sts_size);
			}
		}
		if (pick_complete(offset, dev->ba_cnt, win_info->cnt))
			break;

		wait_ba_task(scan_time);
		disable_all_ba_tasks(offset, win_info->cnt);
		ba_cnt = dev->ba_cnt;
		while (ba_cnt--) {
			if (offset[ba_cnt] >= win_info->cnt)
				continue;

			ret = get_hist_results(dev->ba_info[ba_cnt].ba_tag,
					       ba_result);
			if (ret) {
				kfree(ba_result);
				kfree(offset);
				return ret;
			}
			copy_actc_to_buf(rmem_info,
					 &win_info->segs[offset[ba_cnt]], buf,
					 ba_result->buffer, buf_len, sts_size);
		}
	}
	kfree(ba_result);
	kfree(offset);
	return ret;
}

static u32 get_2m_scan_wins_per_ba(struct segs_info *win_info)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	u32 cnt, max_cnt = 1, ba_cnt = dev->ba_cnt, paral_cnt = 0;
	int offset;
	u64 ba_tag;
	while (ba_cnt--) {
		cnt = 0;
		offset = -1;
		ba_tag = dev->ba_info[ba_cnt].ba_tag;
		while (pick_one_seg(ba_tag, win_info, &offset)) {
			cnt++;
		}
		if (cnt)
			paral_cnt++;
		max_cnt = max_t(u32, cnt, max_cnt);
	}
	pr_debug(
		"2m scan, paral_scan_cnt: %u, max_scan_wins_per_ba: %u, total_wins_num: %u\n",
		paral_cnt, max_cnt, win_info->cnt);
	return max_cnt;
}

static int generate_aligned_wins_info(struct segs_info *win_info,
				      struct segs_info *info, u16 *buf,
				      enum ub_hist_sts_size sts_size)
{
	int ret = 0;

	ret = (sts_size == STS_SIZE_2M) ?
		      generate_aligned_2m_scan_wins_info(win_info, info) :
		      generate_aligned_4k_scan_wins_info(win_info, info, buf);
	return ret;
}

static int do_hist_scan_sliding(struct segs_info *info, bool do_multi_gran,
				u16 *buf, u32 buf_len,
				enum ub_hist_sts_size sts_size)
{
	int ret;
	struct segs_info win_info;
	u32 scan_time;
	/*
	 * First of all, we calculate the least number of windows to
	 * cover all address segments
	 */
	ret = generate_aligned_wins_info(&win_info, info, buf, sts_size);
	if (ret) {
		pr_err("generate merged segs info failed. ret: %d\n", ret);
		return ret;
	}
	/* Secondly, calculate scan period of a single window */
	if (sts_size == STS_SIZE_2M) {
		g_smap_hist_dev.scan_wins_num_per_ba =
			get_2m_scan_wins_per_ba(&win_info);
	}
	scan_time = (do_multi_gran == true) ?
			    HIST_SCAN_DURATION_PER_WIN :
			    HIST_THREAD_PERIOD /
				    g_smap_hist_dev.scan_wins_num_per_ba;
	pr_debug("scan_wins_num_per_win: %u ms\n", scan_time);

	/* Finally, launch scan job for every windows */
	clear_actc_buf();
	ret = smap_hist_read_paral(&win_info, info, sts_size, buf, scan_time,
				   buf_len);
	vfree(win_info.segs);
	return ret;
}

static int hist_scan_sliding(struct segs_info *info, u32 scan_time_total,
			     u16 *buf, u32 buf_len, u32 pgsize)
{
	int ret;
	/*
	 * For 2M pages, size of a window which consist of 16K pages is as large
	 * as 32GB, we can finish all windows scan in time and ensure that each
	 * window has been scanned for a sufficient period of time. But for 4K
	 * pages, windows to scan is 512 times more than that of 2M pages.
	 * To deal this problem, we use a strategy named "Multi-Hierarchy scan",
	 * we do scan of a 32GB sliding window firstly to figure out the hottest
	 * windows, scan work of a 64MB sliding window will be launched in those
	 * windows later to get a fine-grained access count of each 4K pages.
	 */
	bool do_multi_gran = pgsize == SIZE_4K;

	if (!addr_seg_is_valid(info)) {
		pr_err("invalid address segment passed to sliding scan\n");
		return -EINVAL;
	}
	if (!scan_time_total) {
		pr_err("invalid scan period passed to sliding scan\n");
		return -EINVAL;
	}
	if (!buf) {
		pr_err("null buffer passed to sliding scan\n");
		return -EINVAL;
	}
	ret = do_hist_scan_sliding(info, do_multi_gran, buf, buf_len,
				   STS_SIZE_2M);
	if (ret) {
		pr_debug("sliding scan on 2M pages failed, ret: %d\n", ret);
		return ret;
	}
	if (!do_multi_gran) {
		return 0;
	}
	/* Rescan for the secondary hierarchy */
	ret = do_hist_scan_sliding(info, do_multi_gran, buf, buf_len,
				   STS_SIZE_4K);
	if (ret) {
		pr_debug("sliding scan on 4K pages failed, ret: %d\n", ret);
		return ret;
	}
	return 0;
}

static void add_to_actc_data(u16 *dst, u16 *src, int len)
{
	int i, sum;
	int j;
	int group_count;
	u32 freq = 0;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	if (PAGE_SIZE == PAGE_SIZE_64K && dev->pgsize == SIZE_4K) {
		group_count = len / PAGE_SIZE_64K_DIV_4K;
		for (i = 0; i < group_count; i++) {
			for (j = 0; j < PAGE_SIZE_64K_DIV_4K; j++) {
				freq += src[(PAGE_SIZE_64K_DIV_4K * i) + j];
			}
			sum = dst[i] + freq;
			dst[i] = (sum < U16_MAX) ? (u16)sum : U16_MAX;
		}
	} else {
		for (i = 0; i < len; i++) {
			sum = dst[i] + src[i];
			dst[i] = (sum < U16_MAX) ? (u16)sum : U16_MAX;
		}
	}
}

void fetch_hist_actc_buf(u16 *dst_buf, struct addr_seg *seg)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	struct segs_info *info = &dev->info;
	u64 inter_start, inter_end, inter_pages;
	u64 offset, seg_pages = 0;
	u32 shift = dev->pgsize == SIZE_2M ? HIST_ADDR_SHIFT_2M :
					     HIST_ADDR_SHIFT_4K;
	unsigned int i;
	for (i = 0; i < info->cnt; i++) {
		if (is_intersect(&info->segs[i], seg, &inter_start,
				 &inter_end)) {
			offset = (inter_start - info->segs[i].start) >> shift;
			inter_pages = (inter_end - inter_start + 1) >> shift;
			if (seg_pages + offset + inter_pages > dev->pgcount) {
				pr_warn("exceeds upper bound: %llu, when trying to copy ACTC data of amount: %u to buffer\n",
					seg_pages + offset + inter_pages,
					dev->pgcount);
				return;
			}
			if (!dev->status.status_all) {
				add_to_actc_data(dst_buf,
						 dev->buf + seg_pages + offset,
						 inter_pages);
			}
		}
		seg_pages += info->segs[i].size >> shift;
	}
}

void hist_update_pgsize(u32 pgsize)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	if (pgsize != dev->pgsize) {
		dev->status.flag.new_pgsize = pgsize;
	}
}

void hist_set_iomem(void)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	if (!dev->status.flag.should_update_iomem) {
		dev->status.flag.should_update_iomem = true;
	}
}

void hist_thread_resume(void)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	dev->thread_enable = true;
	dev->abort_flag = false;
}

void hist_thread_pause(void)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	dev->thread_enable = false;
	dev->abort_flag = true;
}

void hist_set_flush_actc_cb(flush_actc cb)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	dev->flush_actc = cb;
}

static inline void flush_actc_data(struct smap_hist_dev *dev)
{
	if (dev->flush_actc)
		dev->flush_actc();
}

static inline unsigned int get_remote_ram_segs_locked(void)
{
	unsigned int nr_segs = 0;
	struct ram_segment *seg, *tmp;

	list_for_each_entry_safe(seg, tmp, &remote_ram_list, node) {
		if (seg->numa_node < nr_local_numa)
			continue;

		nr_segs++;
	}
	return nr_segs;
}

static int addr_segs_init(struct smap_hist_dev *dev, u32 pgsize)
{
	unsigned int i = 0, nr_segs = 0;
	u32 pgcount = 0;
	struct ram_segment *seg, *tmp;
	u32 shift = pgsize == SIZE_2M ? HIST_ADDR_SHIFT_2M : HIST_ADDR_SHIFT_4K;
	struct addr_seg *tmp_addr_segs;

	read_lock(&rem_ram_list_lock);
	nr_segs = get_remote_ram_segs_locked();
	read_unlock(&rem_ram_list_lock);
	if (nr_segs == 0) {
		dev->info.cnt = 0;
		dev->info.segs = NULL;
		dev->pgcount = 0;
		dev->pgsize = pgsize;
		pr_info("no page passed to address segment init\n");
		return 0;
	}
	tmp_addr_segs = vzalloc(nr_segs * sizeof(struct addr_seg));
	if (!tmp_addr_segs) {
		pr_err("unable to allocate memory for address segment info\n");
		return -ENOMEM;
	}

	read_lock(&rem_ram_list_lock);
	dev->info.cnt = get_remote_ram_segs_locked();
	if (dev->info.cnt != nr_segs) {
		read_unlock(&rem_ram_list_lock);
		pr_err("remote segment info has been refreshed unexpectedly\n");
		vfree(tmp_addr_segs);
		return -EINVAL;
	}
	dev->info.segs = tmp_addr_segs;
	list_for_each_entry_safe(seg, tmp, &remote_ram_list, node) {
		if (seg->numa_node < nr_local_numa)
			continue;
		dev->info.segs[i].start = seg->start;
		dev->info.segs[i].size = seg->end - seg->start + 1;
		pgcount += (dev->info.segs[i].size >> shift);
		i++;
	}
	read_unlock(&rem_ram_list_lock);
	sort(dev->info.segs, dev->info.cnt, sizeof(struct addr_seg),
	     addr_seg_cmp_start, NULL);
	dev->pgcount = pgcount;
	pr_info("page count has been updated to %u\n", pgcount);
	dev->pgsize = pgsize;
	return 0;
}

static void addr_segs_deinit(struct smap_hist_dev *dev)
{
	if (dev->info.segs) {
		pr_info("deleting addr segs.\n");
		vfree(dev->info.segs);
		dev->info.segs = NULL;
		dev->info.cnt = 0;
	}
}

static int hist_buffer_init(struct smap_hist_dev *dev)
{
	if (!dev->pgcount) {
		dev->buf = NULL;
		return 0;
	}
	dev->buf = vzalloc(dev->pgcount * sizeof(u16));
	if (!dev->buf) {
		pr_err("unable to allocate memory for ACTC buffer\n");
		return -ENOMEM;
	}
	return 0;
}

static void hist_buffer_deinit(struct smap_hist_dev *dev)
{
	if (dev->buf) {
		vfree(dev->buf);
		dev->buf = NULL;
	}
}

static int hist_pginfo_reinit(struct smap_hist_dev *dev, u32 pgsize_new)
{
	int ret;
	addr_segs_deinit(dev);
	ret = addr_segs_init(dev, pgsize_new);
	if (ret) {
		pr_err("unable to reinit address segments, ret: %d\n", ret);
		return -ENOMEM;
	}
	hist_buffer_deinit(dev);
	ret = hist_buffer_init(dev);
	if (ret) {
		addr_segs_deinit(dev);
		pr_err("unable to reinit histogram ACTC buffer, ret: %d\n",
		       ret);
		return -ENOMEM;
	}
	return 0;
}

#define REMOTE_NUMA_OVERFLOW_SIZE 824633720832
static int is_total_seg_overflow(struct segs_info *info)
{
	u32 i = 0;
	u64 total_size = 0;
	for (; i < info->cnt; ++i) {
		total_size += info->segs[i].size;
	}
	if (total_size >= REMOTE_NUMA_OVERFLOW_SIZE) {
		pr_debug("remote numa total memory size overflow with %llu\n",
			 total_size);
		return 1;
	}
	return 0;
}

static int scan_thread_run(void *data)
{
	int ret;
	struct smap_hist_dev *dev = (struct smap_hist_dev *)data;
	int flag = 0;
	while (!kthread_should_stop()) {
		if (flag == 1) {
			if (is_total_seg_overflow(&dev->info) == 0) {
				flag = 0;
			}
			msleep(1);
			continue;
		}
		if (dev->status.status_all) {
			u32 pgsize = dev->status.flag.new_pgsize ?: dev->pgsize;
			pr_info("histogram tracking page info has been reinited, status: %#x\n",
				dev->status.status_all);
			ret = hist_pginfo_reinit(dev, pgsize);
			if (ret) {
				pr_err("failed to reinit histogram tracking page info, ret: %d\n",
				       ret);
				msleep(THREAD_SLEEP);
				continue;
			}
		}
		dev->status.status_all = 0;

		if (!dev->thread_enable || !dev->pgcount) {
			msleep(1);
			continue;
		}

		if (is_total_seg_overflow(&dev->info) == 1) {
			flag = 1;
			continue;
		}

		ret = hist_scan_sliding(&dev->info, dev->period, dev->buf,
					dev->pgcount, dev->pgsize);
		if (ret) {
			pr_debug("failed to do scan sliding, ret: %d\n", ret);
			msleep(THREAD_SLEEP);
			continue;
		}
		if (!dev->status.status_all) {
			pr_debug("flush ACTC data\n");
			flush_actc_data(dev);
		} else {
			pr_info("skip histogram, status: %#x\n",
				dev->status.status_all);
		}
	}
	return 0;
}

static int scan_thread_init(struct smap_hist_dev *dev)
{
	dev->kthread = kthread_run(scan_thread_run, dev, "hist-scan-thread");
	if (!dev->kthread) {
		pr_err("failed to create scan threads\n");
		return -ECHILD;
	}

	return 0;
}

static void scan_thread_deinit(struct smap_hist_dev *dev)
{
	if (dev->kthread) {
		pr_info("scan thread has been deleted\n");
		kthread_stop(dev->kthread);
		dev->kthread = NULL;
	}
}

struct smap_hist_dev *get_hist_dev(void)
{
	return &g_smap_hist_dev;
}

static int query_hist_ba_info(void)
{
	int i, ret;
	u64 *ba_tags;
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	dev->ba_cnt = ub_hist_query_ba_count();
	if (!dev->ba_cnt) {
		pr_err("No smap-histogram dev found\n");
		return -EINVAL;
	}
	pr_info("%d smap-histogram devices found\n", dev->ba_cnt);
	ba_tags = kmalloc(sizeof(u64) * dev->ba_cnt, GFP_KERNEL);
	if (!ba_tags) {
		pr_err("malloc memory failed for ba_tags\n");
		return -ENOMEM;
	}
	ret = ub_hist_query_ba_tags(ba_tags, dev->ba_cnt);
	if (ret) {
		kfree(ba_tags);
		return ret;
	}

	dev->ba_info = kmalloc(sizeof(struct ub_hist_ba_info) * dev->ba_cnt,
			       GFP_KERNEL);
	if (!dev->ba_info) {
		kfree(ba_tags);
		pr_err("malloc memory failed for ba_info\n");
		return -ENOMEM;
	}

	for (i = 0; i < dev->ba_cnt; i++) {
		ub_hist_query_ba_info(ba_tags[i], &dev->ba_info[i]);
	}
	kfree(ba_tags);
	return 0;
}

void hist_deinit(void)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	scan_thread_deinit(dev);
	hist_buffer_deinit(dev);
	addr_segs_deinit(dev);
	kfree(dev->ba_info);
	ub_hist_exit();
}

int hist_init(u32 pgsize)
{
	struct smap_hist_dev *dev = &g_smap_hist_dev;
	int ret;

	ret = ub_hist_init();
	if (ret)
		return ret;
	dev->hw_type = ub_hist_get_hw_type();
	pr_info("Smap hist dev init start, dev->hw_type: %u\n", dev->hw_type);
	dev->freq_register_cnt = (dev->hw_type == UB_HIST_SMAP_TYPE_N6) ?
					 BA_STS_VALUE_N6_COUNT :
					 BA_STS_VALUE_N7_COUNT;

	ret = query_hist_ba_info();
	if (ret)
		goto exit_hist;

	dev->period = HIST_THREAD_PERIOD;

	ret = addr_segs_init(dev, pgsize);
	if (ret)
		goto free_hist;

	ret = hist_buffer_init(dev);
	if (ret)
		goto free_segs;

	ret = scan_thread_init(dev);
	if (ret)
		goto free_buf;

	pr_info("histogram tracking device init successfully\n");
	return 0;

free_buf:
	hist_buffer_deinit(dev);
free_segs:
	addr_segs_deinit(dev);
free_hist:
	kfree(dev->ba_info);
exit_hist:
	ub_hist_exit();
	return ret;
}
