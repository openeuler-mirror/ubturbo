/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP Tiering Memory Solution: kvm interface
 */

#ifndef _KVM_PGTABLE_H
#define _KVM_PGTABLE_H

#include <asm/kvm_pgtable.h>
#include <linux/kvm_host.h>

typedef u64 kvm_pte_t;

typedef void (*smap_pte_young_cb)(u64 gpa, bool is_young, bool pte_valid,
				  void *arg);

typedef void (*smap_hole_cb)(u64 gpa_start, u64 gpa_end, void *arg);

struct smap_stage2_range_mkold_data {
	struct kvm *kvm;
	struct kvm_memory_slot *memslot;
	kvm_pte_t attr_set;
	kvm_pte_t attr_clr;
	kvm_pte_t pte;
	size_t stride;
	void *ap;
	smap_pte_young_cb on_pte_young;
	smap_hole_cb on_hole;
};

bool smap_kvm_pgtable_stage2_mkold(struct kvm_pgtable *pgt, u64 addr);

bool smap_kvm_pgtable_stage2_is_young(struct kvm_pgtable *pgt, u64 addr);

int smap_kvm_pgtable_stage2_mkold_range(
	struct kvm_pgtable *pgt, u64 addr, u64 size,
	struct smap_stage2_range_mkold_data *data);

#endif /* _KVM_PGTABLE_H */
