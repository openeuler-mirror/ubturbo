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
#include <dirent.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <time.h>
#include "securec.h"
#include "smap_env.h"
#include "smap_user_log.h"
#include "manage.h"
#include "access_ioctl.h"
#include "smap_ioctl.h"
#include "device.h"

#define DISABLE_TRACKING_RETRY_DELAY_US 50000

typedef enum {
    PGSIZE_FOUR_KB = 0,
    PGSIZE_TWO_MB = 9,
} PageSize;

typedef enum {
    ACCESS_MODE_SUM = 5,
    MODE_MAX,
} ScanMode;

static int SendCmdToAllNodes(int fds[], unsigned long cmd, int arg)
{
    int i;
    int ret = 0;

    for (i = 0; i < MAX_NODES; i++) {
        if (fds[i] >= 0) {
            if (ioctl(fds[i], cmd, arg) < 0) {
                SMAP_LOGGER_DEBUG("ioctl for node%d failed: %s, skipped.", i, strerror(errno));
                return -EBADF;
            }
        }
    }
    return ret;
}

static int FindFdByNode(int fds[], int fdsLength)
{
    int i;

    for (i = 0; i < fdsLength; i++) {
        if (fds[i] >= 0) {
            return fds[i];
        }
    }
    return -EINVAL;
}

int EnableTracking(struct ProcessManager *manager)
{
    return SendCmdToAllNodes(manager->fds.nodes, SMAP_IOCTL_TRACKING_CMD, 1);
}

static inline int DisableTrackingInternal(struct ProcessManager *manager)
{
    return SendCmdToAllNodes(manager->fds.nodes, SMAP_IOCTL_TRACKING_CMD, 0);
}

int DisableTracking(struct ProcessManager *manager)
{
    int ret;

    while (1) {
        ret = DisableTrackingInternal(manager);
        if (!ret) {
            break;
        }
        SMAP_LOGGER_DEBUG("Scanning still in progress, will retry.");
        usleep(DISABLE_TRACKING_RETRY_DELAY_US);
    }
    return ret;
}

static int ConfigTrackingDev(int *trackingFds, uint32_t pageSize)
{
    int ret = 0;
    int arg;
    arg = ACCESS_MODE_SUM;
    ret |= SendCmdToAllNodes(trackingFds, SMAP_IOCTL_MODE_SET_CMD, arg);

    arg = pageSize == PAGESIZE_2M ? PGSIZE_TWO_MB : PGSIZE_FOUR_KB;
    ret |= SendCmdToAllNodes(trackingFds, SMAP_IOCTL_PAGE_SIZE_SET_CMD, arg);

    return ret;
}

static bool IsLocalNuma(unsigned long nid)
{
#define SYS_NODE_REMOTE_LEN 50
    char path[SYS_NODE_REMOTE_LEN] = { 0 };
#undef SYS_NODE_REMOTE_LEN

    if (nid >= LOCAL_NUMA_NUM) {
        return false;
    }

    int ret = snprintf_s(path, sizeof(path), sizeof(path) - 1, "%s/node%lu/remote", SYS_NODE_PATH, nid);
    if (ret == -1) {
        SMAP_LOGGER_ERROR("Failed to build node%lu remote path.", nid);
        return false;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        SMAP_LOGGER_ERROR("Failed to open node%lu remote file.", nid);
        return false;
    }

    int c = fgetc(file);
    if (fclose(file) != 0) {
        SMAP_LOGGER_WARNING("Failed to close node%lu remote file: %d.", nid, errno);
    }
    if (c == EOF) {
        SMAP_LOGGER_ERROR("Failed to read node%lu remote file.", nid);
        return false;
    }

    return c == '0';
}

