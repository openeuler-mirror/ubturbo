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
extern uint8_t g_criticalErrNodes[REMOTE_NUMA_BITS];

/* 共享类型定义（原定义在 manage.c，供多个拆分文件使用） */
typedef struct {
    uint32_t affinityLocalMask;
    uint32_t residentLocalMask;
    uint64_t numaPages[MAX_NODES];
    bool affinityValid;
    bool affinitySampled;
    bool residentValid;
} ManagedLocalObservation;

/* manage_pair.c 使用 */
uint32_t BuildAllLocalNumaMask(void);
uint32_t BuildAccountLocalMask(const ProcessAttr *attr, int remoteIndex);

/* manage_scan.c 使用 */
void ResetActcData(ActcData *actcData[], int len);

/* manage_grouped.c 使用 */
int DetectPidType(pid_t pid);
int RefreshManagedLocalState(ProcessAttr *attr, bool fullReplacement);
uint32_t BuildManagedTrackingNodes(const ProcessAttr *attr);
int ConfigureMigrationTargetsWithCapacityPolicy(ProcessAttr *attr, const ProcessTargetConfig *config,
                                                bool ignoreRemoteCapacity, bool skipRemoteResidencyCheck);
int GetProcessNumaMapsObservation(pid_t pid, bool hugeFlag, uint32_t *residentLocalMask, uint64_t numaPages[MAX_NODES]);
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
void CalcMigrateNrPagesPerPIDMuiltNuma(void);
void ClearRemoteMemUsed(void);
void CalRemoteMemUsed(void);

/* ========== manage_scan.c 导出给其他文件的内部函数 ========== */

void SetPidNrPages(ProcessAttr *attr, size_t *nrPages, int len);
void CalcActcStats(ProcessAttr *attr);
void DistributeActcData(ProcessAttr *attr, struct ProcessMemBitmap *pmb, ActcData *buf);
int RefreshManagedLocalTrackingScope(ProcessAttr *attr);
int BuildAllPidData(void);

/* ========== manage_grouped.c 导出给其他文件的内部函数 ========== */

void SetGroupedProcessConfig(ProcessAttr *attr, pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);

/* ========== manage_pair.c 导出给其他文件的内部函数 ========== */

/* (CalibratePairAccount 等已在 manage.h 中声明) */

#ifdef __cplusplus
}
#endif

#endif /* MANAGE_INTERNAL_H */
