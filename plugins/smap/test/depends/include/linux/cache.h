/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CACHE_H
#define _LINUX_CACHE_H

/* Test stub: cache-line alignment is irrelevant in the single-threaded
 * unit-test harness; define the attribute away so kernel structs that use
 * ____cacheline_aligned_in_smp compile unchanged.
 */
#ifndef ____cacheline_aligned_in_smp
#define ____cacheline_aligned_in_smp
#endif
#ifndef __cacheline_aligned
#define __cacheline_aligned
#endif

#endif /* _LINUX_CACHE_H */
