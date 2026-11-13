/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/* manage_internal.h — manage 模块内部跨文件调用声明 */

#ifndef MANAGE_INTERNAL_H
#define MANAGE_INTERNAL_H

#include "manage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 共享宏定义（原定义在 manage.c，供多个拆分文件使用） */
#define MAX_GROUP_TARGET_ENTRY (MAX_MIGRATION_GROUP_NUM * MAX_GROUP_REMOTE_NUMA)
#define FREQ_FILE_PATH_LEN 50

/* ========== manage.c 导出给其他文件的内部函数 ========== */

/* 共享全局变量（定义在 manage.c） */
extern uint32_t g_pageSizeNormal;
extern uint32_t g_pageSizeHuge;
extern RunMode g_runMode;
extern uint8_t g_criticalErrNodes[SMAP_NODE_CAP_LIMIT];

/* 共享类型定义（原定义在 manage.c，供多个拆分文件使用） */
typedef struct {
    uint32_t affinityLocalMask;
    uint32_t residentLocalMask;
    uint64_t numaPages[SMAP_MAX_NUMNODES];
    bool affinityValid;
    bool affinitySampled;
    bool residentValid;
} ManagedLocalObservation;

/* manage_pair.c 使用 */
uint32_t BuildAllLocalNumaMask(void);
uint32_t BuildAccountLocalMask(const ProcessAttr *attr, int remoteNid);

/* manage_scan.c 使用 */
void ResetActcData(ActcData *actcData[], int len);
/* 释放本周期 actc 快照（actcLen 清零 + 缓冲释放），无数据时为 no-op。
 * scan worker 单线程调用，actcData 无并发读者，不需要锁。 */
void ClearProcessActcData(ProcessAttr *attr);

/* freq_query 等待者：IPC 线程栈分配，注册到 PidSlot.freqWaiters 链表后 sem_wait，
 * scan worker 在 BuildPidData 填充完成后持 attrLock 遍历链表 copy+post。 */
struct FreqWaiter {
    uint16_t *buf; /* IPC 响应缓冲 */
    uint32_t bufLen; /* 缓冲容量 */
    uint32_t outLen; /* 实际填充长度（scan worker 写） */
    int retCode; /* 结果码（scan worker 写，IPC 线程读） */
    sem_t sem; /* 初始 0，scan worker post 唤醒 */
    struct FreqWaiter *next;
};
/* 注册等待者到 attr 所属槽位的 freqWaiters 链表（持 attrLock 插入头）。
 * 返回 0 = 已注册（有回调来源）；-EINVAL = attr/w 非法或槽位未注册（无回调
 * 来源，调用方应立即失败而非等待） */
int RegisterFreqWaiter(ProcessAttr *attr, struct FreqWaiter *w);
/* 超时取消等待者。返回 true = waiter 已被 scan worker take-all 处理（sem 已 post，
 * 调用方 sem_trywait 消费后读结果）；false = 未被处理（已摘除该 waiter / attr=w 非法 /
 * 槽位未注册，均无 post 来源，retCode 保持 -ETIMEDOUT） */
bool CancelFreqWaiter(ProcessAttr *attr, struct FreqWaiter *w);
/* BuildPidData 填充完成后调用：持 attrLock take 链表，遍历 copy actcData→各 waiter.buf，sem_post */
void InvokeFreqCallbacks(ProcessAttr *attr);

/* manage_grouped.c 使用 */
int DetectPidType(pid_t pid);
int RefreshManagedLocalState(ProcessAttr *attr, bool fullReplacement);
int ConfigureMigrationTargetsWithCapacityPolicy(ProcessAttr *attr, const ProcessTargetConfig *config,
                                                bool ignoreRemoteCapacity, bool skipRemoteResidencyCheck);
int GetProcessNumaMapsObservation(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask,
                                  uint64_t numaPages[SMAP_MAX_NUMNODES]);
int CollectProcessCandidateObservation(pid_t pid, bool hugeFlag, ManagedLocalObservation *observation);
int ApplyManagedLocalObservation(ProcessAttr *attr, const ManagedLocalObservation *observation, bool fullReplacement);

/* manage_remote.c 使用 */
int BuildProcessTargetConfigFromParam(const ProcessParam *param, ProcessTargetConfig *config);
int ApplyProcessTargetConfig(ProcessAttr *attr, const ProcessTargetConfig *config, bool skipRemoteResidencyCheck);
int StagePendingMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config, bool ignoreRemoteCapacity);
int SetProcessConfig(ProcessAttr *attr, ProcessParam *param, bool skipRemoteResidencyCheck);
void PublishProcessTargetCandidate(ProcessAttr *attr, const ProcessAttr *candidate);
void SetBasicProcessConfig(ProcessAttr *attr, ProcessParam *param);
void SetMultiNumaConfig(ProcessAttr *attr, ProcessParam *param, int nrLocalNuma);

/* ========== manage_remote.c 导出给其他文件的内部函数 ========== */

/* (已在 manage.h 中声明的函数不再重复) */

/* ========== manage_numa.c 导出给其他文件的内部函数 ========== */

void CalRemoteNumaSizeAllocPerNuma(void);
void ClearRemoteMemUsed(void);
void CalRemoteMemUsed(void);
/* 事件驱动流水的在线账本（manage_numa.c）：快照与预扣须由调用方持账本锁调用，
 * 与锁内剪辑配合实现「快照→剪辑→预扣」原子化，杜绝并发超卖。
 * 节点维度一律走运行期 cap：privateAvail 为 nrLocalNuma 行 × cap 列扁平矩阵
 * （行跨度 = cap，经 NM_AT 访问）；sharedAvail 按远端绝对 nid 定维（长度 cap） */
void SnapshotRemoteCapacityLocked(uint64_t *privateAvail, uint64_t *sharedAvail);
void LedgerPlanInFlightLocked(const uint32_t *nrMigratePages, int nrLocalNuma);
void LedgerSettleInFlight(const uint32_t *planned, int nrLocalNuma);

/* ========== manage_scan.c 导出给其他文件的内部函数 ========== */

void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len);
void CalcActcStats(ProcessAttr *attr);
void DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf);
int RefreshManagedLocalStatePeriodic(ProcessAttr *attr);
/* BuildPidData/RefreshAllPidData 已在 manage.h 中声明 */

/* ========== manage_grouped.c 导出给其他文件的内部函数 ========== */

void SetGroupedProcessConfig(ProcessAttr *attr, pid_t pid, const NumaBitmap *nodeBitmap,
                             const GroupMigrationPolicy *policy);

/* ========== manage_pair.c 导出给其他文件的内部函数 ========== */

/* (CalibratePairAccount 等已在 manage.h 中声明) */

#ifdef __cplusplus
}
#endif

#endif /* MANAGE_INTERNAL_H */
