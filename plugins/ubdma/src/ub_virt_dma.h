// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ub-dma virt dma
 */

#ifndef VIRT_DMA_H
#define VIRT_DMA_H

#include <linux/dmaengine.h>
#include <linux/interrupt.h>

#include "dmaengine.h"

struct ub_virt_dma_desc {
	struct dma_async_tx_descriptor tx;
	struct dmaengine_result tx_result;
	struct list_head node;
};

struct ub_virt_dma_chan {
	struct dma_chan chan;
	struct tasklet_struct task;
	void (*desc_free)(struct ub_virt_dma_desc *);

	spinlock_t lock;

	struct list_head desc_allocated;
	struct list_head desc_submitted;
	struct list_head desc_issued;
	struct list_head desc_completed;
	struct list_head desc_terminated;

	struct ub_virt_dma_desc *cyclic;
};

static inline struct ub_virt_dma_chan *to_ub_virt_chan(struct dma_chan *chan)
{
	return container_of(chan, struct ub_virt_dma_chan, chan);
}

void vchan_dma_desc_free_list(struct ub_virt_dma_chan *vc,
			      struct list_head *head);
void vchan_init(struct ub_virt_dma_chan *vc, struct dma_device *dmadev);
extern dma_cookie_t ub_dma_vchan_tx_submit(struct dma_async_tx_descriptor *);
extern int vchan_tx_desc_free(struct dma_async_tx_descriptor *tx);

static inline struct dma_async_tx_descriptor *
vchan_tx_prep(struct ub_virt_dma_chan *vc, struct ub_virt_dma_desc *vd,
	      unsigned long tx_flags)
{
	unsigned long flags;

	dma_async_tx_descriptor_init(&vd->tx, &vc->chan);
	vd->tx.flags = tx_flags;
	vd->tx.tx_submit = ub_dma_vchan_tx_submit;
	vd->tx.desc_free = vchan_tx_desc_free;

	vd->tx_result.result = DMA_TRANS_NOERROR;
	vd->tx_result.residue = 0;

	spin_lock_irqsave(&vc->lock, flags);
	list_add_tail(&vd->node, &vc->desc_allocated);
	spin_unlock_irqrestore(&vc->lock, flags);

	return &vd->tx;
}

static inline bool vchan_issue_pending(struct ub_virt_dma_chan *vc)
{
	list_splice_tail_init(&vc->desc_submitted, &vc->desc_issued);
	return !list_empty(&vc->desc_issued);
}

static inline void vchan_cookie_complete(struct ub_virt_dma_desc *vd)
{
	struct ub_virt_dma_chan *vc = to_ub_virt_chan(vd->tx.chan);
	dma_cookie_t cookie;

	cookie = vd->tx.cookie;
	dma_cookie_complete(&vd->tx);
	dev_vdbg(vc->chan.device->dev, "txd %p[%x]: marked complete\n", vd,
		 cookie);
	list_add_tail(&vd->node, &vc->desc_completed);

	tasklet_schedule(&vc->task);
}

static inline void vchan_vdesc_fini(struct ub_virt_dma_desc *vd)
{
	struct ub_virt_dma_chan *vc = to_ub_virt_chan(vd->tx.chan);

	if (dmaengine_desc_test_reuse(&vd->tx)) {
		unsigned long flags;

		spin_lock_irqsave(&vc->lock, flags);
		list_add(&vd->node, &vc->desc_allocated);
		spin_unlock_irqrestore(&vc->lock, flags);
	} else {
		vc->desc_free(vd);
	}
}

static inline struct ub_virt_dma_desc *
vchan_next_desc(struct ub_virt_dma_chan *vc)
{
	return list_first_entry_or_null(&vc->desc_issued,
					struct ub_virt_dma_desc, node);
}

static inline void vchan_get_all_descriptors(struct ub_virt_dma_chan *vc,
					     struct list_head *head)
{
	list_splice_tail_init(&vc->desc_allocated, head);
	list_splice_tail_init(&vc->desc_submitted, head);
	list_splice_tail_init(&vc->desc_issued, head);
	list_splice_tail_init(&vc->desc_completed, head);
	list_splice_tail_init(&vc->desc_terminated, head);
}

static inline void vchan_free_chan_resources(struct ub_virt_dma_chan *vc)
{
	struct ub_virt_dma_desc *vd;
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&vc->lock, flags);
	vchan_get_all_descriptors(vc, &head);
	list_for_each_entry(vd, &head, node) {
		dmaengine_desc_clear_reuse(&vd->tx);
	}
	spin_unlock_irqrestore(&vc->lock, flags);

	vchan_dma_desc_free_list(vc, &head);
}

#endif