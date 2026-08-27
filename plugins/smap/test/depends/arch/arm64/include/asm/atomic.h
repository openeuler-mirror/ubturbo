/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __ASM_GENERIC_ATOMIC_H
#define __ASM_GENERIC_ATOMIC_H

#include <linux/types.h>

static inline int atomic_read(const atomic_t *v)
{
    return v->counter;
}

static inline void atomic_set(atomic_t *v, int i)
{
    v->counter = i;
}

static inline int atomic_inc(atomic_t *v)
{
    v->counter += 1;
    return v->counter - 1;
}

static inline int atomic_dec(atomic_t *v)
{
    v->counter -= 1;
    return v->counter + 1;
}

static inline int atomic_fetch_add(int i, atomic_t *v)
{
    v->counter += i;
    return v->counter - i;
}

#endif /* __ASM_GENERIC_ATOMIC_H */
