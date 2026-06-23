/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RWLOCK_RT_H
#define _LINUX_RWLOCK_RT_H
#include <linux/spinlock_types.h>

static __always_inline void write_lock(rwlock_t *rwlock)
{
    return;
}
static __always_inline void write_unlock(rwlock_t *rwlock)
{
    return;
}
static __always_inline void read_lock(rwlock_t *rwlock)
{
    return;
}
static __always_inline void read_unlock(rwlock_t *rwlock)
{
    return;
}
#endif