// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ub-dma urma adapter
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "ubcore_uapi.h"
#include "ubcore_api.h"
#include "ub_dma_log.h"
#include "urma.h"

#define MIN_PORT_NUMA 2

#define JFS_CFG_PRIORITY 15
#define JFS_CFG_MAX_SGE 2
#define JFS_CFG_RSGE 1
#define JFS_CFG_MAX_INLINE_DATA 32
#define JFS_CFG_ERR_TIMEOUT 14

#define JFR_CFG_MAX_SGE 2
#define JFR_CFG_MIN_RNR_TIMER 12

LIST_HEAD(dev_list_head);

static struct urma_mem_trans *g_urma_jetty;

struct ubcore_dev_list {
	struct ubcore_device *dev;
	struct list_head list;
};

const struct ubcore_seg_cfg default_seg_cfg = {
	.flag.bs.token_policy = UBCORE_TOKEN_NONE,
	.flag.bs.cacheable = UBCORE_CACHEABLE,
	.flag.bs.dsva = false,
	.flag.bs.access = UBCORE_ACCESS_READ | UBCORE_ACCESS_WRITE,
	.flag.bs.non_pin = true,
	.flag.bs.user_iova = false,
	.flag.bs.token_id_valid = false,
	.flag.bs.pa = false,
};

const struct ubcore_jfc_cfg default_jfc_cfg = {
	.depth = UB_DMA_JETTY_DEPTH,
	.flag.bs.lock_free = false,
	.flag.bs.jfc_inline = false,
	.ceqn = 0,
};

const struct ubcore_jfr_cfg default_jfr_cfg = {
	.id = 0,
	.depth = UB_DMA_JETTY_DEPTH,
	.flag.bs.token_policy = UBCORE_TOKEN_NONE,
	.flag.bs.lock_free = false,
	.flag.bs.tag_matching = false,
	.trans_mode = UBCORE_TP_RM,
	.max_sge = JFR_CFG_MAX_SGE,
	.min_rnr_timer = JFR_CFG_MIN_RNR_TIMER,
};

const struct ubcore_jfs_cfg default_jfs_cfg = {
	.depth = UB_DMA_JETTY_DEPTH,
	.flag.bs.lock_free = false,
	.flag.bs.error_suspend = false,
	.flag.bs.outorder_comp = false,
	.trans_mode = UBCORE_TP_RM,
	.priority = JFS_CFG_PRIORITY,
	.max_sge = JFS_CFG_MAX_SGE,
	.max_rsge = JFS_CFG_RSGE,
	.max_inline_data = JFS_CFG_MAX_INLINE_DATA,
	.rnr_retry = 0,
	.err_timeout = JFS_CFG_ERR_TIMEOUT,
	.jfs_context = NULL,
};

static inline struct ubcore_target_seg *
init_segment(uint64_t va, uint64_t size, struct urma_mem_trans *trans)
{
	struct ubcore_seg_cfg cfg = default_seg_cfg;

	cfg.va = va;
	cfg.len = size;
	cfg.eid_index = trans->eid_info.eid_index;

	return ubcore_register_seg(trans->dev, &cfg, NULL);
}

static inline struct ubcore_sge *init_sge(struct ubcore_target_seg *seg,
					  uint64_t addr, uint64_t len)
{
	struct ubcore_sge *sge;
	sge = kzalloc(sizeof(struct ubcore_sge), GFP_KERNEL);
	if (!sge) {
		ub_dma_log_err("fail to allocate sge\n");
		return NULL;
	}
	sge->addr = addr;
	sge->len = len;
	sge->tseg = seg;
	return sge;
}

static int init_wr_list(struct ubcore_tjetty *tjetty,
			struct ubcore_target_seg *lseg,
			struct ubcore_target_seg *rseg,
			struct ubcore_jfs_wr **head_wr, uint64_t src_va,
			uint64_t dst_va, uint32_t source_len)
{
	struct ubcore_jfs_wr *wr;
	int ret = 0;

	wr = kzalloc(sizeof(*wr), GFP_KERNEL);
	if (!wr) {
		ub_dma_log_err("failed to allocate wr.\n");
		return -ENOMEM;
	}
	wr->rw.src.sge = init_sge(lseg, src_va, source_len);
	if (!wr->rw.src.sge) {
		ub_dma_log_err("init src ubcore sge failed.\n");
		ret = -ENOMEM;
		goto err_free_wr;
	}
	wr->rw.dst.sge = init_sge(rseg, dst_va, source_len);
	if (!wr->rw.dst.sge) {
		ub_dma_log_err("init dst ubcore sge failed.\n");
		ret = -ENOMEM;
		goto err_free_wr_src;
	}
	wr->tjetty = tjetty;
	wr->opcode = UBCORE_OPC_WRITE;
	wr->flag.bs.complete_enable = true;
	wr->rw.src.num_sge = 1;
	wr->rw.dst.num_sge = 1;

	*head_wr = wr;
	return ret;

err_free_wr_src:
	kfree(wr->rw.src.sge);
err_free_wr:
	kfree(wr);

	return ret;
}

