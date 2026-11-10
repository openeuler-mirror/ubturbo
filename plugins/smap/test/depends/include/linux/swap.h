/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_H
#define _LINUX_SWAP_H

#include <linux/types.h>

#define MAX_SWAPFILES_SHIFT	5

#define TOTAL_SWAP_PAGES (1 << 20) /* 4096 pages, enough for DT build */

/* DT: extern so tests can stub it with MOCKER (negative/zero swapspace
 * behaviors); the DT definition lives in depends/mm/stub.c */
#ifdef __cplusplus
extern "C" {
#endif
extern long get_nr_swap_pages(void);
#ifdef __cplusplus
}
#endif

#endif /* _LINUX_SWAP_H */
