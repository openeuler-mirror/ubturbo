// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: ub-dma log
 */

#ifndef UB_DMA_LOG_H
#define UB_DMA_LOG_H

#include <linux/types.h>
#include <linux/printk.h>

enum ub_dma_log_level {
	UB_DMA_LOG_LEVEL_EMERG = 0,
	UB_DMA_LOG_LEVEL_ALERT = 1,
	UB_DMA_LOG_LEVEL_CRIT = 2,
	UB_DMA_LOG_LEVEL_ERR = 3,
	UB_DMA_LOG_LEVEL_WARNING = 4,
	UB_DMA_LOG_LEVEL_NOTICE = 5,
	UB_DMA_LOG_LEVEL_INFO = 6,
	UB_DMA_LOG_LEVEL_DEBUG = 7,
	UB_DMA_LOG_LEVEL_MAX = 8
};

#define UB_DMA_LOG_TAG "LogTag_UB_DMA"
#define ub_dma_log(l, format, args...) \
	pr_##l("%s|%s:[%d]|" format, UB_DMA_LOG_TAG, __func__, __LINE__, ##args)

#define UB_DMA_RATELIMIT_INTERVAL (5 * HZ)
#define UB_DMA_RATELIMIT_BURST 100

extern uint32_t g_ub_dma_log_level;

#define ub_dma_log_info(...)                                     \
	do {                                                     \
		if (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_INFO) \
			ub_dma_log(info, __VA_ARGS__);           \
	} while (0)

#define ub_dma_log_err(...)                                     \
	do {                                                    \
		if (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_ERR) \
			ub_dma_log(err, __VA_ARGS__);           \
	} while (0)

#define ub_dma_log_warn(...)                                        \
	do {                                                        \
		if (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_WARNING) \
			ub_dma_log(warn, __VA_ARGS__);              \
	} while (0)

#define ub_dma_log_debug(...)                                     \
	do {                                                      \
		if (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_DEBUG) \
			ub_dma_log(debug, __VA_ARGS__);           \
	} while (0)

#define ubks_log_info_rl(...)                                                 \
	do {                                                                  \
		static DEFINE_RATELIMIT_STATE(_rs, UB_DMA_RATELIMIT_INTERVAL, \
					      UB_DMA_RATELIMIT_BURST);        \
		if ((__ratelimit(&_rs)) &&                                    \
		    (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_INFO))            \
			ub_dma_log(info, __VA_ARGS__);                        \
	} while (0)

#define ubks_log_err_rl(...)                                                  \
	do {                                                                  \
		static DEFINE_RATELIMIT_STATE(_rs, UB_DMA_RATELIMIT_INTERVAL, \
					      UB_DMA_RATELIMIT_BURST);        \
		if ((__ratelimit(&_rs)) &&                                    \
		    (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_ERR))             \
			ub_dma_log(err, __VA_ARGS__);                         \
	} while (0)

#define ubks_log_warn_rl(...)                                                 \
	do {                                                                  \
		static DEFINE_RATELIMIT_STATE(_rs, UB_DMA_RATELIMIT_INTERVAL, \
					      UB_DMA_RATELIMIT_BURST);        \
		if ((__ratelimit(&_rs)) &&                                    \
		    (g_ub_dma_log_level >= UB_DMA_LOG_LEVEL_WARNING))         \
			ub_dma_log(warn, __VA_ARGS__);                        \
	} while (0)

#endif