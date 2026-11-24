// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: SMAP dump info module
 */

#include <linux/kernel.h>

#include "smap_msg.h"
#include "dump_info.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_dump_info: " fmt

void filter_4k_migrate_info(size_t *nr_abnormal)
{
	pr_debug("transhuge %lu, huge %lu, nr_lru %lu, nr_zero_ref %lu\n",
		 nr_abnormal[PAGE_TYPE_TRANSHUGE], nr_abnormal[PAGE_TYPE_HUGE],
		 nr_abnormal[PAGE_TYPE_NOR_LRU],
		 nr_abnormal[PAGE_TYPE_ZERO_REF]);
}
