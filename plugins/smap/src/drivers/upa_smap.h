/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP : ub_hist
 */

#ifndef _UPA_SMAP_H
#define _UPA_SMAP_H

enum {
	BA_CTRL_REG_OFFSET = 0x0,
	BA_ECC_REG_OFFSET = 0x4,
	BA_THRESH_REG_OFFSET = 0x8,
	BA_SMAP_TH1_OFFSET = 0xC,
	BA_SMAP_TH2_OFFSET = 0x10,
	BA_SMAP_TH3_OFFSET = 0x14,
	BA_SMAP_TH4_OFFSET = 0x18,
	BA_SMAP_CNT0_OFFSET = 0x1C,
	BA_SMAP_CNT1_OFFSET = 0x20,
	BA_SMAP_CNT2_OFFSET = 0x24,
	BA_SMAP_CNT3_OFFSET = 0x28,
	BA_SMAP_CNT4_OFFSET = 0x2C,
	BA_SMAP_CNT_CLR0_OFFSET = 0x30,
	BA_SMAP_CNT_CLR1_OFFSET = 0x34,
	BA_SMAP_CNT_CLR2_OFFSET = 0x38,
	BA_SMAP_EN_OFFSET = 0x3C,
	BA_SMAP_INT_MASK_OFFSET = 0x40,
	BA_SMAP_CNT_INT_CLR0_OFFSET = 0x44,
	BA_SMAP_CNT_INT_CLR1_OFFSET = 0x48,
	BA_SMAP_CNT_INT_CLR2_OFFSET = 0x4C,
	BA_SMAP_RPT0_OFFSET = 0x50,
	BA_SMAP_RPT1_OFFSET = 0x54
};

enum {
	BA_CTRL_REG_INIT = 0x000001C1,
	BA_ECC_REG_INIT = 0x00000000,
	BA_THRESH_REG_INIT = 0x00FF0000,
	BA_SMAP_TH1_INIT = 0xFFFFFFFF,
	BA_SMAP_TH2_INIT = 0xFFFF,
	BA_SMAP_TH3_INIT = 0xFFFF,
	BA_SMAP_TH4_INIT = 0xFFFFFFFF,
	BA_SMAP_CNT0_INIT = 0xFFFFFFFF,
	BA_SMAP_CNT1_INIT = 0xFFFF,
	BA_SMAP_CNT2_INIT = 0x0,
	BA_SMAP_CNT3_INIT = 0x0,
	BA_SMAP_CNT4_INIT = 0x0,
	BA_SMAP_CNT_CLR0_INIT = 0x0,
	BA_SMAP_CNT_CLR1_INIT = 0x0,
	BA_SMAP_CNT_CLR2_INIT = 0x0,
	BA_SMAP_EN_INIT = 0x0,
	BA_SMAP_INT_MASK_INIT = 0x7,
	BA_SMAP_CNT_INT_CLR0_INIT = 0x0,
	BA_SMAP_CNT_INT_CLR1_INIT = 0x0,
	BA_SMAP_CNT_INT_CLR2_INIT = 0x0,
	BA_SMAP_RPT0_INIT = 0x0,
	BA_SMAP_RPT1_INIT = 0x0
};

union hi_upa_smap_cfg_smap_cfg00 {
	struct {
		uint32_t ram_init_done : 1;
		uint32_t ecc_inject_mode : 1;
		uint32_t ecc_inject_en : 2;
		uint32_t sts_enable : 1;
		uint32_t page_size : 1;
		uint32_t sts_rd_en : 1;
		uint32_t sts_wr_en : 1;
		uint32_t ecc2bit_err_to_poison : 1;
		uint32_t sts_base_addr : 21;
		uint32_t reserved : 2;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cfg01 {
	struct {
		uint32_t smap_1bit_ecc_int_en : 1;
		uint32_t smap_1bit_ecc_int_src : 1;
		uint32_t smap_1bit_ecc_int_sts : 1;
		uint32_t smap_2bit_ecc_int_en : 1;
		uint32_t smap_2bit_ecc_int_src : 1;
		uint32_t smap_2bit_ecc_int_sts : 1;
		uint32_t reserved : 2;
		uint32_t ecc_addr_1bit : 12;
		uint32_t ecc_addr_2bit : 12;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_th0 {
	struct {
		uint32_t ecc_1bit_intr_cnt : 16;
		uint32_t ecc_1bit_intr_th : 16;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_th1 {
	struct {
		uint32_t smap_cnt_prd_th : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_th2 {
	struct {
		uint32_t smap_page_view_th : 16;
		uint32_t reserved : 16;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_th3 {
	struct {
		uint32_t smap_page_num_th : 16;
		uint32_t reserved : 16;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_th4 {
	struct {
		uint32_t smap_total_cnt_th : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt0 {
	struct {
		uint32_t smap_cnt_prd : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt1 {
	struct {
		uint32_t smap_cnt_page : 16;
		uint32_t reserved : 16;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt2 {
	struct {
		uint32_t smap_cnt_total : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt3 {
	struct {
		uint32_t smap_cnt_all : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt4 {
	struct {
		uint32_t smap_cnt_drop : 32;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_clr0 {
	struct {
		uint32_t smap_cnt_total_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_clr1 {
	struct {
		uint32_t smap_cnt_page_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_clr2 {
	struct {
		uint32_t smap_cnt_prd_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_en {
	struct {
		uint32_t smap_cnt_prd_en : 1;
		uint32_t smap_cnt_page_en : 1;
		uint32_t smap_cnt_total_en : 1;
		uint32_t reserved : 29;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_int_mask {
	struct {
		uint32_t smap_cnt_prd_int_mask : 1;
		uint32_t smap_cnt_page_int_mask : 1;
		uint32_t smap_cnt_total_int_mask : 1;
		uint32_t reserved : 29;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_int_clr0 {
	struct {
		uint32_t smap_cnt_total_int_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_int_clr1 {
	struct {
		uint32_t smap_cnt_page_int_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_cnt_int_clr2 {
	struct {
		uint32_t smap_cnt_prd_int_clr : 1;
		uint32_t reserved : 31;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_rpt0 {
	struct {
		uint32_t smap_cnt_prd_int_rpt : 1;
		uint32_t smap_cnt_page_int_rpt : 1;
		uint32_t smap_cnt_total_int_rpt : 1;
		uint32_t reserved : 29;
	};
	uint32_t val;
};

union hi_upa_smap_cfg_smap_rpt1 {
	struct {
		uint32_t smap_cnt_prd_int_stat : 1;
		uint32_t smap_cnt_page_int_stat : 1;
		uint32_t smap_cnt_total_int_stat : 1;
		uint32_t reserved : 29;
	};
	uint32_t val;
};

struct ub_hist_ba_config {
	uint32_t reg_offset;
	uint32_t reg_value;
};

struct ub_hist_reg_config {
	uint64_t ba_tag;
	struct ub_hist_ba_config config;
};

#endif