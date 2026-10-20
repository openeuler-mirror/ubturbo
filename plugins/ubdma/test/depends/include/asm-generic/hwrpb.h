/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP bug.h stub
 */
#ifndef __ALPHA_HWRPB_H
#define __ALPHA_HWRPB_H
struct hwrpb_struct {
    unsigned long phys_addr;
    unsigned long id;
    unsigned long revision;
    unsigned long size;
    unsigned long cpuid;
    unsigned long pagesize;
    unsigned long pa_bits;
};
extern struct hwrpb_struct *hwrpb;
#endif