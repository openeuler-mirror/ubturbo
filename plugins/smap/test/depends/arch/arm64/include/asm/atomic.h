/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __ASM_GENERIC_ATOMIC_H
#define __ASM_GENERIC_ATOMIC_H

/*
 * Test stub: the unit-test harness is single-threaded, so atomic ops just
 * operate on the underlying counter. The kernel build uses the real
 * arch/arm64 <asm/atomic.h>.
 */
#ifndef atomic_read
#define atomic_read(v) ((v)->counter)
#endif
#define atomic_set(v, i) ((v)->counter = (i))

static inline int atomic_cmpxchg(atomic_t *v, int old, int newp)
{
	int cur = v->counter;

	if (cur == old)
		v->counter = newp;
	return cur;
}

static inline void atomic_inc(atomic_t *v)
{
	v->counter++;
}

static inline void atomic_dec(atomic_t *v)
{
	v->counter--;
}

#endif /* __ASM_GENERIC_ATOMIC_H */
