/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SMAP critical module
 */

#ifndef _CRITICAL_H
#define _CRITICAL_H

#ifdef __cplusplus
extern "C" {
#endif
static inline bool smap_node_is_critical_err(int nid)
{
	return false;
}
#undef node_is_critical_err

#define node_is_critical_err(nid) smap_node_is_critical_err(nid)

#ifdef __cplusplus
}
#endif
#endif /* _CRITICAL_H */