// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ub-dma dmaengine
 */
#ifndef DMAENGINE_H
#define DMAENGINE_H

#include <linux/bug.h>
#include <linux/dmaengine.h>

static inline void dma_cookie_init(struct dma_chan *chan)
{
	chan->cookie = DMA_MIN_COOKIE;
	chan->completed_cookie = DMA_MIN_COOKIE;
}

static inline dma_cookie_t
ub_dma_cookie_assign(struct dma_async_tx_descriptor *tx)
{
	struct dma_chan *dchan = tx->chan;
	dma_cookie_t cookie;

	cookie = dchan->cookie + 1;
	if (cookie < DMA_MIN_COOKIE) {
		cookie = DMA_MIN_COOKIE;
	}
	tx->cookie = dchan->cookie = cookie;

	return cookie;
}

static inline void dma_cookie_complete(struct dma_async_tx_descriptor *tx)
{
	BUG_ON(tx->cookie < DMA_MIN_COOKIE);
	tx->chan->completed_cookie = tx->cookie;
	tx->cookie = 0;
}

static inline enum dma_status dma_cookie_status(struct dma_chan *chan,
						dma_cookie_t cookie,
						struct dma_tx_state *state)
{
	dma_cookie_t used = chan->cookie;
	dma_cookie_t complete = chan->completed_cookie;
	barrier();
	if (state) {
		state->last = complete;
		state->used = used;
		state->residue = 0;
		state->in_flight_bytes = 0;
	}
	return dma_async_is_complete(cookie, complete, used);
}

struct dmaengine_desc_callback {
	dma_async_tx_callback callback;
	dma_async_tx_callback_result callback_result;
	void *callback_param;
};

static inline void
dmaengine_desc_get_callback(struct dma_async_tx_descriptor *tx,
			    struct dmaengine_desc_callback *cb)
{
	cb->callback = tx->callback;
	cb->callback_result = tx->callback_result;
	cb->callback_param = tx->callback_param;
}

static inline void
dmaengine_desc_callback_invoke(struct dmaengine_desc_callback *cb,
			       const struct dmaengine_result *result)
{
	struct dmaengine_result dummy_result = { .result = DMA_TRANS_NOERROR,
						 .residue = 0 };

	if (cb->callback_result) {
		if (!result) {
			result = &dummy_result;
		}
		cb->callback_result(cb->callback_param, result);
	} else if (cb->callback) {
		cb->callback(cb->callback_param);
	}
}

#endif