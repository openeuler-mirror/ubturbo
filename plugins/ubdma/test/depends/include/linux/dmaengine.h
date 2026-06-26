// SPDX-License-Identifier: GPL-2.0-only
/*
* Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/
#ifndef LINUX_DMAENGINE_H
#define LINUX_DMAENGINE_H
#include <asm/memory.h>
#include <asm/page.h>
#include <linux/barrier.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#define BIT(nr) (1 << (nr))
typedef s32 dma_cookie_t;
#define DMA_MIN_COOKIE 1
enum dma_status {
    DMA_COMPLETE,
    DMA_IN_PROGRESS,
    DMA_PAUSED,
    DMA_ERROR,
    DMA_OUT_OF_ORDER,
};
enum dma_transaction_type {
    DMA_MEMCPY,
    DMA_XOR,
    DMA_PQ,
    DMA_XOR_VAL,
    DMA_PQ_VAL,
    DMA_MEMSET,
    DMA_MEMSET_SG,
    DMA_INTERRUPT,
    DMA_PRIVATE,
    DMA_ASYNC_TX,
    DMA_SLAVE,
    DMA_CYCLIC,
    DMA_INTERLEAVE,
    DMA_COMPLETION_NO_ORDER,
    DMA_REPEAT,
    DMA_LOAD_EOT,
    DMA_TX_TYPE_END,
};
enum dma_transfer_direction {
    DMA_MEM_TO_MEM,
    DMA_MEM_TO_DEV,
    DMA_DEV_TO_MEM,
    DMA_DEV_TO_DEV,
    DMA_TRANS_NONE,
};
typedef struct {
    DECLARE_BITMAP(bits, DMA_TX_TYPE_END);
} dma_cap_mask_t;
struct dma_slave_config {
    enum dma_transfer_direction direction;
    phys_addr_t src_addr;
    phys_addr_t dst_addr;
};
enum dma_ctrl_flags {
    DMA_PREP_INTERRUPT = (1 << 0),
    DMA_CTRL_ACK = (1 << 1),
    DMA_CTRL_REUSE = (1 << 6),
};
typedef void (*dma_async_tx_callback)(void *dma_async_param);
enum dmaengine_tx_result {
    DMA_TRANS_NOERROR = 0,
    DMA_TRANS_READ_FAILED,
    DMA_TRANS_WRITE_FAILED,
    DMA_TRANS_ABORTED,
};
struct dmaengine_result {
    enum dmaengine_tx_result result;
    u32 residue;
};

typedef void (*dma_async_tx_callback_result)(void *dma_async_param, const struct dmaengine_result *result);
struct dma_chan;
struct dma_chan_dev {
    struct dma_chan *chan;
    struct device device;
    int dev_id;
    bool chan_dma_dev;
};
struct dma_chan {
    struct dma_device *device;
    struct device *slave;
    dma_cookie_t cookie;
    dma_cookie_t completed_cookie;

    int chan_id;
    struct dma_chan_dev *dev;
    const char *name;
#ifdef CONFIG_DEBUG_FS
    char *dbg_client_name;
#endif
    struct list_head device_node;
    struct dma_chan_percpu __percpu *local;
    int client_count;
    int table_count;

    struct dma_router *router;
    void *route_data;
};
struct dma_async_tx_descriptor {
    dma_cookie_t cookie;
    enum dma_ctrl_flags flags;
    dma_addr_t phys;
    struct dma_chan *chan;
    dma_cookie_t (*tx_submit)(struct dma_async_tx_descriptor *tx);
    int (*desc_free)(struct dma_async_tx_descriptor *tx);
    dma_async_tx_callback callback;
    dma_async_tx_callback_result callback_result;
    void *callback_param;
    struct dmaengine_unmap_data *unmap;
    struct dma_descriptor_metadata_ops *metadata_ops;
};
struct dma_tx_state {
    dma_cookie_t last;
    dma_cookie_t used;
    u32 residue;
    u32 in_flight_bytes;
};

struct dma_device {
    struct list_head channels;
    struct device *dev;
    dma_cap_mask_t cap_mask;
    void (*device_free_chan_resources)(struct dma_chan *chan);
    void (*device_issue_pending)(struct dma_chan *chan);
    struct dma_async_tx_descriptor *(*device_prep_dma_memcpy)(struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
                                                              size_t len, unsigned long flags);
    enum dma_status (*device_tx_status)(struct dma_chan *chan, dma_cookie_t cookie, struct dma_tx_state *txstate);
    int (*device_config)(struct dma_chan *chan, struct dma_slave_config *config);
};
static inline enum dma_status dma_async_is_complete(dma_cookie_t cookie, dma_cookie_t last_complete,
                                                    dma_cookie_t last_used)
{
    if (last_complete <= last_used) {
        if ((cookie <= last_complete) || (cookie > last_used)) {
            return DMA_COMPLETE;
        }
    } else {
        if ((cookie <= last_complete) && (cookie > last_used)) {
            return DMA_COMPLETE;
        }
    }
    return DMA_IN_PROGRESS;
}
static inline void dma_async_tx_descriptor_init(struct dma_async_tx_descriptor *tx, struct dma_chan *chan)
{
    tx->chan = chan;
}
void dmaengine_desc_clear_reuse(struct dma_async_tx_descriptor *tx);
#define dma_cap_set(tx, mask) __dma_cap_set((tx), &(mask))
static inline void __dma_cap_set(enum dma_transaction_type tx_type, dma_cap_mask_t *dstp)
{
    set_bit(tx_type, dstp->bits);
}

static inline bool dmaengine_desc_test_reuse(struct dma_async_tx_descriptor *tx)
{
    return (tx->flags & DMA_CTRL_REUSE) == DMA_CTRL_REUSE;
}

void dma_async_device_unregister(struct dma_device *device);

static inline struct device *dmaengine_get_dma_device(struct dma_chan *chan)
{
    if (chan->dev->chan_dma_dev) {
        return &chan->dev->device;
    }
    return chan->device->dev;
}

static inline int dma_submit_error(dma_cookie_t cookie)
{
    return cookie < 0 ? cookie : 0;
}

static inline enum dma_status dma_wait_for_async_tx(struct dma_async_tx_descriptor *tx)
{
    return DMA_COMPLETE;
}

#define bitmap_size(nbits) (ALIGN(nbits, BITS_PER_LONG) / BITS_PER_BYTE)

#define dma_cap_zero(mask) __dma_cap_zero(&(mask))
static inline void __dma_cap_zero(dma_cap_mask_t *dstp)
{
}

typedef bool (*dma_filter_fn)(struct dma_chan *chan, void *filter_param);

#define dma_request_channel(mask, x, y) __dma_request_channel(&(mask), x, y, NULL)
#endif