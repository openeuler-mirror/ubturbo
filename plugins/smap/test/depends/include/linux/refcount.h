/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REFCOUNT_H
#define _LINUX_REFCOUNT_H

#include <linux/atomic.h>

typedef struct refcount_struct {
	atomic_t refs;
} refcount_t;

/*
 * Test stub implementations. The unit-test harness is single-threaded (locks
 * are no-ops), so these provide sequential semantics sufficient to exercise
 * the slot state machine and reclamation logic. The kernel build uses the
 * real <linux/refcount.h> implementations.
 */
static inline void refcount_set(refcount_t *r, int v)
{
	atomic_set(&r->refs, v);
}

static inline int refcount_read(refcount_t *r)
{
	return atomic_read(&r->refs);
}

static inline bool refcount_inc_not_zero(refcount_t *r)
{
	int v = atomic_read(&r->refs);

	if (!v)
		return false;
	atomic_set(&r->refs, v + 1);
	return true;
}

static inline void refcount_inc(refcount_t *r)
{
	atomic_set(&r->refs, atomic_read(&r->refs) + 1);
}

static inline bool refcount_dec_and_test(refcount_t *r)
{
	int v = atomic_read(&r->refs);

	atomic_set(&r->refs, v - 1);
	return v - 1 == 0;
}

#endif /* _LINUX_REFCOUNT_H */
