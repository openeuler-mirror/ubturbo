// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: SMAP : ub_hist
 */

#include <asm/io.h>
#include <linux/acpi.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/minmax.h>
#include <linux/uaccess.h>
#include "ub_hist.h"

#define UDIE_PHY_ADDR_STS_REGS_N6_OFFSET 0x4000
#define UDIE_PHY_ADDR_STS_REGS_N7_OFFSET 0x1000
#define DEVICE_MEM_LEN_N6 0x8000
#define DEVICE_MEM_LEN_N7 0x11000

struct ub_hist_ba_device {
	struct device *dev;
	void __iomem *base_addr;
	struct list_head list;
	struct ub_hist_ba_info info;
};

static LIST_HEAD(ub_hist_ba_list);
static DEFINE_SPINLOCK(ub_hist_ba_list_lock);
static DEFINE_MUTEX(dev_sta_mutex);
static ub_hist_smap_type g_ub_hist_smap_type = UB_HIST_SMAP_TYPE_N6;

static struct ub_hist_ba_device *ub_hist_find_dev_by_tag(uint64_t ba_tag)
{
	unsigned long flags;
	struct ub_hist_ba_device *ba_dev;

	spin_lock_irqsave(&ub_hist_ba_list_lock, flags);
	list_for_each_entry(ba_dev, &ub_hist_ba_list, list) {
		if (ba_dev->info.ba_tag == ba_tag) {
			spin_unlock_irqrestore(&ub_hist_ba_list_lock, flags);
			return ba_dev;
		}
	}
	pr_err("failed to find BA device by BA tag\n");
	ba_dev = NULL;
	spin_unlock_irqrestore(&ub_hist_ba_list_lock, flags);
	return ba_dev;
}

static inline u32 ub_hist_read_reg(struct ub_hist_ba_device *ba_dev, u32 off)
{
	void __iomem *reg = ba_dev->base_addr + off;
	u32 out = readl(reg);

	return le32_to_cpu(*(__le32 *)&out);
}

static inline void ub_hist_write_reg(struct ub_hist_ba_device *ba_dev, u32 off,
				     u32 val)
{
	void __iomem *reg = ba_dev->base_addr + off;
	__le32 val_le = cpu_to_le32(val);

	writel(*(u32 *)&val_le, reg);
}

int ub_hist_offset_init(struct ub_hist_ba_device *ba_dev, u32 reg_offset,
			u32 reg_init)
{
	ub_hist_write_reg(ba_dev, reg_offset, reg_init);
	u32 val = ub_hist_read_reg(ba_dev, reg_offset);
	if (val != reg_init) {
		dev_err(ba_dev->dev,
			"hist ba ctrl reg mismatch(read_value(%#x):expect_value(%#x))\n",
			val, reg_init);
		return -EBUSY;
	}
	pr_info("[ub_hist]reg_offset:%#x, reg_init:%#x\n", reg_offset,
		reg_init);
	return 0;
}

