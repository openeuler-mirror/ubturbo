// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMAP unified module entry.
 *
 * The scan and migration subsystems share one module lifecycle.
 */
#include <linux/module.h>

#include "scan/scan_main.h"
#include "migrate/migrate_main.h"

static int __init smap_init(void)
{
	int ret;

	ret = scan_init();
	if (ret)
		return ret;

	ret = migrate_init();
	if (ret)
		goto err_scan;

	return 0;

err_scan:
	scan_exit();
	return ret;
}

static void __exit smap_exit(void)
{
	migrate_exit();
	scan_exit();
}

module_init(smap_init);
module_exit(smap_exit);

MODULE_DESCRIPTION("SMAP tiered memory tracking and migration");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL v2");
