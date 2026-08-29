/*
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
#include <linux/io.h>

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
extern "C" int ub_hist_init(void);
extern "C" void ub_hist_exit(void);
extern "C" int ub_hist_query_ba_count(void);
extern "C" int ub_hist_query_ba_tags(uint64_t *p_tags, int count);
extern "C" int ub_hist_set_state(struct ub_hist_ba_config *config, uint64_t ba_tag);
extern "C" int ub_hist_get_state(struct ub_hist_ba_config *config, uint64_t ba_tag);
extern "C" int ub_hist_get_statistic_result(struct ub_hist_ba_result *result);
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

extern "C" int ub_hist_query_ba_info(uint64_t ba_tag, struct ub_hist_ba_info *ba_info);

TEST_F(DriversUbHistMidTest, QueryCountAndTags)
{
    uint64_t tags[DEFAULT_NUMA_NODE] = {};

    EXPECT_EQ(0, ub_hist_query_ba_count());
    EXPECT_EQ(-EINVAL, ub_hist_query_ba_tags(tags, DEFAULT_NUMA_NODE));
}

TEST_F(DriversUbHistMidTest, ValidateStateApisInvalidArgs)
{
    struct ub_hist_ba_config config = {};
    struct ub_hist_ba_result result = {};

    EXPECT_EQ(-EINVAL, ub_hist_set_state(nullptr, REG_BASE_CPP_ADDR_0));
    EXPECT_EQ(-ENODEV, ub_hist_set_state(&config, 0xdeadbeef));

    EXPECT_EQ(-EINVAL, ub_hist_get_state(nullptr, REG_BASE_CPP_ADDR_0));
    EXPECT_EQ(-ENODEV, ub_hist_get_state(&config, 0xdeadbeef));

    EXPECT_EQ(-EINVAL, ub_hist_get_statistic_result(nullptr));
    result.ba_tag = 0xdeadbeef;
    EXPECT_EQ(-ENODEV, ub_hist_get_statistic_result(&result));
}

extern "C" int ub_hist_set_hw_type(void);
TEST_F(DriversUbHistMidTest, UbHistInitSetHwTypeFail)
{
    // This test would require mocking platform_driver_register which is complex
    // Skipping detailed implementation for now
}

extern "C" int ub_hist_ba_init(struct ub_hist_ba_device *ba_dev);
extern "C" int ub_hist_rd_clr_sts(struct ub_hist_ba_device *ba_dev, u32 *buf, size_t len);
extern "C" int ub_hist_get_ba_resource(struct platform_device *pdev, struct ub_hist_ba_device *ba_dev);
TEST_F(DriversUbHistMidTest, UbHistProbeBaInitFail)
{
    struct platform_device test_pdev;
    struct ub_hist_ba_device ba_dev;

    memset(&test_pdev, 0, sizeof(test_pdev));
    memset(&ba_dev, 0, sizeof(ba_dev));

    MOCKER(devm_kzalloc).stubs().will(returnValue(&ba_dev));
    MOCKER(ub_hist_get_ba_resource).stubs().will(returnValue(0));
    MOCKER(ub_hist_ba_init).stubs().will(returnValue(-ENOMEM));

    int ret = ub_hist_probe(&test_pdev);
    EXPECT_EQ(-ENOMEM, ret);
}

TEST_F(DriversUbHistMidTest, UbHistProbeRdClrStsFail)
{
    struct platform_device test_pdev;
    struct ub_hist_ba_device ba_dev;

    memset(&test_pdev, 0, sizeof(test_pdev));
    memset(&ba_dev, 0, sizeof(ba_dev));
    ba_dev.base_addr = (void __iomem *)0x1000;

    MOCKER(devm_kzalloc).stubs().will(returnValue(&ba_dev));
    MOCKER(ub_hist_get_ba_resource).stubs().will(returnValue(0));
    MOCKER(ub_hist_ba_init).stubs().will(returnValue(0));
    MOCKER(ub_hist_rd_clr_sts).stubs().will(returnValue(-EIO));

    int ret = ub_hist_probe(&test_pdev);
    EXPECT_EQ(-EIO, ret);
}

#undef readl
#undef writel
extern "C" u32 readl(const volatile void __iomem *addr);
extern "C" void writel(u32 value, const volatile void __iomem *addr);
extern "C" u32 ub_hist_read_reg(struct ub_hist_ba_device *ba_dev, u32 reg_offset);
extern "C" void ub_hist_write_reg(struct ub_hist_ba_device *ba_dev, u32 reg_offset, u32 value);

extern "C" int ub_hist_offset_init(struct ub_hist_ba_device *ba_dev, u32 reg_offset, u32 init_value);
TEST_F(DriversUbHistMidTest, UbHistOffsetInitSuccess)
{
    struct ub_hist_ba_device ba_dev;
    ba_dev.base_addr = (void __iomem *)0x1000;
    MOCKER(ub_hist_read_reg).stubs().will(returnValue(0x100));
    int ret = ub_hist_offset_init(&ba_dev, 0x10, 0x100);
    EXPECT_EQ(0, ret);
}

TEST_F(DriversUbHistMidTest, UbHistOffsetInitMismatch)
{
    struct ub_hist_ba_device ba_dev;
    ba_dev.base_addr = (void __iomem *)0x1000;
    MOCKER(ub_hist_read_reg).stubs().will(returnValue(0x200));
    int ret = ub_hist_offset_init(&ba_dev, 0x10, 0x100);
    EXPECT_EQ(-EBUSY, ret);
}

extern "C" int ub_hist_ba_init(struct ub_hist_ba_device *ba_dev);
TEST_F(DriversUbHistMidTest, UbHistBaInitRamNotReady)
{
    struct ub_hist_ba_device ba_dev;
    ba_dev.base_addr = (void __iomem *)0x1000;
    MOCKER(ub_hist_read_reg).stubs().will(returnValue(0));
    int ret = ub_hist_ba_init(&ba_dev);
    EXPECT_EQ(-EBUSY, ret);
}

extern "C" int ub_hist_rd_clr_sts(struct ub_hist_ba_device *ba_dev, u32 *buf, size_t len);
TEST_F(DriversUbHistMidTest, UbHistRdClrStsStsEnabled)
{
    struct ub_hist_ba_device ba_dev;
    ba_dev.base_addr = (void __iomem *)0x1000;
    u32 buf[4];
    /* sts_enable is at bit 4 in hi_upa_smap_cfg_smap_cfg00 union;
       0x10 sets sts_enable=1 to trigger -EBUSY path */
    MOCKER(ub_hist_read_reg).stubs().will(returnValue(0x10));
    int ret = ub_hist_rd_clr_sts(&ba_dev, buf, 4);
    EXPECT_EQ(-EBUSY, ret);
}

