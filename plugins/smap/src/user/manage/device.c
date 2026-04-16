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

typedef enum {
    PGSIZE_FOUR_KB = 0,
    PGSIZE_TWO_MB = 9,
} PageSize;

typedef enum {
    ACCESS_MODE_SUM = 5,
    MODE_MAX,
} ScanMode;

#define MAX_DISABLE_SCAN_ATTEMPS 3

static int SendCmdToAllNodes(int fds[], unsigned long cmd, int arg)
{
    int i;
    int ret = 0;

    for (i = 0; i < MAX_NODES; i++) {
        if (fds[i] >= 0) {
            if (ioctl(fds[i], cmd, arg) < 0) {
                SMAP_LOGGER_ERROR("ioctl for node%d failed: %s, skipped.", i, strerror(errno));
                ret = -EBADF;
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
    int attempts = 0;

    while (attempts < MAX_DISABLE_SCAN_ATTEMPS) {
        ret = DisableTrackingInternal(manager);
        if (!ret) {
            break;
        }
        SMAP_LOGGER_INFO("Scanning still in progress, attempt %d/%d, will retry.",
                            attempts + 1, MAX_DISABLE_SCAN_ATTEMPS);
        attempts++;
        if (attempts < MAX_DISABLE_SCAN_ATTEMPS) {
            sleep(1);
            continue;
        } else {
            SMAP_LOGGER_WARNING("Max attempts reached, skipping current migration cycle.");
            return ret;
        }
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

static int SetNrLocalNuma(struct ProcessManager *manager)
{
    DIR *dir = opendir(SYS_NODE_PATH);
    if (!dir) {
        SMAP_LOGGER_ERROR("Failed to open node directory: %d.", -errno);
        return -ENODEV;
    }

#define NODE_LITERAL_LEN 4
    struct dirent *entry;
    manager->nrLocalNuma = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "node", NODE_LITERAL_LEN) == 0) {
            char *numStr = entry->d_name + NODE_LITERAL_LEN;
            char *endPtr;
            unsigned long num = strtoul(numStr, &endPtr, DECIMAL);
            if (*numStr != '\0' && *endPtr == '\0' && IsLocalNuma(num) && num >= manager->nrLocalNuma) {
                manager->nrLocalNuma = num + 1;
            }
        }
    }
    closedir(dir);
#undef NODE_LITERAL_LEN

    return 0;
}

int ConfigureTrackingDevices(struct ProcessManager *manager)
{
    int ret = SetNrLocalNuma(manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Unable to set local NUMA num: %d.", ret);
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

int GetRamIsChange(struct ProcessManager *manager, int *change)
{
    int fd;
    int ret;

    fd = FindFdByNode(manager->fds.nodes, MAX_NODES);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Can't find fd by node %d.", fd);
        return fd;
    }
    ret = ioctl(fd, SMAP_IOCTL_RAM_CHANGE, change);
    if (ret) {
        SMAP_LOGGER_ERROR("Error when change ram segment : %d.", ret);
        return ret;
    }
    return 0;
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
