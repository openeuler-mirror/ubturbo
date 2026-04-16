/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_CPER_H
#define __LINUX_CPER_H

#include <linux/bitops.h>
#include <linux/types.h>

typedef struct {
    u8 b[16];
} guid_t;

#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    {{ (u8)(a), (u8)((a) >> 8), (u8)((a) >> 16), (u8)((a) >> 24), \
       (u8)(b), (u8)((b) >> 8), (u8)(c), (u8)((c) >> 8), \
       (u8)(d0), (u8)(d1), (u8)(d2), (u8)(d3), \
       (u8)(d4), (u8)(d5), (u8)(d6), (u8)(d7) }}

static inline void import_guid(guid_t *dst, const u8 *src)
{
    int i;

    for (i = 0; i < 16; i++) {
        dst->b[i] = src[i];
    }
}

static inline bool guid_equal(const guid_t *guid1, const guid_t *guid2)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (guid1->b[i] != guid2->b[i]) {
            return false;
        }
    }

    return true;
}

#define CPER_ARM_VALID_VENDOR_INFO          BIT(3)
#define CPER_ARM_INFO_VALID_PHYSICAL_ADDR   BIT(4)
#define CPER_ARM_CACHE_ERROR                0

struct cper_sec_proc_arm {
    u32 validation_bits;
    u16 err_info_num;
    u16 context_info_num;
    u32 section_length;
    u8 affinity_level;
    u8 reserved[3];
    u64 mpidr;
    u64 midr;
    u32 running_state;
    u32 psci_state;
};

struct cper_arm_err_info {
    u8 version;
    u8 length;
    u16 validation_bits;
    u8 type;
    u16 multiple_error;
    u8 flags;
    u64 error_info;
    u64 virt_fault_addr;
    u64 physical_fault_addr;
};

struct cper_arm_ctx_info {
    u16 version;
    u16 type;
    u32 size;
};

#endif /* __LINUX_CPER_H */
