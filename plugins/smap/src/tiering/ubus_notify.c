#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/cper.h>
#include <acpi/ghes.h>
#include <linux/numa_remote.h>

#include "trouble_numa_meta.h"
#include "ubus_notify.h"

#pragma pack(push, 1)  // 强制1字节对齐

// 华为 Vendor Specific Error Info 结构体
struct hisi_vendor_error_info {
    // Magic Number (8字节)
    uint32_t magic_num_l;      // 0xCC08CC08
    uint32_t magic_num_h;      // 0xCC08CC08
    
    // 版本信息
    uint32_t ver_info;          // Version number, start from 0x00000000
    
    // Error Record Feature Register (64位)
    uint32_t err_fr_0;          // Feature Register低32位
    uint32_t err_fr_1;          // Feature Register高32位
    
    // Error Record Control Register (64位)
    uint32_t err_ctrl_0;        // Control Register低32位
    uint32_t err_ctrl_1;        // Control Register高32位
    
    // Error Record Primary Status Register (64位)
    uint32_t err_status_0;      // Status Register低32位
    uint32_t err_status_1;      // Status Register高32位
    
    // Error Record Address Register (64位)
    uint32_t err_addr_0;        // Address Register低32位
    uint32_t err_addr_1;        // Address Register高32位
    
    // Error Record Miscellaneous Register 0 (64位)
    uint32_t err_misc0_0;       // Misc Register 0低32位
    uint32_t err_misc0_1;       // Misc Register 0高32位
    
    // Error Record Miscellaneous Register 1 (64位)
    uint32_t err_misc1_0;       // Misc Register 1低32位
    uint32_t err_misc1_1;       // Misc Register 1高32位
    
    // SEI Information (64位)
    uint32_t sei_info_l;        // SEI Information低32位
    uint32_t sei_info_h;        // SEI Information高32位
    
    // L2BERRSR_EL1 Register (64位)
    uint32_t l2berrsr_el1_l;    // L2BERRSR_EL1低32位
    uint32_t l2berrsr_el1_h;    // L2BERRSR_EL1高32位
    
    // PC Point for User Space (64位)
    uint32_t pc_el0_l;          // PC (User Space)低32位
    uint32_t pc_el0_h;          // PC (User Space)高32位
    
    // PC Point for Kernel Space (64位)
    uint32_t pc_el2_l;          // PC (Kernel Space)低32位
    uint32_t pc_el2_h;          // PC (Kernel Space)高32位
    
    // Physical Address of UB Memory (64位)
    uint32_t pa_l;              // Physical Address低32位
    uint32_t pa_h;              // Physical Address高32位
    
    // Port Bits (64位)
    uint32_t port_bits_l;       // Port Bits低32位，每位对应一个port
    uint32_t port_bits_h;       // Port Bits高32位
    
    // Port Link Status Bits (64位)
    uint32_t port_status_bits_l; // 0: linkdown, 1: linkup
    uint32_t port_status_bits_h;
    
    // MAR ID
    uint32_t marid;             // Associated MAR ID
    
    // MAR Error Status
    uint32_t mar_status;        // bit0: mar_err, bit4: mar_timeout, bit8: mar_noport_vld
    
    // SMT ID (Process Address Space IDentifier)
    uint32_t smt_id;            // 在MPIDR_EL1中承载
};

#pragma pack(pop)

static guid_t hisi_ubus_sec_guid = 
    GUID_INIT(0xE19E3D16, 0xBC11, 0x11E4, 0x9C, 0xAA, 0xC2, 0x05, 0x1D, 0x5D, 0x46, 0xB0);

static int is_link_down_err(struct cper_sec_proc_arm *err)
{
    unsigned long err_info_sz;
    struct hisi_vendor_error_info *err_info = NULL;
    char *p = (char *)err;
    int i;

    if (err->validation_bits & CPER_ARM_VALID_VENDOR_INFO) {
        err_info_sz = sizeof(struct cper_arm_err_info) * err->err_info_num;
        if (!err->context_info_num) {
            err_info = (struct hisi_vendor_error_info *)(p + err_info_sz);
        } else {
            struct cper_arm_ctx_info *ctx_info = (struct cper_arm_ctx_info *)(p + err_info_sz);
            err_info = (struct hisi_vendor_error_info *)(p + err_info_sz + ctx_info->size * err->context_info_num);
        }
        for (i = 0; i < 32; i++) {
            if (!(err_info->port_bits_l & (1 << i))) {
                return 1;
            }
        }
    }

    return 0;
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
        int has_pa = err_info->validation_bits & CPER_ARM_INFO_VALID_PHYSICAL_ADDR;
        if (has_pa) {
            pfn = PHYS_PFN(err_info->physical_fault_addr);
            if (!pfn_valid(pfn)) {
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
    if (is_link_down_err(err_data)) {
        ghes_handle_critical_ras(err_data);
        pr_info("ubus ras submit link down.\n");
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