static inline void free_wr_list(struct ubcore_jfs_wr *wr)
{
	kfree(wr->rw.src.sge);
	kfree(wr->rw.dst.sge);
	kfree(wr);
	wr = NULL;
}

static int post_client_jfs(struct ubcore_target_seg *src_seg,
			   struct ubcore_target_seg *dst_seg, uint64_t src_va,
			   uint64_t dst_va, uint32_t source_len)
{
	struct ubcore_jfs_wr *header_wr = NULL;
	struct ubcore_jfs_wr *fail_wr;
	int ret;

	ret = init_wr_list(g_urma_jetty->tjetty, src_seg, dst_seg, &header_wr,
			   src_va, dst_va, source_len);
	if (ret) {
		ub_dma_log_err("failed to init wr list\n");
		return -ENOMEM;
	}

	ret = ubcore_post_jfs_wr(g_urma_jetty->client_jfs, header_wr, &fail_wr);
	if (ret) {
		ub_dma_log_err("failed to post wr.\n");
		ret = -EFAULT;
	}

	free_wr_list(header_wr);
	return ret;
}

static struct ubcore_target_seg *
urma_import_segment(uint64_t start_va, uint64_t len,
		    struct ubcore_target_seg *target_sge)
{
	struct ubcore_target_seg_cfg cfg = { 0 };
	struct ubcore_target_seg *seg;

	cfg.seg.ubva.va = start_va;
	cfg.seg.ubva.eid = g_urma_jetty->eid_info.eid;
	cfg.seg.len = len;
	cfg.seg.token_id = target_sge->token_id->token_id;
	cfg.seg.attr.bs.cacheable = UBCORE_CACHEABLE;
	cfg.seg.attr.bs.access = UBCORE_ACCESS_READ | UBCORE_ACCESS_WRITE;

	seg = ubcore_import_seg(g_urma_jetty->dev, &cfg, NULL);
	if (IS_ERR_OR_NULL(seg)) {
		ub_dma_log_err("fail to import segment\n");
		return NULL;
	}

	return seg;
}

int urma_register_segment(uint64_t start_va, uint64_t end_va,
			  struct urma_sge_info *sge_info)
{
	struct ubcore_target_seg *sge;
	struct ubcore_target_seg *i_seg;
	uint64_t len = end_va - start_va + 1;

	sge = init_segment(start_va, len, g_urma_jetty);
	if (IS_ERR_OR_NULL(sge)) {
		ub_dma_log_err("fail to init seg\n");
		return -ENOMEM;
	}

	i_seg = urma_import_segment(start_va, len, sge);
	if (IS_ERR_OR_NULL(i_seg)) {
		ub_dma_log_err("urma import segment failed\n");
		ubcore_unregister_seg(sge);
		return -ENOMEM;
	}

	sge_info->sge = sge;
	sge_info->i_seg = i_seg;
	sge_info->start_va = start_va;
	sge_info->end_va = end_va;

	return 0;
}

int urma_register_tmp_segment(struct urma_trans_segment_info *info,
			      struct tmp_urma_segment_info *tmp_info,
			      bool need_import_sge)
{
	struct ubcore_target_seg *sge;
	struct ubcore_target_seg *i_seg;

	sge = init_segment(info->addr, info->len, g_urma_jetty);
	if (IS_ERR_OR_NULL(sge)) {
		ub_dma_log_err(
			"urma register trans segment failed to init seg\n");
		return -ENOMEM;
	}

	if (need_import_sge) {
		i_seg = urma_import_segment(info->addr, info->len, sge);
		if (IS_ERR_OR_NULL(i_seg)) {
			ub_dma_log_err("urma import segment failed\n");
			ubcore_unregister_seg(sge);
			return -ENOMEM;
		}
		tmp_info->seg = sge;
		tmp_info->i_seg = i_seg;
		info->sge = i_seg;
	} else {
		tmp_info->seg = sge;
		tmp_info->i_seg = NULL;
		info->sge = sge;
	}

	return 0;
}

void unregister_urma_segment(struct urma_sge_info *sge_info)
{
	ubcore_unimport_seg(sge_info->i_seg);
	ubcore_unregister_seg(sge_info->sge);
}

int urma_run_send(uint64_t src_va, uint64_t dst_va,
		  struct ubcore_target_seg *src_seg,
		  struct ubcore_target_seg *dst_seg, uint32_t len)
{
	int ret = 0;

