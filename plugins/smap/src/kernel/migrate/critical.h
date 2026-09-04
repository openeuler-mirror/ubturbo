/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SMAP critical module
 */

#ifndef _CRITICAL_H
#define _CRITICAL_H

#ifndef CONFIG_ACPI_APEI_RAS_CRITICAL
static inline bool node_is_critical_err(int nid)
{
	return false;
}

#endif
#endif /* _CRITICAL_H */
