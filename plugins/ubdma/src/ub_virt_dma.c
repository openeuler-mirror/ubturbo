// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ub-dma virt dma
 */

#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "ub_virt_dma.h"

static struct ub_virt_dma_desc *
to_ub_virt_desc(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct ub_virt_dma_desc, tx);
}

dma_cookie_t ub_dma_vchan_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct ub_virt_dma_chan *uvc = to_ub_virt_chan(tx->chan);
	struct ub_virt_dma_desc *uvd = to_ub_virt_desc(tx);
	unsigned long flag;
	dma_cookie_t cookie;

	spin_lock_irqsave(&uvc->lock, flag);
	cookie = ub_dma_cookie_assign(tx);

	list_move_tail(&uvd->node, &uvc->desc_submitted);
	spin_unlock_irqrestore(&uvc->lock, flag);

	return cookie;
}

int vchan_tx_desc_free(struct dma_async_tx_descriptor *tx)
{
	struct ub_virt_dma_chan *uvc = to_ub_virt_chan(tx->chan);
	struct ub_virt_dma_desc *uvd = to_ub_virt_desc(tx);
	unsigned long flags;

	spin_lock_irqsave(&uvc->lock, flags);
	list_del(&uvd->node);
	spin_unlock_irqrestore(&uvc->lock, flags);

	uvc->desc_free(uvd);
	return 0;
}

static void ub_vchan_complete(struct tasklet_struct *t)
{
	struct ub_virt_dma_chan *uvc = from_tasklet(uvc, t, task);
	struct ub_virt_dma_desc *uvd;
	struct ub_virt_dma_desc *tmp_uvd;
	struct dmaengine_desc_callback cb;
	LIST_HEAD(head);

	spin_lock_irq(&uvc->lock);
	list_splice_tail_init(&uvc->desc_completed, &head);
	uvd = uvc->cyclic;
	if (uvd) {
		uvc->cyclic = NULL;
		dmaengine_desc_get_callback(&uvd->tx, &cb);
	} else {
		memset(&cb, 0, sizeof(cb));
	}
	spin_unlock_irq(&uvc->lock);

	dmaengine_desc_callback_invoke(&cb, &uvd->tx_result);

	list_for_each_entry_safe(uvd, tmp_uvd, &head, node) {
		dmaengine_desc_get_callback(&uvd->tx, &cb);

		list_del(&uvd->node);
		dmaengine_desc_callback_invoke(&cb, &uvd->tx_result);
		vchan_vdesc_fini(uvd);
	}
}

void vchan_dma_desc_free_list(struct ub_virt_dma_chan *vc,
			      struct list_head *head)
{
	struct ub_virt_dma_desc *vd;
	struct ub_virt_dma_desc *tmp_vd;

	list_for_each_entry_safe(vd, tmp_vd, head, node) {
		list_del(&vd->node);
		vchan_vdesc_fini(vd);
	}
}

void vchan_init(struct ub_virt_dma_chan *vc, struct dma_device *dmadev)
{
	dma_cookie_init(&vc->chan);

	spin_lock_init(&vc->lock);
	INIT_LIST_HEAD(&vc->desc_allocated);
	INIT_LIST_HEAD(&vc->desc_submitted);
	INIT_LIST_HEAD(&vc->desc_issued);
	INIT_LIST_HEAD(&vc->desc_completed);
	INIT_LIST_HEAD(&vc->desc_terminated);

	tasklet_setup(&vc->task, ub_vchan_complete);

	vc->chan.device = dmadev;
	list_add_tail(&vc->chan.device_node, &dmadev->channels);
}