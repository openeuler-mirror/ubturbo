// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP DT memory module
 */

#include <linux/of.h>
#include <linux/mm.h>

#include "check.h"
#include "access_dt_mem.h"
#include "access_acpi_mem.h"

#undef pr_fmt
#define pr_fmt(fmt) "smap_drv_dt: " fmt

static void merge_dt_mem_segments(void)
{
	struct acpi_mem_segment *cur, *next, *tmp;

	if (list_empty(&acpi_mem.mem))
		return;

	cur = list_first_entry(&acpi_mem.mem, struct acpi_mem_segment, segment);
	while (cur) {
		next = list_next_entry(cur, segment);
		if (list_entry_is_head(next, &acpi_mem.mem, segment))
			break;
		if (cur->node == next->node && cur->end + 1 == next->start) {
			cur->end = next->end;
			list_del(&next->segment);
			acpi_mem.len--;
			kfree(next);
			tmp = cur;
		} else {
			tmp = next;
		}
		cur = tmp;
	}
}

static void update_nr_local_numa_dt(int node)
{
	if (node >= nr_local_numa) {
		nr_local_numa = node + 1;
		pr_info("local NUMA nodes amount: %u (from node %d)\n",
			nr_local_numa, node);
	}
}

static int dt_insert_mem_segment(struct acpi_mem_segment *mem)
{
	struct acpi_mem_segment *tmp;

	if (list_empty(&acpi_mem.mem)) {
		list_add_tail(&mem->segment, &acpi_mem.mem);
	} else {
		list_for_each_entry_reverse(tmp, &acpi_mem.mem, segment) {
			if (tmp->start < mem->start)
				break;
		}
		if (list_entry_is_head(tmp, &acpi_mem.mem, segment))
			list_add(&mem->segment, &acpi_mem.mem);
		else
			list_add(&mem->segment, &tmp->segment);
	}
	acpi_mem.len++;

	return 0;
}

int init_dt_mem(void)
{
	struct device_node *dn;
	u32 nid;
	u64 base, size;
	struct acpi_mem_segment *mem;
	int ret;

	for_each_node_by_type(dn, "memory") {
		ret = of_property_read_u32(dn, "numa-node-id", &nid);
		if (ret) {
			pr_debug("memory node %pOF has no numa-node-id, skip\n",
				 dn);
			continue;
		}

		ret = of_property_read_reg(dn, 0, &base, &size);
		if (ret) {
			pr_warn("memory node %pOF has no reg property, ret: %d\n",
				dn, ret);
			continue;
		}

		if (size == 0) {
			pr_debug("memory node %pOF has zero size, skip\n", dn);
			continue;
		}

		mem = kmalloc(sizeof(*mem), GFP_KERNEL);
		if (!mem)
			return -ENOMEM;

		mem->start = base;
		mem->end = base + size - 1;
		mem->node = nid;
		mem->pxm = nid;

		update_nr_local_numa_dt(mem->node);
		dt_insert_mem_segment(mem);

		pr_debug("memory segment: node=%d, start=%#llx, end=%#llx\n",
			 mem->node, mem->start, mem->end);
	}

	if (list_empty(&acpi_mem.mem)) {
		pr_warn("no valid memory segments found from DT\n");
		return -ENODEV;
	}

	merge_dt_mem_segments();

	pr_info("init_dt_mem: nr_local_numa = %d, segments = %zu\n",
		nr_local_numa, acpi_mem.len);
	return 0;
}

void reset_dt_mem(void)
{
	struct acpi_mem_segment *mem, *tmp;

	list_for_each_entry_safe(mem, tmp, &acpi_mem.mem, segment) {
		list_del(&mem->segment);
		kfree(mem);
	}
	acpi_mem.len = 0;
	nr_local_numa = 0;
}