static int ub_hist_ba_init(struct ub_hist_ba_device *ba_dev)
{
	union hi_upa_smap_cfg_smap_cfg00 smap_cfg00;
	size_t i;
	struct {
		unsigned int offset;
		unsigned int init_value;
	} reg_info[] = {
		{ BA_CTRL_REG_OFFSET, BA_CTRL_REG_INIT },
		{ BA_ECC_REG_OFFSET, BA_ECC_REG_INIT },
		{ BA_THRESH_REG_OFFSET, BA_THRESH_REG_INIT },
		{ BA_SMAP_TH1_OFFSET, BA_SMAP_TH1_INIT },
		{ BA_SMAP_TH2_OFFSET, BA_SMAP_TH2_INIT },
		{ BA_SMAP_TH3_OFFSET, BA_SMAP_TH3_INIT },
		{ BA_SMAP_TH4_OFFSET, BA_SMAP_TH4_INIT },
		{ BA_SMAP_CNT0_OFFSET, BA_SMAP_CNT0_INIT },
		{ BA_SMAP_CNT1_OFFSET, BA_SMAP_CNT1_INIT },
		{ BA_SMAP_CNT_CLR0_OFFSET, BA_SMAP_CNT_CLR0_INIT },
		{ BA_SMAP_CNT_CLR1_OFFSET, BA_SMAP_CNT_CLR1_INIT },
		{ BA_SMAP_CNT_CLR2_OFFSET, BA_SMAP_CNT_CLR2_INIT },
		{ BA_SMAP_EN_OFFSET, BA_SMAP_EN_INIT },
		{ BA_SMAP_INT_MASK_OFFSET, BA_SMAP_INT_MASK_INIT },
		{ BA_SMAP_CNT_INT_CLR0_OFFSET, BA_SMAP_CNT_INT_CLR0_INIT },
		{ BA_SMAP_CNT_INT_CLR1_OFFSET, BA_SMAP_CNT_INT_CLR1_INIT },
		{ BA_SMAP_CNT_INT_CLR2_OFFSET, BA_SMAP_CNT_INT_CLR2_INIT },
		{ BA_SMAP_RPT0_OFFSET, BA_SMAP_RPT0_INIT },
		{ BA_SMAP_RPT1_OFFSET, BA_SMAP_RPT1_INIT },
	};

	smap_cfg00.val = ub_hist_read_reg(ba_dev, BA_CTRL_REG_OFFSET);
	if (!smap_cfg00.ram_init_done) {
		dev_err(ba_dev->dev, "hist ba init state error\n");
		return -EBUSY;
	}

	for (i = 0; i < sizeof(reg_info) / sizeof(reg_info[0]); i++) {
		ub_hist_offset_init(ba_dev, reg_info[i].offset,
				    reg_info[i].init_value);
	}

	return 0;
}

static inline u32 ub_hist_get_smap_sts_value_reg_addr(void)
{
	return (g_ub_hist_smap_type == UB_HIST_SMAP_TYPE_N7) ?
		       UDIE_PHY_ADDR_STS_REGS_N7_OFFSET :
		       UDIE_PHY_ADDR_STS_REGS_N6_OFFSET;
}

static inline u32 ub_hist_get_ba_sts_value_count(void)
{
	return (g_ub_hist_smap_type == UB_HIST_SMAP_TYPE_N7) ?
		       BA_STS_VALUE_N7_COUNT :
		       BA_STS_VALUE_N6_COUNT;
}

int ub_hist_rd_clr_sts(struct ub_hist_ba_device *ba_dev, u32 *buf, size_t len)
{
	int i;
	union hi_upa_smap_cfg_smap_cfg00 smap_cfg00;
	u32 smap_sts_value_reg_addr = ub_hist_get_smap_sts_value_reg_addr();
	u32 ba_sts_value_count = ub_hist_get_ba_sts_value_count();

	smap_cfg00.val = ub_hist_read_reg(ba_dev, BA_CTRL_REG_OFFSET);
	if (smap_cfg00.sts_enable) {
		pr_err("[UB_HIST] REGs cannot be read when statistics are enabled!\n");
		return -EBUSY;
	}
	if (buf) {
		for (i = 0; i < BA_STS_WORD_COUNT(ba_sts_value_count); i++) {
			buf[i] = ub_hist_read_reg(ba_dev,
						  smap_sts_value_reg_addr +
							  i * sizeof(u32));
		}
	} else {
		for (i = 0; i < BA_STS_WORD_COUNT(ba_sts_value_count); i++) {
			ub_hist_read_reg(ba_dev, smap_sts_value_reg_addr +
							 i * sizeof(u32));
		}
	}
	return 0;
}

int ub_hist_query_ba_count(void)
{
	int ba_count = 0;
	struct ub_hist_ba_device *ba_dev;

	spin_lock(&ub_hist_ba_list_lock);
	list_for_each_entry(ba_dev, &ub_hist_ba_list, list) {
		ba_count++;
	}
	spin_unlock(&ub_hist_ba_list_lock);
	return ba_count;
}
EXPORT_SYMBOL(ub_hist_query_ba_count);

int ub_hist_query_ba_tags(uint64_t *p_tags, int count)
{
	int ret = 0, ba_count = 0;
	struct ub_hist_ba_device *ba_dev;

	spin_lock(&ub_hist_ba_list_lock);
	list_for_each_entry(ba_dev, &ub_hist_ba_list, list) {
		if (ba_count >= count) {
			ret = -EINVAL;
			spin_unlock(&ub_hist_ba_list_lock);
			return ret;
		}
		p_tags[ba_count] = ba_dev->info.ba_tag;
		ba_count++;
	}

	if (ba_count < count)
		ret = -EINVAL;

	spin_unlock(&ub_hist_ba_list_lock);
	return ret;
}
EXPORT_SYMBOL(ub_hist_query_ba_tags);

