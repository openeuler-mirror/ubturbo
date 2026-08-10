/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SMAP Tiering Memory Solution: kvm interface
 */

#ifndef _KVM_PGTABLE_H
#define _KVM_PGTABLE_H

#include <asm/kvm_pgtable.h>
#include <linux/kvm_host.h>

typedef u64 kvm_pte_t;
bool smap_kvm_pgtable_stage2_mkold(struct kvm_pgtable *pgt, u64 addr);

bool smap_kvm_pgtable_stage2_is_young(struct kvm_pgtable *pgt, u64 addr);

/*
 * Callback for bulk stage-2 walk: called for each valid leaf PTE.
 * @gpa: guest physical address of the PTE
 * @young: true if the access flag was set
 * @pte_valid: true if the PTE is valid
 * @arg: caller-provided context
 * Returns: 0 on success, negative on error (stops the walk)
 */
typedef int (*smap_pte_cb)(u64 gpa, u64 hpa, bool young, bool pte_valid,
			   void *arg);

typedef void (*smap_hole_cb)(u64 gpa_start, u64 gpa_end, void *arg);

/* Per-range data for bulk stage-2 mkold walk */
struct smap_stage2_range_data {
	smap_pte_cb on_pte;
	smap_hole_cb on_hole;
	void *cb_arg;
	/* Output: counters set by the walk */
	int total_ptes;
	int young_ptes;
};

int smap_kvm_pgtable_stage2_mkold_range(struct kvm_pgtable *pgt, u64 addr,
					u64 size,
					struct smap_stage2_range_data *data);

#endif /* _KVM_PGTABLE_H */
