/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_UIO_H
#define __LINUX_UIO_H

#include <stdbool.h>

struct kvec {
	void *iov_base; /* and that should *never* hold a userland pointer */
	size_t iov_len;
};

struct iov_iter {
	const struct kvec *kvec;
	unsigned long nr_segs;
};

/* DT 桩环境仅构造 kvec 迭代，无 iter type 字段，恒真；
 * nr_segs 校验仍由调用方完成 */
static inline bool iov_iter_is_kvec(const struct iov_iter *i)
{
	return (bool)i;
}

#endif /* __LINUX_UIO_H */
