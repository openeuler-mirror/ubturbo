// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: memory stub
 */
#include <stdlib.h>
#include <linux/mm.h>
static struct folio io = {0};

struct folio *get_hugetlb_folio_nodemask(unsigned long size, int preferred_nid, nodemask_t *nmask, gfp_t gfp_mask,
    void *data)
{
    return &io;
}

bool folio_test_head(struct folio *folio)
{
    return false;
}

bool folio_test_hugetlb_migratable(struct folio *folio)
{
    return false;
}

unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{
    return 0;
}

void *kmalloc(size_t size, gfp_t flags)
{
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

void *vmalloc(unsigned long size)
{
    return malloc(size);
}

void *vzalloc(unsigned long size)
{
    return calloc(1, size);
}

void vfree(void *p)
{
    return free(p);
}

struct page *compound_head(struct page *page)
{
    return page;
}