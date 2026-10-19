/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Description: linux memory.h stub
 */
#ifndef LINUX_MEMORY_H
#define LINUX_MEMORY_H

#define MIN_MEMORY_BLOCK_SIZE     (1UL << SECTION_SIZE_BITS)

#define SECTION_SIZE_BITS 28
#define MEM_ONLINE              (1<<0)
#define MEM_GOING_OFFLINE       (1<<1)
#define MEM_OFFLINE             (1<<2)
#define MEM_GOING_ONLINE        (1<<3)
#define MEM_CANCEL_ONLINE       (1<<4)
#define MEM_CANCEL_OFFLINE      (1<<5)

struct memory_notify {
        unsigned long start_pfn;
        unsigned long nr_pages;
        int status_change_nid_normal;
        int status_change_nid;
};

#endif /* LINUX_MEMORY_H */
