/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP Tiering Memory Solution: SMAP MSG
 */

#ifndef _SMAP_MSG_H
#define _SMAP_MSG_H

#include <linux/workqueue.h>
#include <linux/time64.h>
#include <linux/completion.h>
#include "common.h"

extern unsigned int smap_pgtype;

enum smap_pgtype_args {
	NORMAL_PAGE,
	HUGE_PAGE,
	NR_PGSIZE_ARGS,
};

enum page_type_stat {
	PAGE_TYPE_TRANSHUGE,
	PAGE_TYPE_HUGE,
	PAGE_TYPE_NOR_LRU,
	PAGE_TYPE_ZERO_REF,
	NR_ABNORMAL,
};

extern bool is_smap_pg_huge(void);

#endif /* _SMAP_MSG_H */