int ub_hist_query_ba_info(uint64_t ba_tag, struct ub_hist_ba_info *ba_info)
{
	struct ub_hist_ba_device *ba_dev;

	if (!ba_info) {
		pr_err("invalid BA info passed to query BA info\n");
		return -EINVAL;
	}

	ba_dev = ub_hist_find_dev_by_tag(ba_tag);
	if (!ba_dev)
		return -ENODEV;

	memcpy(ba_info, &ba_dev->info, sizeof(struct ub_hist_ba_info));

	return 0;
}
EXPORT_SYMBOL(ub_hist_query_ba_info);

int ub_hist_set_state(struct ub_hist_ba_config *config, uint64_t ba_tag)
{
	struct ub_hist_ba_device *ba_dev;

	if (!config) {
		pr_err("invalid config passed to histogram state set\n");
		return -EINVAL;
	}

	ba_dev = ub_hist_find_dev_by_tag(ba_tag);
	if (!ba_dev)
		return -ENODEV;

	ub_hist_write_reg(ba_dev, config->reg_offset, config->reg_value);

	return 0;
}
EXPORT_SYMBOL(ub_hist_set_state);

int ub_hist_get_state(struct ub_hist_ba_config *config, uint64_t ba_tag)
{
	struct ub_hist_ba_device *ba_dev;

	if (!config) {
		pr_err("invalid config passed to histogram state get\n");
		return -EINVAL;
	}

	ba_dev = ub_hist_find_dev_by_tag(ba_tag);
	if (!ba_dev)
		return -ENODEV;

	config->reg_value = ub_hist_read_reg(ba_dev, config->reg_offset);

	return 0;
}
EXPORT_SYMBOL(ub_hist_get_state);

int ub_hist_get_statistic_result(struct ub_hist_ba_result *result)
{
	int ret;
	struct ub_hist_ba_device *ba_dev;
	u32 ba_sts_value_count = ub_hist_get_ba_sts_value_count();

	if (!result) {
		pr_err("invalid buffer passed to get histogram statistics result\n");
		return -EINVAL;
	}

	ba_dev = ub_hist_find_dev_by_tag(result->ba_tag);
	if (!ba_dev)
		return -ENODEV;

	ret = ub_hist_rd_clr_sts(ba_dev, (u32 *)result->buffer,
				 BA_STS_WORD_COUNT(ba_sts_value_count));
	return ret;
}
EXPORT_SYMBOL(ub_hist_get_statistic_result);

static int ub_hist_get_ba_resource(struct platform_device *pdev,
				   struct ub_hist_ba_device *ba_dev)
{
	int ret;
	uint64_t value;
	uint64_t device_mem_len =
		(g_ub_hist_smap_type == UB_HIST_SMAP_TYPE_N7) ?
			DEVICE_MEM_LEN_N7 :
			DEVICE_MEM_LEN_N6;

	if (!ACPI_COMPANION(&pdev->dev)) {
		pr_err("ACPI companion not found\n");
		return -ENODEV;
	}

	ret = device_property_read_u64(&pdev->dev, "ba_reg_base_addr", &value);
	if (ret) {
		pr_err("failed to read register base address, ret: %d\n", ret);
		return ret;
	}

	ba_dev->info.ba_tag = value;
	ba_dev->base_addr = ioremap(value, device_mem_len);
	if (!ba_dev->base_addr) {
		pr_err("failed to remap register base address\n");
		return -ENOMEM;
	}

	ret = device_property_read_u64(&pdev->dev, "cc_mem_base_addr", &value);
	if (ret) {
		pr_err("failed to read CC memory base address, ret: %d\n", ret);
		iounmap(ba_dev->base_addr);
		return ret;
	}
	ba_dev->info.cc_range.start = value;

	ret = device_property_read_u64(&pdev->dev, "cc_mem_size", &value);
	if (ret) {
		pr_err("failed to read CC memory size, ret: %d\n", ret);
		iounmap(ba_dev->base_addr);
		return ret;
	}
	ba_dev->info.cc_range.end = ba_dev->info.cc_range.start + value;

