/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP NUMA module
 */

#ifndef _SRC_TIERING_NUMA_H
#define _SRC_TIERING_NUMA_H

#include <linux/nodemask.h>
#include "kernel_common.h"

unsigned long get_node_nr_free_pages(int nid);

/*
 * A node id coming from user space must not only be in range but also be
 * online: NODE_DATA() of a never-onlined node may be NULL, and allocating
 * on it (node_zonelist()) dereferences that NULL pointer.
 */
static inline bool is_node_invalid(int node)
{
	return node < 0 || node >= MAX_NUMNODES || !node_online(node);
}

#endif /* _SRC_TIERING_NUMA_H */
