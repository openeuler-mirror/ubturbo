// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ubcore types
 */
#ifndef UBCORE_TYPES_H
#define UBCORE_TYPES_H

#include <linux/list.h>
#include <linux/compiler_attributes.h>
#include <linux/mutex.h>
#include "ubcore_opcode.h"

#define UBCORE_ACCESS_LOCAL_WRITE   0x1
#define UBCORE_ACCESS_REMOTE_READ   (0x1 << 1)
#define UBCORE_ACCESS_REMOTE_WRITE  (0x1 << 2)
#define UBCORE_ACCESS_REMOTE_ATOMIC (0x1 << 3)
#define UBCORE_ACCESS_REMOTE_INVALIDATE (0x1 << 4)
#define UBCORE_ACCESS_READ UBCORE_ACCESS_REMOTE_READ
#define UBCORE_ACCESS_WRITE UBCORE_ACCESS_REMOTE_WRITE
#define UBCORE_MAX_DEV_NAME 20

struct ubcore_device {
    char dev_name[UBCORE_MAX_DEV_NAME];
};
struct ubcore_client {
    struct list_head list_node;
    char *client_name;
    int (*add)(struct ubcore_device *dev);
    void (*remove)(struct ubcore_device *dev, void *client_ctx);
};
struct ubcore_tjetty {
};
struct ubcore_token {
    uint32_t token;
};
enum ubcore_target_type {
    UBCORE_JFR = 0,
    UBCORE_JETTY,
    UBCORE_JETTY_GROUP
};
enum ubcore_tp_type {
    UBCORE_RTP,
    UBCORE_CTP,
    UBCORE_UTP
};
enum ubcore_transport_mode {
    UBCORE_TP_RM = 0x1,
    UBCORE_TP_RC = 0x1 << 1,
    UBCORE_TP_UM = 0x1 << 2
};
union ubcore_jfr_flag {
    struct {
        uint32_t token_policy : 3;
        uint32_t tag_matching : 1;
        uint32_t lock_free : 1;
        uint32_t sub_trans_mode : 8;
        uint32_t reserved : 19;
    } bs;
    uint32_t value;
};
struct ubcore_jfr_cfg {
    uint32_t id;
    uint32_t eid_index;
    uint32_t depth;
    union ubcore_jfr_flag flag;
    struct ubcore_token token_value;
    enum ubcore_transport_mode trans_mode;
    struct ubcore_jfc *jfc;
    uint8_t min_rnr_timer;
    uint8_t max_sge;
};