extern "C" int ub_hist_get_ba_resource(struct platform_device *pdev, struct ub_hist_ba_device *ba_dev);
TEST_F(DriversUbHistMidTest, UbHistGetBaResourcePropertyReadFail)
{
    struct platform_device test_pdev;
    struct ub_hist_ba_device ba_dev;
    memset(&test_pdev, 0, sizeof(test_pdev));
    memset(&ba_dev, 0, sizeof(ba_dev));
    MOCKER(device_property_read_u64).stubs().will(returnValue(-EINVAL));
    int ret = ub_hist_get_ba_resource(&test_pdev, &ba_dev);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" void ub_hist_mar_perf_en(uint32_t perf_prd_ms);
extern "C" bool ub_hist_mar_perf_check(void);
extern "C" void ub_hist_get_access_count(uint32_t *flux_wr, uint32_t *flux_rd);
extern "C" void __iomem *ub_hist_mar_cfg_base_addr[MAR_CFG_REG_CNT];
extern "C" uint32_t ub_hist_mar_perf_expected_val;

TEST_F(DriversUbHistMidTest, MarPerfEnSetsExpectedVal)
{
    /* Setup valid base addrs (writel/readl are macros that operate on the address) */
    uint32_t perf_bufs[MAR_CFG_REG_CNT] = {0};
    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        ub_hist_mar_cfg_base_addr[i] = (void __iomem *)&perf_bufs[i];
    }

    ub_hist_mar_perf_en(200);
    /* Verify expected val is set and contains the perf_prd bits */
    uint32_t expected_prd = (200 * 1000000 >> 4);
    EXPECT_NE(0u, ub_hist_mar_perf_expected_val);
    EXPECT_EQ(expected_prd, ub_hist_mar_perf_expected_val & 0x0FFFFFFF);

    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        ub_hist_mar_cfg_base_addr[i] = nullptr;
    }
}

TEST_F(DriversUbHistMidTest, MarPerfCheckNoBaseAddr)
{
    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        ub_hist_mar_cfg_base_addr[i] = nullptr;
    }
    ub_hist_mar_perf_expected_val = 0x1234;
    bool ret = ub_hist_mar_perf_check();
    EXPECT_EQ(false, ret);
}

TEST_F(DriversUbHistMidTest, GetAccessCountNullParams)
{
    ub_hist_get_access_count(nullptr, nullptr);
}

TEST_F(DriversUbHistMidTest, GetAccessCountNormalRead)
{
    uint32_t flux_wr[MAR_CFG_REG_CNT] = {0};
    uint32_t flux_rd[MAR_CFG_REG_CNT] = {0};

    /* readl is a macro returning the address itself; use stack buffers as base */
    uint32_t wr_buf[MAR_CFG_REG_CNT] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint32_t rd_buf[MAR_CFG_REG_CNT] = {11, 21, 31, 41, 51, 61, 71, 81};
    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        ub_hist_mar_cfg_base_addr[i] = (void __iomem *)&wr_buf[i];
    }

    ub_hist_get_access_count(flux_wr, flux_rd);
    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        EXPECT_NE(0, flux_wr[i]);
    }

    for (int i = 0; i < MAR_CFG_REG_CNT; i++) {
        ub_hist_mar_cfg_base_addr[i] = nullptr;
    }
}
