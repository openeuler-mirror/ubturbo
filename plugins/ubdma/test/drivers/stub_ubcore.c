/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UBDMA 驱动模块打桩文件
 */
#include <stdlib.h>
#include "linux/rwlock_rt.h"
#include "linux/workqueue.h"
#include "linux/dmaengine.h"
#include "ubcore_uapi.h"

struct ubcore_target_seg *ubcore_register_seg(struct ubcore_device *dev, struct ubcore_seg_cfg *cfg,
                                              struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_target_seg));
}

int ubcore_post_jfs_wr(struct ubcore_jfs *jfs, struct ubcore_jfs_wr *wr, struct ubcore_jfs_wr **bad_wr)
{
    return 0;
}

int ubcore_unregister_seg(struct ubcore_target_seg *tseg)
{
    free(tseg);
    return 0;
}

struct ubcore_jfc *ubcore_create_jfc(struct ubcore_device *dev, struct ubcore_jfc_cfg *cfg,
                                     ubcore_comp_callback_t jfce_handler, ubcore_event_callback_t jfae_handler,
                                     struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_jfc));
}

int ubcore_register_client(struct ubcore_client *new_client)
{
    return 0;
}

int ubcore_delete_jfr(struct ubcore_jfr *jfr)
{
    free(jfr);
    return 0;
}

int ubcore_delete_jfs(struct ubcore_jfs *jfs)
{
    free(jfs);
    return 0;
}

int ubcore_delete_jfc(struct ubcore_jfc *jfc)
{
    free(jfc);
    return 0;
}

void ubcore_unregister_client(struct ubcore_client *rm_client)
{
}

struct ubcore_jfs *ubcore_create_jfs(struct ubcore_device *dev, struct ubcore_jfs_cfg *cfg,
                                     ubcore_event_callback_t jfae_handler, struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_jfs));
}

int ubcore_unimport_seg(struct ubcore_target_seg *tseg)
{
    free(tseg);
    return 0;
}

struct ubcore_target_seg *ubcore_import_seg(struct ubcore_device *dev, struct ubcore_target_seg_cfg *cfg,
                                            struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_target_seg));
}

struct ubcore_tjetty *ubcore_import_jfr(struct ubcore_device *dev, struct ubcore_tjetty_cfg *cfg,
                                        struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_tjetty));
}

int ubcore_unimport_jfr(struct ubcore_tjetty *tjfr)
{
    free(tjfr);
    return 0;
}

struct ubcore_eid_info eids[2];
struct ubcore_eid_info *ubcore_get_eid_list(struct ubcore_device *dev, uint32_t *cnt)
{
    eids[1].eid.in4.addr = 0x1000;
    eids[1].eid.in4.prefix = 0;
    eids[1].eid.in4.reserved = 0;
    return eids;
}

void ubcore_free_eid_list(struct ubcore_eid_info *eid_list)
{
}

struct ubcore_jfr *ubcore_create_jfr(struct ubcore_device *dev, struct ubcore_jfr_cfg *cfg,
                                     ubcore_event_callback_t jfae_handler, struct ubcore_udata *udata)
{
    return malloc(sizeof(struct ubcore_jfr));
}

int ubcore_rearm_jfc(struct ubcore_jfc *jfc, bool solicited_only)
{
    return 0;
}

int ubcore_flush_jfs(struct ubcore_jfs *jfs, int cr_cnt, struct ubcore_cr *cr)
{
    return 0;
}

int ubcore_poll_jfc(struct ubcore_jfc *jfc, int cr_cnt, struct ubcore_cr *cr)
{
    return 0;
}

void rt_write_lock(rwlock_t *rwlock)
{
    ;
}

void rt_write_unlock(rwlock_t *rwlock)
{
    ;
}
int pfn_valid(unsigned long pfn)
{
    return 1;
}
struct page *pfn_to_online_page(unsigned long pfn)
{
    return (struct page *)1;
}

void complete(struct completion *x)
{
    ;
}
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size, int dir,
                              unsigned long attrs)
{
    return 0;
}
void __init_swait_queue_head(struct swait_queue_head *q, const char *name, struct lock_class_key *key)
{
    ;
}

void msleep(unsigned int msecs)
{
    ;
}

void dma_release_channel(struct dma_chan *chan)
{
    return;
}

struct dma_chan *__dma_request_channel(const dma_cap_mask_t *mask, dma_filter_fn fn, void *fn_param,
                                       struct device_node *np)
{
    return NULL;
}

struct dma_async_tx_descriptor *dmaengine_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dest, dma_addr_t src,
                                                          size_t len, unsigned long flags)
{
    return NULL;
}

int dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
    return -ENOMEM;
}

dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc)
{
    return desc->tx_submit(desc);
}

void dma_async_issue_pending(struct dma_chan *chan)
{
    ;
}

struct folio *pfn_folio(unsigned long pfn)
{
    return NULL;
}

int pfn_to_nid(unsigned long pfn)
{
    return 0;
}

int dmaengine_slave_config(struct dma_chan *chan, struct dma_slave_config *config)
{
    if (chan->device->device_config) {
        return chan->device->device_config(chan, config);
    }
    return -ENOSYS;
}
bool folio_try_get(struct folio *folio)
{
    return false;
}

int page_to_nid(const struct page *page)
{
    return 0;
}
bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
    return true;
}
void flush_workqueue(struct workqueue_struct *wq)
{
    return;
}
void destroy_workqueue(struct workqueue_struct *wq)
{
    return;
}
int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start, u64 end, void *arg,
                        int (*func)(struct resource *, void *))
{
    return 0;
}
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
    return true;
}

u64 page_size(const struct page *page)
{
    return 0;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    return true;
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active)
{
    return NULL;
}

int register_memory_notifier(struct notifier_block *nb)
{
    return 0;
}

void unregister_memory_notifier(struct notifier_block *nb)
{
    return;
}

bool cancel_work_sync(struct work_struct *work)
{
    return true;
}

bool numa_is_remote_node(int nid)
{
    return true;
}