union ubcore_jfc_flag {
    struct {
        uint32_t lock_free : 1;
        uint32_t jfc_inline : 1;
        uint32_t reserved : 30;
    } bs;
    uint32_t value;
};
struct ubcore_jfc_cfg {
    uint32_t depth;
    union ubcore_jfc_flag flag;
    uint32_t ceqn;
    void *jfc_context;
};
#define UBCORE_EID_SIZE (16)
union ubcore_eid {
    uint8_t raw[UBCORE_EID_SIZE];
    struct {
        uint64_t reserved;
        uint32_t prefix;
        uint32_t addr;
    } in4;
    struct {
        uint64_t subnet_prefix;
        uint64_t interface_id;
    } in6;
};
struct ubcore_jetty_id {
    union ubcore_eid eid;
    uint32_t id;
};
struct ubcore_jfr {
    struct ubcore_jetty_id jfr_id;
    struct ubcore_jfr_cfg jfr_cfg;
};
struct ubcore_jfs {
};
struct ubcore_jfc {
    uint64_t urma_jfc;
};
struct ubcore_cr {
    enum ubcore_cr_status status;
};
union ubcore_reg_seg_flag {
    struct {
        uint32_t token_policy : 3;
        uint32_t cacheable : 1;
        uint32_t dsva : 1;
        uint32_t access : 6;
        uint32_t non_pin : 1;
        uint32_t user_iova : 1;
        uint32_t token_id_valid : 1;
        uint32_t pa : 1;
        uint32_t reserved : 17;
    } bs;
    uint32_t value;
};
struct ubcore_seg_cfg {
    uint64_t va;
    uint64_t len;
    uint32_t eid_index;
    union ubcore_reg_seg_flag flag;
};
union ubcore_jfs_flag {
    struct {
        uint32_t lock_free : 1;
        uint32_t error_suspend : 1;
        uint32_t outorder_comp : 1;
        uint32_t sub_trans_mode : 8;
        uint32_t reserved : 21;
    } bs;
    uint32_t value;
};
struct ubcore_jfs_cfg {
    uint32_t depth;
    union ubcore_jfs_flag flag;
    enum ubcore_transport_mode trans_mode;
    uint32_t eid_index;
    uint8_t priority;
    uint8_t max_sge;
    uint8_t max_rsge;
    uint32_t max_inline_data;
    uint8_t rnr_retry;
    uint8_t err_timeout;
    void *jfs_context;
    struct ubcore_jfc *jfc;
};
union ubcore_import_jetty_flag {
    struct {
        uint32_t token_policy : 3;
        uint32_t sub_trans_mode : 8;
        uint32_t rc_share_tp : 1;
        uint32_t ctp : 1;
        uint32_t reserved : 19;
    } bs;
    uint32_t value;
};
struct ubcore_tjetty_cfg {
    struct ubcore_token token_value;
    union ubcore_import_jetty_flag flag;
    enum ubcore_transport_mode trans_mode;
    struct ubcore_jetty_id id;
    uint32_t eid_index;
    enum ubcore_target_type type;
    enum ubcore_tp_type tp_type;
};
struct ubcore_udata {
};
struct ubcore_ucontext {
};
struct ubcore_event {
};
struct ubcore_ubva {
    union ubcore_eid eid;
    uint64_t va;
};
union ubcore_seg_attr {
    struct {
        uint32_t token_policy : 3;
        uint32_t cacheable : 1;
        uint32_t dsva : 1;
        uint32_t access : 6;
        uint32_t non_pin : 1;
        uint32_t user_iova : 1;
        uint32_t user_token_id : 1;
        uint32_t pa : 1;
        uint32_t reserved : 17;
    } bs;
    uint32_t value;
};
struct ubcore_seg {
    struct ubcore_ubva ubva;
    uint64_t len;
    union ubcore_seg_attr attr;
    uint32_t token_id;
};
struct ubcore_target_seg_cfg {
    struct ubcore_seg seg;
};
struct ubcore_token_id {
    uint32_t token_id;
};
struct ubcore_target_seg {
    struct ubcore_token_id *token_id;
    struct ubcore_seg seg;
};
struct ubcore_sge {
    uint64_t addr;
    uint32_t len;
    struct ubcore_target_seg *tseg;
};
struct ubcore_sg {
    struct ubcore_sge *sge;
    uint32_t num_sge;
};

struct ubcore_rw_wr {
    struct ubcore_sg src;
    struct ubcore_sg dst;
};

union ubcore_jfs_wr_flag {
    struct {
        uint32_t place_order : 2;
        uint32_t comp_order : 1;
        uint32_t fence : 1;
        uint32_t solicited_enable : 1;
        uint32_t complete_enable : 1;
        uint32_t inline_flag : 1;
        uint32_t reserved : 25;
    } bs;
    uint32_t value;
};

struct ubcore_jfs_wr {
    enum ubcore_opcode opcode;
    union ubcore_jfs_wr_flag flag;
    struct ubcore_tjetty *tjetty;
    union {
        struct ubcore_rw_wr rw;
    };
};
struct ubcore_eid_info {
    union ubcore_eid eid;
    uint32_t eid_index;
};
typedef void (*ubcore_event_callback_t)(struct ubcore_event *event, struct ubcore_ucontext *ctx);
typedef void (*ubcore_comp_callback_t)(struct ubcore_jfc *jfc);
#endif