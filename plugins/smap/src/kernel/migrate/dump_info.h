/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP dump info module
 */

#ifndef _SRC_DUMP_INFO_H
#define _SRC_DUMP_INFO_H

#include "smap_msg.h"

#define FILENAME_LEN 100
#define TIME_STR_LEN 30
#define TZ_OFFSET (8 * 60 * 60)
#define EPOCH (1900L)

void filter_4k_migrate_info(size_t *nr_abnormal);

#endif /* _SRC_DUMP_INFO_H */
