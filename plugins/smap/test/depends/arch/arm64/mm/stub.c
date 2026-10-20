// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description: arm64 mm stub
 */

#include <asm/pgtable-types.h>

pte_t g_tmp_pte = { 0 };

pte_t ptep_get(pte_t *ptep)
{
    if (!ptep)
        return g_tmp_pte;
    return *ptep;
}
