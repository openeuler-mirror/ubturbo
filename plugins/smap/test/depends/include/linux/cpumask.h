/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_CPUMASK_H
#define __LINUX_CPUMASK_H

#include <linux/threads.h>
#include <linux/bitops.h>

#define num_online_cpus()	2U

#define num_possible_cpus() 2U

#define for_each_online_cpu(cpu)	for ((cpu) = 0; (cpu) < 2; (cpu)++)

typedef struct cpumask { unsigned long bits[BITS_TO_LONGS(NR_CPUS)]; } cpumask_t;

typedef struct cpumask cpumask_var_t[1];

static inline void cpumask_clear(struct cpumask *dstp)
{
    unsigned int i;
    for (i = 0; i < BITS_TO_LONGS(NR_CPUS); i++)
        dstp->bits[i] = 0;
}

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *dstp)
{
    if (cpu < NR_CPUS)
        dstp->bits[cpu / BITS_PER_LONG] |= 1UL << (cpu % BITS_PER_LONG);
}

static inline int cpu_online(int cpu)
{
    return cpu >= 0 && cpu < (int)num_online_cpus();
}

#endif /* __LINUX_CPUMASK_H */
