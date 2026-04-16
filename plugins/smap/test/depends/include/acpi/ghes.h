/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */

#ifndef __GHES_H__
#define __GHES_H__

#include <linux/cper.h>
#include <linux/notifier.h>
#include <linux/types.h>

struct acpi_hest_generic_data {
    u8 section_type[16];
    void *payload;
};

static inline void *acpi_hest_get_payload(struct acpi_hest_generic_data *gdata)
{
    if (gdata->payload) {
        return gdata->payload;
    }

    return (void *)(gdata + 1);
}

static inline bool arch_is_platform_page(phys_addr_t addr)
{
    (void)addr;
    return false;
}

static inline int ghes_register_vendor_record_notifier(
    struct notifier_block *nb)
{
    (void)nb;
    return 0;
}

static inline void ghes_unregister_vendor_record_notifier(
    struct notifier_block *nb)
{
    (void)nb;
}

#endif /* __GHES_H__ */