	mutex_lock(&g_urma_jetty->mutex_lock);
	ret = post_client_jfs(src_seg, dst_seg, src_va, dst_va, len);
	mutex_unlock(&g_urma_jetty->mutex_lock);
	if (ret) {
		ub_dma_log_err("fail to post jfs\n");
	}

	return ret;
}

static inline struct ubcore_jfc *create_jfc(struct urma_mem_trans *trans,
					    ubcore_comp_callback_t jfce_handler)
{
	struct ubcore_jfc_cfg cfg = default_jfc_cfg;

	return ubcore_create_jfc(trans->dev, &cfg, jfce_handler, NULL, NULL);
}

static struct ubcore_jfs *create_client_jfs(struct urma_mem_trans *trans)
{
	struct ubcore_jfs_cfg cfg = default_jfs_cfg;

	cfg.eid_index = trans->eid_info.eid_index;
	cfg.jfc = trans->client_jfc;
	return ubcore_create_jfs(trans->dev, &cfg, NULL, NULL);
}

static struct ubcore_jfr *create_server_jfr(struct urma_mem_trans *trans)
{
	struct ubcore_jfr_cfg cfg = default_jfr_cfg;

	cfg.eid_index = trans->eid_info.eid_index;
	cfg.jfc = trans->server_jfc;
	return ubcore_create_jfr(trans->dev, &cfg, NULL, NULL);
}

static struct ubcore_tjetty *import_server_jfr(struct urma_mem_trans *trans)
{
	struct ubcore_tjetty_cfg cfg = { 0 };
	cfg.eid_index = trans->server_jfr->jfr_cfg.eid_index;
	cfg.id.eid = trans->eid_info.eid;
	cfg.id.id = trans->server_jfr->jfr_id.id;
	cfg.trans_mode = trans->server_jfr->jfr_cfg.trans_mode;
	cfg.token_value = trans->server_jfr->jfr_cfg.token_value;
	cfg.flag.bs.token_policy =
		trans->server_jfr->jfr_cfg.flag.bs.token_policy;
	cfg.type = UBCORE_JFR;
	cfg.tp_type = UBCORE_CTP;

	return ubcore_import_jfr(trans->dev, &cfg, NULL);
}

static int get_trans_entity(struct urma_mem_trans *trans)
{
	struct ubcore_eid_info *eid_list;
	struct ubcore_dev_list *dev_node;
	uint32_t eid_cnt;

	list_for_each_entry(dev_node, &dev_list_head, list) {
		eid_list = ubcore_get_eid_list(dev_node->dev, &eid_cnt);
		if (IS_ERR_OR_NULL(eid_list)) {
			continue;
		}
		if (eid_cnt < 1) {
			continue;
		}
		if (eid_cnt < MIN_PORT_NUMA) {
			continue;
		}
		trans->dev = dev_node->dev;
		trans->eid_info = eid_list[eid_cnt - 1];
		ubcore_free_eid_list(eid_list);
		return 0;
	}

	ubcore_free_eid_list(eid_list);
	return -ENODEV;
}

static int urma_add_device(struct ubcore_device *dev)
{
	struct ubcore_dev_list *dev_node =
		kzalloc(sizeof(*dev_node), GFP_KERNEL);
	if (!dev_node) {
		ub_dma_log_err("failed to allocate dev node.\n");
		return -ENOMEM;
	}
	dev_node->dev = dev;
	list_add_tail(&dev_node->list, &dev_list_head);
	ub_dma_log_info("dev add to list.\n");
	return 0;
}

static void urma_remove_device(struct ubcore_device *dev,
			       void *d __always_unused)
{
	struct ubcore_dev_list *dev_node;
	ub_dma_log_info("remove dev from dev list.\n");
	list_for_each_entry(dev_node, &dev_list_head, list) {
		if (dev_node->dev == dev) {
			list_del(&dev_node->list);
			kfree(dev_node);
			break;
		}
	}
}

static struct ubcore_client urma_ubcore_client = {
	.list_node = LIST_HEAD_INIT(urma_ubcore_client.list_node),
	.client_name = "urma_ubcore_client",
	.add = urma_add_device,
	.remove = urma_remove_device,
};

