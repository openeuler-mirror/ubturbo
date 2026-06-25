/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Macros for manipulating and testing page->flags
 */

#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/mm_types.h>
#include <linux/pfn.h>

#define PageReserved(p) 0

#ifdef __cplusplus
extern "C" {
#endif

#define pageblock_nr_pages	(1UL << pageblock_order)
#define pageblock_align(pfn)	ALIGN((pfn), pageblock_nr_pages)

struct page *compound_head(struct page *page);
extern unsigned long _compound_head(struct page *page);

static inline int PageLRU(struct page *page)
{
	return 0;
}

static inline int PageTransHuge(struct page *page)
{
	return 0;
}

static inline bool is_page_hwpoison(struct page *page)
{
    (void)page;
    return false;
}

static inline bool folio_test_hugetlb(struct folio *folio)
{
    return false;
}

static inline bool folio_test_anon(struct folio *folio)
{
    return false;
}

static inline bool folio_test_ksm(struct folio *folio)
{
    return false;
}

static inline bool folio_test_swapcache(struct folio *folio)
{
    return false;
}

#define PF_POISONED_CHECK(page) ({ page; })

#ifdef __cplusplus
static inline struct folio *page_folio(struct page *page)
{
    return (struct folio *)_compound_head(page);
}
#else
#define page_folio(p)		(_Generic((p),				\
		const struct page *:	(const struct folio *)_compound_head(p), \
		struct page *:		(struct folio *)_compound_head(p)))
#endif

#ifdef __cplusplus
}
#endif

#endif	/* PAGE_FLAGS_H */
