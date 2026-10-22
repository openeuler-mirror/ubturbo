// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMAP unified module entry.
 *
 * The scan and migration subsystems share one module lifecycle.
 */
#include <linux/module.h>
#include <linux/numa.h>

#include "common.h"
#include "scan/scan_main.h"
#include "migrate/migrate_main.h"

static int __init smap_init(void)
{
	int ret;

	/* 编译期守卫：内核节点空间上限不得超过 SMAP 共享定维常量。
	 * 内核内部数据结构按 MAX_NUMNODES 定维（随目标内核自适应）；
	 * ABI 流（readback/payload）按 SMAP_MAX_NUMNODES 定宽，两者经由
	 * write_bitmap_nrpage 的补 0 写出保持自洽
	 * （用户态运行期防线见 GetNodeCapFromStatus：解析 /proc/self/status
	 * Mems_allowed 打印宽度得到 MAX_NUMNODES） */
	BUILD_BUG_ON(SMAP_MAX_NUMNODES < MAX_NUMNODES);

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
