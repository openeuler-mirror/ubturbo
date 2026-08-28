// SPDX-License-Identifier: GPL-2.0-only
/*
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/

#ifndef TEST_INSTRUMENTED_ATOMIC_H
#define TEST_INSTRUMENTED_ATOMIC_H

#include <asm/bitsperlong.h>
static void set_bit(long nr, volatile unsigned long *addr)
{
    addr[nr / __BITS_PER_LONG] |= 1UL << (nr % __BITS_PER_LONG);
}

#endif // TEST_INSTRUMENTED_ATOMIC_H
