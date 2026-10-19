// SPDX-License-Identifier: GPL-2.0-only
/*
* Description: SMAP3.0 stub function
*/

#ifndef ASM64_GENERIC_HUGETLB_H
#define ASM64_GENERIC_HUGETLB_H

#include <asm/pgtable.h>

#ifdef __cplusplus
extern "C" {
#endif

pte_t huge_ptep_get(pte_t *ptep);

#ifdef __cplusplus
}
#endif

#endif /* ASM64_GENERIC_HUGETLB_H */