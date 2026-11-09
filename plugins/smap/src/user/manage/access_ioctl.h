/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef __ACCESS_IOCTL_H__
#define __ACCESS_IOCTL_H__

#include <sys/ioctl.h>
#include <linux/types.h>
#include <stddef.h>

#include "common.h"
#include "manage.h"

#define ACCESS_DEVICE "/dev/smap_scan_dev"
#define MAX_NR_PID MAX_NR_MIGOUT

/*
 * 调用方视图：numaNodes 为变长位图句柄（长度由运行期 cap 折算），
 * 线格式序列化在 AccessIoctlAddPid 内部完成。
 */
struct AccessAddPidPayload {
    pid_t pid;
    uint32_t scanTime;
    NumaBitmap numaNodes;
    uint32_t duration;
    ScanType type;
    uint32_t nTimes;
    PidType pidType; /* per-pid 身份：VM_TYPE/PROCESS_TYPE，与全局 pageType 解耦，镜像内核 access_pid.pid_type */
};

/*
 * 线格式（user → kernel）：定长头 + 尾部柔性位图，与内核 struct access_add_pid_payload
 * 逐字节同构。numaNodes 字数由调用侧运行期 cap 决定，不再按 SMAP_MAX_NUMNODES 定长。
 */
struct AccessAddPidWirePayload {
    pid_t pid;
    uint32_t scanTime;
    uint32_t nrWords; // 尾部 numaNodes 字数
    uint32_t duration;
    ScanType type;
    uint32_t nTimes;
    PidType pidType;
    unsigned long numaNodes[]; // 柔性数组，长度 nrWords
};

struct AccessAddPidMsg {
    int count;
    uint32_t stride; // 单条 wire 记录字节数 = offsetof(numaNodes) + nrWords * sizeof(unsigned long)
    struct AccessAddPidWirePayload *payload;
};

struct AccessRemovePidPayload {
    pid_t pid;
};

struct AccessRemovePidMsg {
    int count;
    struct AccessRemovePidPayload *payload;
};

struct AccessPidFreq {
    pid_t pid;
    size_t len[SMAP_MAX_NUMNODES];
    actc_t *freq[SMAP_MAX_NUMNODES];
};

struct TrakingInfoPayload {
    pid_t pid;
    uint32_t length;
    uint16_t *data; /* DFX 统计扫描频次，保留原始 u16 真值，不压缩 */
};

struct UserInfo {
    uid_t uid;
    gid_t gid;
};

struct SmapScanCpuRange {
    uint32_t cpuMin;
    uint32_t cpuMax;
};

#define SMAP_ACCESS_ADD_PID _IOW(SMAP_ACCESS_MAGIC, 1, struct AccessAddPidMsg)
#define SMAP_ACCESS_REMOVE_PID _IOW(SMAP_ACCESS_MAGIC, 2, struct AccessRemovePidMsg)
#define SMAP_ACCESS_REMOVE_ALL_PID _IOW(SMAP_ACCESS_MAGIC, 3, int)
/* per-pid页数查询：与内核struct smap_pid_page_num布局对齐（pid_t + 每节点一个size_t） */
#define SMAP_ACCESS_WALK_PAGEMAP _IOWR(SMAP_ACCESS_MAGIC, 4, struct ProcessMemBitmap)
#define SMAP_ACCESS_GET_TRACKING _IOW(SMAP_ACCESS_MAGIC, 5, struct TrakingInfoPayload)
#define SMAP_ACCESS_CREATE_PROCFS _IOW(SMAP_ACCESS_MAGIC, 6, struct UserInfo)
#define SMAP_ACCESS_GET_NR_LOCAL_NUMA _IOR(SMAP_ACCESS_MAGIC, 7, int)
#define SMAP_ACCESS_REFRESH_REMOTE_RAM _IO(SMAP_ACCESS_MAGIC, 8)
#define SMAP_ACCESS_SET_SCAN_CPU _IOW(SMAP_ACCESS_MAGIC, 9, struct SmapScanCpuRange)

int AccessIoctlAddPid(int len, struct AccessAddPidPayload *payload);
int AccessIoctlRemovePid(int len, struct AccessRemovePidPayload *payload);
int AccessIoctlRemoveAllPid(void);
int AccessIoctlWalkPagemap(pid_t pid, struct ProcessMemBitmap *pmb);
int AccessIoctlRestartPid(pid_t pid);
int AccessIoctlCreateProcfs(struct UserInfo *ui);
void IoctlUpdateUbDmaAvail(uint32_t value);
void IoctlSetScanCpuRange(uint32_t cpuMin, uint32_t cpuMax);
void IoctlSetRemoteScanMode(uint32_t mode);

#endif /* __ACCESS_IOCTL_H__ */
