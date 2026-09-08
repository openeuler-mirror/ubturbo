
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef __SMAP_IOCTL_H__
#define __SMAP_IOCTL_H__

#include <sys/ioctl.h>
#include <stdint.h>
#include "common.h"
#include "numa_nodes.h"

/*
 * cmd 0->Tracking able config:
 * arg 0->Tracking disable
 * arg 1->Tracking enable
 */

#define SMAP_IOCTL_TRACKING_CMD _IOW(SMAP_ACCESS_MAGIC, 0, unsigned long)

#define SMAP_IOCTL_PAGE_SIZE_SET_CMD _IOW(SMAP_ACCESS_MAGIC, 10, unsigned long)

struct NumaUbFluxMb {
    int numaId;
    uint32_t readMb;
    uint32_t writeMb;
};

struct UbFluxMbStatistic {
    int len;
    struct NumaUbFluxMb flux[REMOTE_NUMA_NUM];
};

#define SMAP_IOCTL_UB_WATCH_CMD _IOR(SMAP_ACCESS_MAGIC, 11, struct UbFluxMbStatistic)

struct UbWatchConfig {
    uint32_t durationMs;
};

#define SMAP_IOCTL_UB_WATCH_CONFIG_CMD _IOW(SMAP_ACCESS_MAGIC, 12, struct UbWatchConfig)

#define SMAP_MIG_MIGRATE _IOW(SMAP_MIGRATE_MAGIC, 0, struct MigrateMsg)
#define SMAP_SET_PAGETYPE _IOW(SMAP_MIGRATE_MAGIC, 1, uint32_t)
#define SMAP_MIG_MIGRATE_NUMA \
    _IOW(SMAP_MIGRATE_MAGIC, 2, struct MigrateNumaIoctlMsg)
#define SMAP_MIG_PID_REMOTE_NUMA \
    _IOW(SMAP_MIGRATE_MAGIC, 3, struct MigPidRemoteNumaIoctlMsg)
#define SMAP_SET_UB_DMA_AVAIL _IOW(SMAP_MIGRATE_MAGIC, 4, unsigned int)

#define SMAP_MIGRATE_BACK _IOW(SMAP_MIGRATE_MAGIC, 5, struct MigrateBackMsg)

#endif /* __SMAP_IOCTL_H__ */
