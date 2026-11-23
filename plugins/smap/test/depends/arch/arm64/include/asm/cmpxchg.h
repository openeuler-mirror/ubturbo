/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CMPXCHG_H
#define __ASM_CMPXCHG_H

#define cmpxchg(ptr, oldval, newval) __sync_val_compare_and_swap(ptr, oldval, newval)
#define xchg(ptr, val) __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)

#ifndef LAST_CPUPID_PGSHIFT
#define LAST_CPUPID_PGSHIFT 42
#endif
#ifndef LAST_CPUPID_WIDTH
#define LAST_CPUPID_WIDTH 20
#endif

#endif
