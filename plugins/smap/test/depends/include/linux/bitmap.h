/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITMAP_H
#define __LINUX_BITMAP_H

#include <stdlib.h>
#include <linux/bitops.h>

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)
{
    (void)flags;
    return calloc(BITS_TO_LONGS(nbits), sizeof(unsigned long));
}

static inline void bitmap_free(const unsigned long *bitmap)
{
    free((void *)bitmap);
}

#endif /* __LINUX_BITMAP_H */
