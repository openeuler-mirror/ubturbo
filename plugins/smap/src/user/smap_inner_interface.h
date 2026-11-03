/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef __SMAP_INNER_INTERFACE_H__
#define __SMAP_INNER_INTERFACE_H__

#include "smap_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DISABLE_MIG_INFO,
    ENABLE_MIG_INFO,
};

enum {
    DISABLE_ADAPT_MEM,
    ENABLE_ADAPT_MEM,
};

struct VmRatio {
    pid_t pid;
    double ratio;
};

struct VmRatioMsg {
    int nrVm;
    struct VmRatio vr[MAX_2M_PROCESSES_CNT];
};

int SmapEnableAdaptMem(int flag);
int SmapQueryVmMemRatio(struct VmRatioMsg *vrMsg);
int SmapSetLogLevel(int level);

/* NORMAL_SCAN only. Milliseconds: scan [50, 60000], migrate [500, 300000], scan <= migrate.
 * Directly updates the PID's scanTime/migPeriod under its slot attrLock.
 * Effective priority: enabled file configuration > PID values > defaults (200/2000 ms).
 * Kernel scanning uses these values from the next window; ioctl failures retry next window.
 * Errors: -EPERM, -ENOENT, -EINVAL, -EOPNOTSUPP.
 */
int SmapSetPidPeriod(pid_t pid, uint32_t scanPeriodMs, uint32_t migratePeriodMs);
/* Restore PID values to 200/2000 ms; enabled file configuration still takes priority. */
int SmapResetPidPeriod(pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* __SMAP_INNER_INTERFACE_H__ */
