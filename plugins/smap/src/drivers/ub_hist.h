/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP : ub_hist
 */

#ifndef UB_HIST_H
#define UB_HIST_H

#include <linux/device.h>
#include <linux/io.h>
#include "upa_smap.h"

#define BA_STS_VALUE_SIZE 2
#define BA_STS_VALUE_COUNT 32768
#define BA_STS_VALUE_N7_COUNT 32768
#define BA_STS_VALUE_N6_COUNT 8192
#define BA_STS_TOTAL_LEHGTH(ba_sts_value_count) \
	((ba_sts_value_count) * BA_STS_VALUE_SIZE)
#define BA_STS_WORD_COUNT(ba_sts_value_count) \
	(BA_STS_TOTAL_LEHGTH(ba_sts_value_count) / sizeof(u32))

#define KB (1ULL << 10)
#define MB (1ULL << 20)
#define GB (1ULL << 30)

#define SIZE_4K (4 * KB)
#define SHIFT_4K 12

#define SIZE_2M (2 * MB)
#define SHIFT_2M 21

#define SIZE_32M (32 * MB)
#define SHIFT_32M 25

#define SIZE_16G (16 * GB)
#define SHIFT_16G 34

#define SIZE_128M (128 * MB)
#define SHIFT_128M 27

#define SIZE_64G (64 * GB)
#define SHIFT_64G 36

enum ub_hist_sts_size {
	STS_SIZE_4K = 0,
	STS_SIZE_2M = 1,
};

struct phy_addr_range {
	phys_addr_t start;
	phys_addr_t end;
};

struct ub_hist_ba_info {
	uint64_t ba_tag;
	int socket;
	struct phy_addr_range nc_range;
	struct phy_addr_range cc_range;
};

struct ub_hist_ba_result {
	uint64_t ba_tag;
	uint16_t buffer[BA_STS_VALUE_COUNT];
};

struct ub_hist_ba_tags {
	int32_t ba_count;
	int32_t rsvd;
	uint64_t ba_tags[0];
};

typedef enum {
	UB_HIST_SMAP_TYPE_N6 = 0,
	UB_HIST_SMAP_TYPE_N7
} ub_hist_smap_type;

int ub_hist_init(void);
void ub_hist_exit(void);
ub_hist_smap_type ub_hist_get_hw_type(void);
int ub_hist_query_ba_count(void);
int ub_hist_query_ba_tags(uint64_t *p_tags, int count);
int ub_hist_query_ba_info(uint64_t ba_tag, struct ub_hist_ba_info *ba_info);
int ub_hist_set_state(struct ub_hist_ba_config *config, uint64_t ba_tag);
int ub_hist_get_state(struct ub_hist_ba_config *config, uint64_t ba_tag);
int ub_hist_get_statistic_result(struct ub_hist_ba_result *result);

#endif