// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ubcore uapi
 */
#ifndef UBCORE_UAPI_H
#define UBCORE_UAPI_H

#include "ubcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif
void ubcore_unregister_client(struct ubcore_client *rm_client);
int ubcore_register_client(struct ubcore_client *new_client);
struct ubcore_tjetty *ubcore_import_jfr(struct ubcore_device *dev, struct ubcore_tjetty_cfg *cfg,
    struct ubcore_udata *udata);
struct ubcore_jfs *ubcore_create_jfs(struct ubcore_device *dev, struct ubcore_jfs_cfg *cfg,
    ubcore_event_callback_t jfae_handler, struct ubcore_udata *udata);
struct ubcore_jfc *ubcore_create_jfc(struct ubcore_device *dev, struct ubcore_jfc_cfg *cfg,
    ubcore_comp_callback_t jfce_handler, ubcore_event_callback_t jfae_handler,
    struct ubcore_udata *udata);
int ubcore_unregister_seg(struct ubcore_target_seg *tseg);
int ubcore_post_jfs_wr(struct ubcore_jfs *jfs, struct ubcore_jfs_wr *wr, struct ubcore_jfs_wr **bad_wr);
struct ubcore_target_seg *ubcore_register_seg(struct ubcore_device *dev, struct ubcore_seg_cfg *cfg,
    struct ubcore_udata *udata);
struct ubcore_target_seg *ubcore_import_seg(struct ubcore_device *dev, struct ubcore_target_seg_cfg *cfg,
    struct ubcore_udata *udata);
struct ubcore_jfr *ubcore_create_jfr(struct ubcore_device *dev, struct ubcore_jfr_cfg *cfg,
    ubcore_event_callback_t jfae_handler, struct ubcore_udata *udata);
struct ubcore_eid_info *ubcore_get_eid_list(struct ubcore_device *dev, uint32_t *cnt);
void ubcore_free_eid_list(struct ubcore_eid_info *eid_list);
int ubcore_rearm_jfc(struct ubcore_jfc *jfc, bool solicited_only);
int ubcore_delete_jfr(struct ubcore_jfr *jfr);
int ubcore_unimport_seg(struct ubcore_target_seg *tseg);
int ubcore_delete_jfs(struct ubcore_jfs *jfs);
int ubcore_poll_jfc(struct ubcore_jfc *jfc, int cr_cnt, struct ubcore_cr *cr);
int ubcore_flush_jfs(struct ubcore_jfs *jfs, int cr_cnt, struct ubcore_cr *cr);
int ubcore_unimport_jfr(struct ubcore_tjetty *tjfr);
int ubcore_delete_jfc(struct ubcore_jfc *jfc);
#ifdef __cplusplus
}
#endif
#endif