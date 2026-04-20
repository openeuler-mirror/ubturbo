/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP : tests for smap_hist_mid
 * Create: 2025-7-2
 */
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/types.h>
#include <linux/acpi.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/platform_device.h>

#include "ub_hist.h"

using namespace std;

constexpr int DEFAULT_NUMA_NODE = 4;
constexpr uint64_t REG_BASE_CPP_ADDR_0 = 0x33030a0000;
constexpr uint64_t REG_BASE_CPP_ADDR_1 = 0x330a0a0000;
constexpr uint64_t REG_BASE_CPP_ADDR_2 = 0x73030a0000;
constexpr uint64_t REG_BASE_CPP_ADDR_3 = 0x730a0a0000;

struct ub_hist_ba_device {
    struct device *dev;
    void __iomem *base_addr;
    struct list_head list;
    struct ub_hist_ba_info info;
};

struct platform_device *pdev[DEFAULT_NUMA_NODE];

static uint64_t reg_base_cpp_addrs[] = {
    REG_BASE_CPP_ADDR_0,
    REG_BASE_CPP_ADDR_1,
    REG_BASE_CPP_ADDR_2,
    REG_BASE_CPP_ADDR_3
};

extern "C" int ub_hist_ba_init(struct ub_hist_ba_device *ba_dev);
extern "C" int ub_hist_probe(struct platform_device *pdev);
extern "C" int ub_hist_remove(struct platform_device *pdev);
class DriversUbHistMidTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        GlobalMockObject::reset();
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        for (int i = 0; i < DEFAULT_NUMA_NODE; i++) {
            ub_hist_remove(pdev[i]);
            if (pdev[i]) {
                free(pdev[i]);
                pdev[i] = nullptr;
            }
        }
        ub_hist_exit();
        cout << "[Phase TearDown End]" << endl;
    }
};