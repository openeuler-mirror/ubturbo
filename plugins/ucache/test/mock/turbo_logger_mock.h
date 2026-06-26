/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 */
#ifndef TURBO_LOGGER_H
#define TURBO_LOGGER_H
#include <iostream>

namespace turbo::log {
#define UBTURBO_LOG_ERROR(...) std::cout
#define UBTURBO_LOG_DEBUG(...) std::cout
#define UBTURBO_LOG_INFO(...) std::cout
#define UBTURBO_LOG_WARN(...) std::cout
} // namespace turbo::log
#endif /* TURBO_LOGGER_H */