	ret = device_property_read_u64(&pdev->dev, "nc_mem_base_addr", &value);
	if (ret) {
		pr_err("failed to read NC memory base address, ret: %d\n", ret);
		iounmap(ba_dev->base_addr);
		return ret;
	}
	ba_dev->info.nc_range.start = value;

	ret = device_property_read_u64(&pdev->dev, "nc_mem_size", &value);
	if (ret) {
		pr_err("failed to read NC memory size, ret: %d\n", ret);
		iounmap(ba_dev->base_addr);
		return ret;
	}
	ba_dev->info.nc_range.end = ba_dev->info.nc_range.start + value;

	return 0;
}

static int ub_hist_probe(struct platform_device *pdev)
{
	int ret;
	unsigned long flags;
	struct ub_hist_ba_device *ba_dev;
	struct resource *r;

	ba_dev = devm_kzalloc(&pdev->dev, sizeof(*ba_dev), GFP_KERNEL);
	if (!ba_dev)
		return -ENOMEM;

	ba_dev->dev = &pdev->dev;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ret = ub_hist_get_ba_resource(pdev, ba_dev);
	if (ret)
		return ret;

	ret = ub_hist_ba_init(ba_dev);
	if (ret)
		return ret;

	ret = ub_hist_rd_clr_sts(ba_dev, NULL, 0);
	if (ret)
		return ret;

	spin_lock_irqsave(&ub_hist_ba_list_lock, flags);
	list_add_tail(&ba_dev->list, &ub_hist_ba_list);
	spin_unlock_irqrestore(&ub_hist_ba_list_lock, flags);
	pr_debug("ub_hist_probe success\n");
	return 0;
}

static int ub_hist_remove(struct platform_device *pdev)
{
	struct ub_hist_ba_device *ba_dev, *tmp;
	unsigned long flags;
	void __iomem *base_addr_to_unmap = NULL;
	struct ub_hist_ba_device *device_to_remove = NULL;

	spin_lock_irqsave(&ub_hist_ba_list_lock, flags);
	list_for_each_entry_safe(ba_dev, tmp, &ub_hist_ba_list, list) {
		if (ba_dev->dev == &pdev->dev) {
			device_to_remove = ba_dev;
			base_addr_to_unmap = ba_dev->base_addr;
			list_del(&ba_dev->list);
			break;
		}
	}
	spin_unlock_irqrestore(&ub_hist_ba_list_lock, flags);

	if (device_to_remove) {
		if (base_addr_to_unmap) {
			iounmap(base_addr_to_unmap);
		}
	}
	return 0;
}

static const struct acpi_device_id ub_hist_acpi_ids[] = {
	{ "HISI0531", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, ub_hist_acpi_ids);

static struct platform_driver ub_hist_platform_driver = {
    .probe = ub_hist_probe,
    .remove = ub_hist_remove,
    .driver =
        {
            .name = "ub_hist_platform_driver",
            .owner = THIS_MODULE,
            .acpi_match_table = ACPI_PTR(ub_hist_acpi_ids),
        },
};

ub_hist_smap_type ub_hist_get_hw_type(void)
{
	return g_ub_hist_smap_type;
}
EXPORT_SYMBOL(ub_hist_get_hw_type);

int ub_hist_init(void)
{
	int ret;

	ret = platform_driver_register(&ub_hist_platform_driver);
	if (ret) {
		pr_err("failed to register platform driver, ret: %d\n", ret);
		return ret;
	}

	pr_debug("UB histogram init successfully\n");
	return ret;
}
EXPORT_SYMBOL(ub_hist_init);

void ub_hist_exit(void)
{
	platform_driver_unregister(&ub_hist_platform_driver);
	pr_info("UB histogram exit successfully\n");
}
EXPORT_SYMBOL(ub_hist_exit);

static int __init histogram_module_init(void)
{
	pr_info("smap hist tracking init successfully.\n");
	return 0;
}

static void __exit histogram_module_exit(void)
{
	pr_info("smap hist tracking exit successfully.\n");
}

MODULE_DESCRIPTION("SMAP hist driver");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL v2");
module_init(histogram_module_init);
module_exit(histogram_module_exit);
