/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_H
#define _LINUX_SWAP_H

#include <linux/types.h>

#define MAX_SWAPFILES_SHIFT	5

#define TOTAL_SWAP_PAGES (1 << 20) /* 4096 pages, enough for DT build */

static inline long get_nr_swap_pages(void)
{
	return TOTAL_SWAP_PAGES;
}

#endif /* _LINUX_SWAP_H */
