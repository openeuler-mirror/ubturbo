#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/cper.h>
#include <acpi/ghes.h>
#include <linux/numa_remote.h>

#include "trouble_numa_meta.h"
#include "ubus_notify.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_ubus_notify: " fmt

struct hisi_armp_vendor_info {
	u64 magic_num;
	u32 ver_info;
	u32 err_flag;	/* bit0:critical error, others: reserved */
	u32 *regs;
} __packed;

#define HISI_VENDOR_MAGIC_NUM		0xCC08CC08CC08CC08
#define HISI_VENDOR_CRITICAL_ERR	BIT(0)

static guid_t hisi_ubus_sec_guid = 
    GUID_INIT(0xE19E3D16, 0xBC11, 0x11E4, 0x9C, 0xAA, 0xC2, 0x05, 0x1D, 0x5D, 0x46, 0xB0);

static int ghes_hisi_critical_hw_error(struct cper_sec_proc_arm *err)
{
	struct hisi_armp_vendor_info *vendor_info;
	unsigned long err_info_sz;
	char *p;

	if (!(err->validation_bits & CPER_ARM_VALID_VENDOR_INFO))
		return 0;

	p = (char *)(err + 1);
	err_info_sz = sizeof(struct cper_arm_err_info) * err->err_info_num;
	if (!err->context_info_num) {
		vendor_info = (struct hisi_armp_vendor_info *)
			(p + err_info_sz);
	} else {
		struct cper_arm_ctx_info *ctx_info = (struct cper_arm_ctx_info *)
			(p + err_info_sz);

		vendor_info = (struct hisi_armp_vendor_info *)
			(p + err_info_sz +
			ctx_info->size * err->context_info_num);
	}

	if (vendor_info->magic_num != HISI_VENDOR_MAGIC_NUM) {
		return 0;
    }

	return (vendor_info->err_flag & HISI_VENDOR_CRITICAL_ERR);
}

static int ghes_handle_addr_to_numa(unsigned long pfn)
{
    struct page *p;
    int node;
    int ret;

    p = pfn_to_page(pfn);
    if (!p) {
        pr_err("invalid pfn: %lu\n", pfn);
        return -EINVAL;
    }

    node = page_to_nid(p);
    if (!numa_is_remote_node(node)) {
        pr_err("invalid local node: %d\n", node);
        return -EINVAL;
    }

    ret = trouble_numa_list_add(node);
    if (ret && ret != -EEXIST) {
        pr_err("failed to add trouble NUMA node: %d\n", node);
        return ret;
    }
    if (ret != -EEXIST) {
        pr_info("ubus ras add trouble numa meta, NUMA node: %d\n", node);
    }

    return 0;
}

static void ghes_handle_critical_ras(struct cper_sec_proc_arm *err)
{
    int i;
    unsigned long pfn;
    char *p;
    int ret;

    p = (char *)(err + 1);
    for (i = 0; i < err->err_info_num; i++) {
        struct cper_arm_err_info *err_info = (struct cper_arm_err_info *)p;
        int is_cache = (err_info->type == CPER_ARM_CACHE_ERROR);
        int has_pa = err_info->validation_bits & CPER_ARM_INFO_VALID_PHYSICAL_ADDR;
        if (is_cache && has_pa) {
            pfn = PHYS_PFN(err_info->physical_fault_addr);
            if (!pfn_valid(pfn) && !arch_is_platform_page(err_info->physical_fault_addr)) {
                pr_err("invalid pfn: %lu\n", pfn);
                continue;
            }
            ret = ghes_handle_addr_to_numa(pfn);
            if (ret) {
                pr_err("failed to handle addr to numa: %d\n", ret);
                continue;
            }
        }
        p += err_info->length;
    }
}

int hisi_ubus_notify_error(struct notifier_block *nb, unsigned long event, void *data)
{
    struct acpi_hest_generic_data *gdata;
    guid_t err_guid;

    gdata = (struct acpi_hest_generic_data *)data;
    import_guid(&err_guid, gdata->section_type);
    if (!guid_equal(&err_guid, &hisi_ubus_sec_guid)) {
        return NOTIFY_DONE;
    }

    struct cper_sec_proc_arm *err_data = (struct cper_sec_proc_arm *)acpi_hest_get_payload(gdata);
    if (ghes_hisi_critical_hw_error(err_data)) {
        pr_info("ubus ras submit link down.\n");
        ghes_handle_critical_ras(err_data);
    }

    return NOTIFY_DONE;
};

static struct notifier_block hisi_ubus_link_down_notify_err = {
	.notifier_call = hisi_ubus_notify_error,
	.priority = 100,
};

int hisi_ubus_register_link_down_notifier(void)
{
	return ghes_register_vendor_record_notifier(&hisi_ubus_link_down_notify_err);
}

void hisi_ubus_unregister_link_down_notifier(void)
{
    ghes_unregister_vendor_record_notifier(&hisi_ubus_link_down_notify_err);
}