static int GetNrLocalNumaFromKernel(struct ProcessManager *manager)
{
    if (manager->fds.access < 0) {
        SMAP_LOGGER_ERROR("access device fd is invalid: %d", manager->fds.access);
        return -EINVAL;
    }

    int nrLocalNuma = 0;
    int ret = ioctl(manager->fds.access, SMAP_ACCESS_GET_NR_LOCAL_NUMA, &nrLocalNuma);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("failed to get nr_local_numa from kernel: %d, errno: %d", ret, errno);
        return ret;
    }
    if (nrLocalNuma <= 0 || nrLocalNuma > LOCAL_NUMA_NUM) {
        SMAP_LOGGER_ERROR("get nr_local_numa invalid :%d", nrLocalNuma);
        return -EINVAL;
    }
    manager->nrLocalNuma = nrLocalNuma;
    SMAP_LOGGER_INFO("get nr_local_numa %d from kernel successfully", manager->nrLocalNuma);
    return 0;
}

int ConfigureTrackingDevices(struct ProcessManager *manager)
{
    int ret = GetNrLocalNumaFromKernel(manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Unable to get nr_local_numa from kernel: %d.", ret);
        return ret;
    }

    ret = ConfigTrackingDev(manager->fds.nodes, manager->tracking.pageSize);
    if (ret) {
        SMAP_LOGGER_ERROR("Error when config tracking-node devices.");
        return ret;
    }
    return 0;
}

static int OpenAndFlockFd(int *fd, const char *device)
{
    int tmp = open(device, O_RDWR);
    if (tmp < 0) {
        SMAP_LOGGER_ERROR("cannot find %s, skipped.", device);
        return -ENODEV;
    }

    if (flock(tmp, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            SMAP_LOGGER_ERROR("Device %s is already in use by another process.", device);
        } else {
            SMAP_LOGGER_ERROR("Flock %s failed.", device);
        }
        close(tmp);
        return -EACCES;
    }

    *fd = tmp;
    return 0;
}

int InitTrackingDev(struct ProcessManager *manager)
{
    int i;
    int ret = 0;
    int fd;
    char path[PATH_MAX];

    ret = OpenAndFlockFd(&fd, TIERING_PATH);
    if (ret) {
        return ret;
    }
    manager->fds.migrate = fd;
    fd = open(ACCESS_DEVICE, O_RDWR);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("cannot find access dev under /dev.");
        return -ENODEV;
    }
    manager->fds.access = fd;
    for (i = 0; i < MAX_NODES; i++) {
        ret = snprintf_s(path, sizeof(path), sizeof(path), NODE_PATH, i);
        if (ret == -1) {
            SMAP_LOGGER_ERROR("Build tracking node path failed: %d.", ret);
            return -EINVAL;
        }
        if (access(path, F_OK) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            SMAP_LOGGER_ERROR("%s exists, but cannot be accessed.", path);
            return -ENODEV;
        }
        fd = open(path, O_RDWR);
        if (fd < 0) {
            manager->fds.nodes[i] = DEFAULT_FD;
            SMAP_LOGGER_WARNING("Open tracking node failed: %d.", -errno);
            continue;
        }
        manager->fds.nodes[i] = fd;
        SMAP_LOGGER_INFO("%s is managed.", path);
    }

    ret = ConfigureTrackingDevices(manager);
    if (ret) {
        SMAP_LOGGER_ERROR("config tracking devices failed : %d.", ret);
    }
    return ret;
}

void DeinitTrackingDev(struct ProcessManager *manager)
{
    int i;

    DisableTracking(manager);
    for (i = 0; i < MAX_NODES; i++) {
        if (manager->fds.nodes[i] >= 0) {
            close(manager->fds.nodes[i]);
            manager->fds.nodes[i] = DEFAULT_FD;
        }
    }
    if (manager->fds.migrate >= 0) {
        close(manager->fds.migrate);
        manager->fds.migrate = DEFAULT_FD;
    }
    if (manager->fds.access >= 0) {
        close(manager->fds.access);
        manager->fds.access = DEFAULT_FD;
    }
}
