/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stub of linux/overflow.h for DT build.
 * Mirrors the kernel semantics with GCC/Clang overflow builtins.
 */
#ifndef __LINUX_OVERFLOW_H
#define __LINUX_OVERFLOW_H

#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)

#endif /* __LINUX_OVERFLOW_H */
