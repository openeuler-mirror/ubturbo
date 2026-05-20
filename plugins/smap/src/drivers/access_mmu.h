/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: smap access mmu module
 */

#ifndef _SRC_ACCESS_MMU_H
#define _SRC_ACCESS_MMU_H

#include "access_pid.h"
#include "check.h"

extern struct list_head remote_ram_list;
extern rwlock_t rem_ram_list_lock;

void walk_pid_pagemap(struct pagemapread *pm);
struct mm_struct *get_mm_by_pid(pid_t pid);

int add_to_bm_hugepage(u64 vaddr, u64 paddr, struct access_pid *ap);
int add_to_bm_page(u64 paddr, struct access_pid *ap);
int add_to_bm_page_fast(u64 paddr, int nid, u64 acidx, struct access_pid *ap);
bool is_file_or_shared_page(phys_addr_t paddr);

static inline void add_to_bm_huge(u64 vaddr, u64 paddr, struct access_pid *ap)
{
	u64 mask_paddr = paddr & TWO_MEGA_MASK;
	add_to_bm_hugepage(vaddr, mask_paddr, ap);
}

#endif /* _SRC_ACCESS_MMU_H */
