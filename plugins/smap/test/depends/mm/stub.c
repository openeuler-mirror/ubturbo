// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: memory stub
 */

#include <linux/mm.h>

static void volatile_memcpy(volatile char *to, const volatile char *from, unsigned long n)
{
	unsigned long tmp = n;
	while (tmp--)
		*(to++) = *(from++);
}

unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{
	volatile_memcpy(to, from, n);
	return 0;
}

unsigned long copy_to_user(void *to, const void *from, unsigned long n)
{
	volatile_memcpy(to, from, n);
	return 0;
}

bool get_page_unless_zero(struct page *page)
{
	return true;
}

bool __folio_test_movable(struct folio *folio)
{
	return true;
}

void put_page(struct page *page)
{
}

void folio_put(struct folio *folio)
{
}

unsigned long compound_nr(struct page *page)
{
	return 0;
}

int __PageMovable(struct page *page)
{
	return 0;
}

unsigned long page_to_pfn(struct page *page)
{
	return 0;
}

phys_addr_t page_to_phys(struct page *page)
{
	return 0;
}

bool PageAnon(struct page *page)
{
        return true;
}

/* vmscan reclaim_pages() is EXPORT_SYMBOL_GPL in the kernel; the DT build
 * replaces the whole vmscan with stubs, so provide a no-op reclaim.
 */
unsigned long reclaim_pages(struct list_head *folio_list, bool ignore_references)
{
	return 0;
}
