/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "smap_user_log.h"
#include "securec.h"

#include "thread.h"
#include "manage.h"
#include "device.h"
#include "access_ioctl.h"
#include "smap_ioctl.h"

int AccessIoctlAddPid(int len, struct AccessAddPidPayload *payload)
{
    if (len <= 0 || len > GetCurrentMaxNrPid()) {
        SMAP_LOGGER_ERROR("AccessIoctlAddPid: invalid len %d.", len);
        return -EINVAL;
    }
    struct AccessAddPidMsg accessMsg = { .count = len };
    accessMsg.payload = malloc(len * sizeof(struct AccessAddPidPayload));
    if (accessMsg.payload == NULL) {
        SMAP_LOGGER_ERROR("AccessIoctlAddPid: malloc failed.");
        return -ENOMEM;
    }
    struct ProcessManager *manager = GetProcessManager();
    uint32_t migrationPeriod;
    migrationPeriod = manager->migPeriod;

    for (int i = 0; i < len; i++) {
        accessMsg.payload[i].pid = payload[i].pid;
        accessMsg.payload[i].numaNodes = payload[i].numaNodes;
        if (payload[i].scanTime == 0) {
            SMAP_LOGGER_ERROR("AccessIoctlAddPid: invalid payload[%d].scanTime %u.", i, payload[i].scanTime);
            free(accessMsg.payload);
            return -EINVAL;
        }
        accessMsg.payload[i].scanTime = payload[i].scanTime;
        accessMsg.payload[i].duration = payload[i].duration;
        if (payload[i].type == STATISTIC_SCAN) {
            accessMsg.payload[i].nTimes = payload[i].duration * MS_PER_SEC / payload[i].scanTime;
        } else {
            accessMsg.payload[i].nTimes = migrationPeriod / payload[i].scanTime;
        }
        accessMsg.payload[i].type = payload[i].type;
        accessMsg.payload[i].pidType = payload[i].pidType;
    }
    int ret = ioctl(manager->fds.access, SMAP_ACCESS_ADD_PID, &accessMsg);
    free(accessMsg.payload);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("access ioctl add pids error: %d.", ret);
        ret = -EBADF;
    } else {
        for (int i = 0; i < len; i++) {
            if (payload[i].type == NORMAL_SCAN) {
                EventLoopRegisterPid(payload[i].pid);
            }
        }
    }
    return ret;
}

int AccessIoctlRemovePid(int len, struct AccessRemovePidPayload *payload)
{
    if (len <= 0 || len > GetCurrentMaxNrPid()) {
        SMAP_LOGGER_ERROR("AccessIoctlRemovePid: invalid len %d.", len);
        return -EINVAL;
    }
    struct AccessRemovePidMsg accessMsg = { .count = len };
    accessMsg.payload = malloc(len * sizeof(struct AccessRemovePidPayload));
    if (accessMsg.payload == NULL) {
        SMAP_LOGGER_ERROR("AccessIoctlRemovePid: malloc failed.");
        return -ENOMEM;
    }
    struct ProcessManager *manager = GetProcessManager();

    for (int i = 0; i < len; i++) {
        accessMsg.payload[i].pid = payload[i].pid;
    }
    int ret = ioctl(manager->fds.access, SMAP_ACCESS_REMOVE_PID, &accessMsg);
    free(accessMsg.payload);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("access ioctl remove pid error: %d.", ret);
        ret = -EBADF;
    }
    return ret;
}

int AccessIoctlRemoveAllPid(void)
{
    struct ProcessManager *manager = GetProcessManager();

    int ret = ioctl(manager->fds.access, SMAP_ACCESS_REMOVE_ALL_PID, 0);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("access ioctl remove all pid error: %d.", ret);
        ret = -EBADF;
    }
    return ret;
}

int AccessIoctlWalkPagemap(pid_t pid, struct ProcessMemBitmap *pmb)
{
    struct ProcessManager *manager = GetProcessManager();
    struct ProcessMemBitmap param;
    int ret;

    if (!pmb) {
        return -EINVAL;
    }
    (void)memset_s(&param, sizeof(param), 0, sizeof(param));
    param.pid = pid;
    ret = ioctl(manager->fds.access, SMAP_ACCESS_WALK_PAGEMAP, &param);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("access walk pagemap for pid %d error: %d.", pid, -errno);
        return -errno;
    }
    *pmb = param;
    return 0;
}

int AccessIoctlCreateProcfs(struct UserInfo *ui)
{
    struct ProcessManager *manager = GetProcessManager();

    int ret = ioctl(manager->fds.access, SMAP_ACCESS_CREATE_PROCFS, ui);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("access create procfs error: %d\n", -errno);
        ret = -EBADF;
    }
    return ret;
}

void IoctlUpdateUbDmaAvail(uint32_t value)
{
    struct ProcessManager *manager = GetProcessManager();
    uint32_t val = value;

    int ret = ioctl(manager->fds.migrate, SMAP_SET_UB_DMA_AVAIL, &val);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("ioctl update ub dma avail failed: %d, errno %d", ret, errno);
        return;
    }

    SMAP_LOGGER_INFO("ioctl update ub dma avail: %u", val);
}

void IoctlSetScanCpuRange(uint32_t cpuMin, uint32_t cpuMax)
{
    struct ProcessManager *manager = GetProcessManager();
    struct SmapScanCpuRange range = { cpuMin, cpuMax };

    int ret = ioctl(manager->fds.access, SMAP_ACCESS_SET_SCAN_CPU, &range);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("ioctl set scan cpu range failed: %d, errno %d.", ret, errno);
        return;
    }

    SMAP_LOGGER_INFO("ioctl set scan cpu range=%u-%u", cpuMin, cpuMax);
}

void IoctlSetRemoteScanMode(uint32_t mode)
{
    struct ProcessManager *manager = GetProcessManager();
    int ret = ioctl(manager->fds.access, SMAP_IOCTL_SCAN_MODE_SET_CMD, mode);

    if (ret < 0) {
        ret = -errno;
        SMAP_LOGGER_ERROR("ioctl set remote scan mode failed: %s.", strerror(-ret));
        return;
    }
    SMAP_LOGGER_INFO("Set remote scan mode to %u success.", mode);
}

int AccessIoctlRestartPid(pid_t pid)
{
    struct ProcessManager *manager = GetProcessManager();

    /* TRACKING_CMD 复用为 per-pid 窗口重启：arg=pid；-ENOENT 表示 pid 未被内核跟踪 */
    int ret = ioctl(manager->fds.access, SMAP_IOCTL_TRACKING_CMD, pid);
    if (ret < 0) {
        ret = errno;
        SMAP_LOGGER_ERROR("ioctl restart pid %d scan failed: %d.", ret, errno);
        return -ret;
    }
    return ret;
}
