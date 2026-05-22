/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 */

#ifndef RACK_MANAGER_TEST_RACK_LOGGER_RINGBUFFER_H
#define RACK_MANAGER_TEST_RACK_LOGGER_RINGBUFFER_H

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#define private public
#include "rack_logger_ringbuffer.h"
#include "turbo_logger.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

namespace turbo::log {

class RackLoggerRingbufferTest : public testing::Test {
public:
    RackLoggerRingbufferTest();
    void SetUp() override;
    void TearDown() override;
};
} // namespace turbo::log
#endif // RACK_MANAGER_TEST_RACK_LOGGER_RINGBUFFER_H
