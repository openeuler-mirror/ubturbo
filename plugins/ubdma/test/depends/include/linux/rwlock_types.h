// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Create: 2025-2-20
 */
#ifndef __LINUX_RWLOCK_TYPES_H
#define __LINUX_RWLOCK_TYPES_H
#include <linux/spinlock_types_up.h>
#if !defined(__LINUX_SPINLOCK_TYPES_H)
# error "Do not"
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define RW_DEP_MAP_INIT(lockname) \
    .dep_map = { \
        .name = #lockname, \
        .wait_type_inner = LD_WAIT_CONFIG, \
    }
#else
#define RW_DEP_MAP_INIT(lockname)
#endif

#ifndef CONFIG_PREEMPT_RT

typedef struct {
    arch_rwlock_t raw_lock;
#ifdef CONFIG_DEBUG_SPINLOCK
    unsigned int magic, owner_cpu;
    void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
} rwlock_t;

#define RWLOCK_MAGIC 0xdeaf1eed

#ifdef CONFIG_DEBUG_SPINLOCK
#define __RW_LOCK_UNLOCKED(lockname) \
    (rwlock_t) \
    { \
        .raw_lock = __ARCH_RW_LOCK_UNLOCKED, .magic = RWLOCK_MAGIC, .owner = SPINLOCK_OWNER_INIT, .owner_cpu = -1, \
        RW_DEP_MAP_INIT(lockname) \
    }
#else
#define __RW_LOCK_UNLOCKED(lockname) \
    (rwlock_t) \
    { \
        .raw_lock = __ARCH_RW_LOCK_UNLOCKED, RW_DEP_MAP_INIT(lockname) \
    }
#endif

#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_UNLOCKED(x)

#else

#include <linux/rwbase_rt.h>

typedef struct {
    struct rwbase_rt rwbase;
    atomic_t readers;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
} rwlock_t;

#define __RWLOCK_RT_INITIALIZER(name) \
    { \
        .rwbase = __RWBASE_INITIALIZER(name), RW_DEP_MAP_INIT(name) \
    }

#define __RW_LOCK_UNLOCKED(name) __RWLOCK_RT_INITIALIZER(name)
#define DEFINE_RWLOCK(name) rwlock_t name = __RW_LOCK_UNLOCKED(name)
#endif
#endif /* __LINUX_RWLOCK_TYPES_H */