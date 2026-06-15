// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: smap memory notifier
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/memory.h>

#include "bus.h"
#include "check.h"
#include "access_acpi_mem.h"
#include "access_iomem.h"
#include "access_tracking.h"
#include "memory_notifier.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_memory_notifier: " fmt

#define SMAP_MN_CALLBACK_PRI 100

static int memory_notifier_cb(struct memory_notify *mnb, unsigned long action)
{
	int ret;
	int nid;
	struct page *page;
	unsigned long start_pfn = mnb->start_pfn;

	pr_debug("remote memory %s detected\n",
		 (action == MEM_ONLINE ? "online" : "offline"));

	if (smap_scene == UB_QEMU_SCENE)
		return 0;

	/* Find the node which the memory block belongs to */
	if (action == MEM_ONLINE) {
		if (!pfn_valid(start_pfn)) {
			pr_err("invalid start pfn has passed to SMAP memory notifier\n");
			return -EINVAL;
		}
		page = pfn_to_online_page(start_pfn);
		if (!page) {
			pr_err("unable to find page according to pfn\n");
			return -EINVAL;
		}
		nid = page_to_nid(page);
	} else {
		nid = get_numa_by_pfn(start_pfn);
	}
	if (nid == NUMA_NO_NODE) {
		pr_warn("unable to find NUMA node according to pfn\n");
	}

	ret = refresh_remote_ram();
	if (ret) {
		pr_err("unable to update remote memory info, ret: %d\n", ret);
		return ret;
	}
	pr_debug(
		"update remote memory info on remote NUMA node: %d successfully\n",
		nid);

	/* set delayed reinit flag instead of immediate reinit */
	set_reinit_pending_flag(nid);
	pr_debug("set reinit pending flag for NUMA node: %d\n", nid);

	return 0;
}

static int pre_offline_notifier_cb(struct memory_notify *mnb,
				   unsigned long action)
{
	pr_info("memory is going offline\n");

	if (smap_scene == UB_QEMU_SCENE)
		return 0;

	return 0;
}

static int smap_memory_notifier(struct notifier_block *self,
				unsigned long action, void *arg)
{
	struct memory_notify *mnb = (struct memory_notify *)arg;
	int ret = 0;

	switch (action) {
	case MEM_ONLINE:
	case MEM_OFFLINE:
		ret = memory_notifier_cb(mnb, action);
		break;
	case MEM_GOING_OFFLINE:
		ret = pre_offline_notifier_cb(mnb, action);
		break;
	default:
		break;
	}

	pr_debug("handle memory notify, ret: %d\n", ret);
	return NOTIFY_OK;
}

static struct notifier_block smap_nb = {
	.notifier_call = smap_memory_notifier,
	.priority = SMAP_MN_CALLBACK_PRI,
};

int memory_notifier_init(void)
{
	int ret = register_memory_notifier(&smap_nb);
	return ret;
}

void memory_notifier_exit(void)
{
	unregister_memory_notifier(&smap_nb);
}