int init_urma_mem_trans(ubcore_comp_callback_t jfce_handler)
{
	int ret;

	if (!jfce_handler) {
		ub_dma_log_err("ubcore comp callback is null\n");
		return -EINVAL;
	}
	if (!list_empty(&dev_list_head)) {
		ub_dma_log_err("hw_clear is already setup\n");
		return -EEXIST;
	}
	ret = ubcore_register_client(&urma_ubcore_client);
	if (ret) {
		ub_dma_log_err("fail to register ubcore client\n");
		return -EFAULT;
	}
	if (list_empty(&dev_list_head)) {
		ub_dma_log_err("fail to get ubcore device\n");
		ret = -ENODEV;
		goto err_unregister;
	}

	g_urma_jetty = kzalloc(sizeof(struct urma_mem_trans), GFP_KERNEL);
	if (!g_urma_jetty) {
		ub_dma_log_err("failed to allocate handler\n");
		ret = -ENOMEM;
		goto err_unregister;
	}

	g_urma_jetty->cr = kzalloc(
		sizeof(struct ubcore_cr) * UB_DMA_JETTY_DEPTH, GFP_KERNEL);
	if (!g_urma_jetty->cr) {
		ub_dma_log_err("failed to allocate handler\n");
		ret = -ENOMEM;
		goto free_trans;
	}

	ret = get_trans_entity(g_urma_jetty);
	if (ret) {
		ub_dma_log_err("failed to get entity\n");
		goto free_cr;
	}

	mutex_init(&g_urma_jetty->mutex_lock);
	g_urma_jetty->server_jfc = create_jfc(g_urma_jetty, NULL);
	if (IS_ERR_OR_NULL(g_urma_jetty->server_jfc)) {
		ub_dma_log_err("fail to create server jfc\n");
		ret = PTR_ERR(g_urma_jetty->server_jfc);
		goto free_cr;
	}

	g_urma_jetty->client_jfc = create_jfc(g_urma_jetty, jfce_handler);
	if (IS_ERR_OR_NULL(g_urma_jetty->client_jfc)) {
		ub_dma_log_err("fail to create client jfc\n");
		ret = PTR_ERR(g_urma_jetty->client_jfc);
		goto delete_server_jfc;
	}
	ret = ubcore_rearm_jfc(g_urma_jetty->client_jfc, false);
	if (ret) {
		ub_dma_log_err("ub core rearm jfc fail.\n");
		goto delete_client_jfc;
	}

	g_urma_jetty->client_jfs = create_client_jfs(g_urma_jetty);
	if (IS_ERR_OR_NULL(g_urma_jetty->client_jfs)) {
		ub_dma_log_err("fail to create client jfs\n");
		ret = PTR_ERR(g_urma_jetty->client_jfs);
		goto delete_client_jfc;
	}
	g_urma_jetty->server_jfr = create_server_jfr(g_urma_jetty);
	if (IS_ERR_OR_NULL(g_urma_jetty->server_jfr)) {
		ub_dma_log_err("fail to create server jfr\n");
		ret = PTR_ERR(g_urma_jetty->server_jfr);
		goto delete_client_jfs;
	}
	g_urma_jetty->tjetty = import_server_jfr(g_urma_jetty);
	if (IS_ERR_OR_NULL(g_urma_jetty->tjetty)) {
		ub_dma_log_err("fail to import tjetty\n");
		ret = PTR_ERR(g_urma_jetty->tjetty);
		goto delete_server_jfr;
	}

	ub_dma_log_info("udma is %s, eid index is %d",
			g_urma_jetty->dev->dev_name,
			g_urma_jetty->eid_info.eid_index);
	return 0;

delete_server_jfr:
	ubcore_delete_jfr(g_urma_jetty->server_jfr);
delete_client_jfs:
	ubcore_delete_jfs(g_urma_jetty->client_jfs);
delete_client_jfc:
	ubcore_delete_jfc(g_urma_jetty->client_jfc);
delete_server_jfc:
	ubcore_delete_jfc(g_urma_jetty->server_jfc);
free_cr:
	kfree(g_urma_jetty->cr);
free_trans:
	kfree(g_urma_jetty);
err_unregister:
	ubcore_unregister_client(&urma_ubcore_client);
	return ret;
}

struct urma_mem_trans *get_urma_jetty(void)
{
	return g_urma_jetty;
}

void release_urma_mem_trans(void)
{
	if (!g_urma_jetty) {
		return;
	}

	ubcore_unimport_jfr(g_urma_jetty->tjetty);
	ubcore_delete_jfr(g_urma_jetty->server_jfr);
	ubcore_delete_jfs(g_urma_jetty->client_jfs);
	ubcore_delete_jfc(g_urma_jetty->client_jfc);
	ubcore_delete_jfc(g_urma_jetty->server_jfc);
	mutex_destroy(&g_urma_jetty->mutex_lock);
	kfree(g_urma_jetty->cr);
	g_urma_jetty->cr = NULL;
	kfree(g_urma_jetty);
	g_urma_jetty = NULL;
	ubcore_unregister_client(&urma_ubcore_client);
}