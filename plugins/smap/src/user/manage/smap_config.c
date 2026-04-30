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

#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "securec.h"
#include "smap_user_log.h"
#include "manage.h"
#include "advanced-strategy/scene.h"
#include "smap_config.h"

#define NORMAL_ERR (-1)

/*
 * Functions to calculate config/payload length
 */

/* CalcNumaPayloadNum - Calculate numa config number */
static inline size_t CalcNumaPayloadNum(void)
{
    int nrLocalNuma = GetNrLocalNuma();
    return nrLocalNuma * REMOTE_NUMA_NUM + REMOTE_NUMA_NUM;
}

/* CalcNumaPayloadLen - Calculate numa payload length */
static inline size_t CalcNumaPayloadLen(void)
{
    return CONFIG_NUMA_LEN * CalcNumaPayloadNum();
}

/* CalcNumaConfigLen - Calculate numa config length */
static inline size_t CalcNumaConfigLen(void)
{
    return PAYLOAD_HEADER_LEN + CalcNumaPayloadLen();
}

/* CalcProcessConfigLen - Calculate process config length */
static inline size_t CalcProcessConfigLen(int nrProcess)
{
    return PAYLOAD_HEADER_LEN + (nrProcess * CONFIG_PROC_LEN);
}

static inline size_t CalcGroupProcessConfigLen(int nrProcess)
{
    return PAYLOAD_HEADER_LEN + (nrProcess * CONFIG_GROUP_PROC_LEN);
}

/* GetProcessConfigLen - Get process config length from process payload header */
static inline size_t GetProcessConfigLen(struct PayloadHeader *header)
{
    return PAYLOAD_HEADER_LEN + header->len;
}

static inline size_t CalcConfigLen(int nrProcess, int nrGroupProcess)
{
    return CONFIG_HEADER_LEN + CalcNumaConfigLen() + CalcProcessConfigLen(nrProcess) +
           CalcGroupProcessConfigLen(nrGroupProcess);
}

/* JumpToNumaConfig - jump to numa config from config base addr */
static inline char *JumpToNumaConfig(char *base)
{
    return base + CONFIG_HEADER_LEN;
}

/* JumpToNumaPayload - jump to numa payload from numa config base addr */
static inline char *JumpToNumaPayload(char *numaBase)
{
    return numaBase + PAYLOAD_HEADER_LEN;
}

/* JumpToProcessConfig - jump to process config from config base addr */
static inline char *JumpToProcessConfig(char *base)
{
    return JumpToNumaConfig(base) + CalcNumaConfigLen();
}

static inline char *JumpToGroupProcessConfig(char *processBase)
{
    return processBase + GetProcessConfigLen((struct PayloadHeader *)processBase);
}

/* JumpToProcessPayload - jump to process payload from process config base addr */
static inline char *JumpToProcessPayload(char *processBase)
{
    return processBase + PAYLOAD_HEADER_LEN;
}

static inline bool DoesConfigExist(void)
{
    return access(SMAP_CONFIG_PATH, R_OK | W_OK) == 0;
}

static inline int RemoveConfig(void)
{
    int ret = unlink(SMAP_CONFIG_PATH);
    if (ret && errno != ENOENT) {
        return -errno;
    }
    return 0;
}

static inline int OpenConfig(void)
{
    int fd = open(SMAP_CONFIG_PATH, O_CREAT | O_RDWR, SMAP_CONFIG_MODE);
    if (fd == NORMAL_ERR) {
        return -errno;
    }
    return fd;
}

/*
 * RemoveAndOpenConfig - remove (if needed) and open smap config
 *
 * @remove: remove old config before open
 *
 * Returns smap config file's fd, it's caller's responsibility to close it.
 */
static int RemoveAndOpenConfig(bool remove)
{
    int fd;

    if (remove) {
        fd = RemoveConfig();
        if (fd) {
            SMAP_LOGGER_ERROR("Remove old smap config failed: %d.", fd);
            return fd;
        }
    }
    fd = OpenConfig();
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
    }
    return fd;
}

static inline int TruncateConfig(int fd, size_t len)
{
    if (ftruncate(fd, len) == NORMAL_ERR) {
        return -errno;
    }
    return 0;
}

static inline char *MapConfig(int fd, size_t len, int prot, int flags)
{
    char *addr = mmap(NULL, len, prot, flags, fd, 0);
    if (addr == MAP_FAILED) {
        SMAP_LOGGER_ERROR("Mapping smap config failed: %d.", -errno);
        return NULL;
    }
    return addr;
}

static inline void UnmapConfig(char *addr, size_t len)
{
    if (munmap(addr, len) == NORMAL_ERR) {
        SMAP_LOGGER_ERROR("Unmapping smap config failed: %d.", -errno);
    }
}

static inline void WriteRunMode(char *addr, RunMode mode)
{
    struct SmapConfigHeader *header = (struct SmapConfigHeader *)addr;
    header->runMode = mode;
}

static inline void WriteHeader(char *addr, RunMode mode, size_t len)
{
    struct SmapConfigHeader *header = (struct SmapConfigHeader *)addr;
    header->ver = SMAP_CONFIG_VER;
    header->runMode = mode;
    header->headerLen = CONFIG_HEADER_LEN;
    header->totalLen = len;
}

static void WriteNumaConfig(char *base)
{
    struct ProcessManager *manager = GetProcessManager();
    struct RemoteNumaInfo *numaInfo = &manager->remoteNumaInfo;
    struct PayloadHeader *header = (struct PayloadHeader *)base;
    struct NumaPayload *payload;
    int nrLocalNuma = GetNrLocalNuma();

    header->len = CalcNumaPayloadLen();
    payload = (struct NumaPayload *)JumpToNumaPayload(base);

    for (int i = 0; i < nrLocalNuma; i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            payload->local = i;
            payload->remote = j + nrLocalNuma;
            payload->size = numaInfo->privateSize[i][j];
            payload++;
        }
    }
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        payload->size = numaInfo->sharedSize[i];
        payload++;
    }
}

static void WriteEmptyProcessConfigs(char *base)
{
    char *processBase = JumpToProcessConfig(base);
    struct PayloadHeader *processHeader = (struct PayloadHeader *)processBase;
    char *groupBase = JumpToGroupProcessConfig(processBase);
    struct PayloadHeader *groupHeader = (struct PayloadHeader *)groupBase;

    processHeader->len = 0;
    groupHeader->len = 0;
}

/*
 * InitSmapConfig - initialize smap config, including header and numa config
 *
 * @fd: smap config file descriptor
 */
static int InitSmapConfig(int fd)
{
    int ret;
    char *addr;
    size_t mapLen = CalcConfigLen(0, 0);
    RunMode runMode = GetRunMode();

    // Set config file length
    SMAP_LOGGER_INFO("Truncate smap config to %zu.", mapLen);
    ret = TruncateConfig(fd, mapLen);
    if (ret) {
        SMAP_LOGGER_ERROR("Truncate smap config to %zu failed: %d.", mapLen, ret);
        return ret;
    }

    addr = MapConfig(fd, mapLen, PROT_WRITE, MAP_SHARED);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }
    SMAP_LOGGER_INFO("Mmap smap config %zu done.", mapLen);

    // Initialize header
    WriteHeader(addr, runMode, mapLen);

    // Initialize NumaConfig
    WriteNumaConfig(JumpToNumaConfig(addr));
    WriteEmptyProcessConfigs(addr);

    UnmapConfig(addr, mapLen);
    return 0;
}

static inline bool IsRunModeValid(RunMode runMode)
{
    return runMode >= 0 && runMode < MAX_RUN_MODE;
}

static bool IsConfigHeaderValid(struct SmapConfigHeader *header)
{
    if (header->ver != SMAP_CONFIG_VER_V1 && header->ver != SMAP_CONFIG_VER_V2) {
        SMAP_LOGGER_ERROR("Wrong smap config header ver: %hd.", header->ver);
        return false;
    }
    if (!IsRunModeValid(header->runMode)) {
        SMAP_LOGGER_ERROR("Wrong smap config header run mode: %hd.", header->runMode);
        return false;
    }
    if (header->headerLen != CONFIG_HEADER_LEN) {
        SMAP_LOGGER_ERROR("Wrong smap config header len: %hd.", header->headerLen);
        return false;
    }
    uint32_t minLen = CONFIG_HEADER_LEN + CalcNumaConfigLen();
    if (header->ver == SMAP_CONFIG_VER_V2) {
        minLen += PAYLOAD_HEADER_LEN + PAYLOAD_HEADER_LEN;
    }
    if (header->totalLen < minLen) {
        SMAP_LOGGER_ERROR("Wrong smap config total len: %d.", header->totalLen);
        return false;
    }
    return true;
}

static inline void ReadHeader(char *addr, struct SmapConfigHeader *header)
{
    struct SmapConfigHeader *h = (struct SmapConfigHeader *)addr;
    header->ver = h->ver;
    header->runMode = h->runMode;
    header->headerLen = h->headerLen;
    header->totalLen = h->totalLen;
}

/*
 * ParseHeader - read smap config and fill in config header
 */
static int ParseHeader(int fd, struct SmapConfigHeader *header)
{
    char *addr;

    SMAP_LOGGER_DEBUG("Enter ParseHeader.");
    addr = MapConfig(fd, CONFIG_HEADER_LEN, PROT_READ, MAP_PRIVATE);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }

    ReadHeader(addr, header);
    if (!IsConfigHeaderValid(header)) {
        SMAP_LOGGER_ERROR("Wrong smap config header.");
        UnmapConfig(addr, CONFIG_HEADER_LEN);
        return -EINVAL;
    }

    UnmapConfig(addr, CONFIG_HEADER_LEN);
    SMAP_LOGGER_DEBUG("Exit ParseHeader.");
    return 0;
}

static void RecoverRunMode(RunMode runMode)
{
    SetRunMode(runMode);
    SetAdaptMem(runMode == WATERLINE_MODE);
}

/*
 * IsNumaConfigValid - check whether numa config is valid
 *
 * @numaBase: numa config base addr
 * @totalLen: smap config total length
 */
static bool IsNumaConfigValid(char *numaBase, size_t totalLen)
{
    struct PayloadHeader *header = (struct PayloadHeader *)numaBase;
    uint32_t expectedLen = CalcNumaPayloadLen();
    if (header->len != expectedLen) {
        SMAP_LOGGER_ERROR("Numa payload actualLen != expectedLen: %u != %u.", header->len, expectedLen);
        return false;
    }
    return (CONFIG_HEADER_LEN + CalcNumaConfigLen()) <= totalLen;
}

static int RecoverNumaConfig(char *numaBase)
{
    struct ProcessManager *manager = GetProcessManager();
    struct RemoteNumaInfo *numaInfo = &manager->remoteNumaInfo;
    struct NumaPayload *payload;

    SMAP_LOGGER_DEBUG("Enter RecoverNumaConfig.");
    payload = (struct NumaPayload *)JumpToNumaPayload(numaBase);
    for (int i = 0; i < GetNrLocalNuma(); i++) {
        for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
            // only called during initialization, no need to lock
            numaInfo->privateSize[i][j] = payload->size;
            numaInfo->privateUsedInfo[i][j].size = CalcRemoteBorrowPages(payload->size);
            SMAP_LOGGER_INFO("Numa privateSize[%d][%d] %u.", i, j, payload->size);
            payload++;
        }
    }
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        numaInfo->sharedSize[i] = payload->size;
        SMAP_LOGGER_INFO("Numa sharedSize[%d] %u.", i, payload->size);
        payload++;
    }
    for (int j = 0; j < REMOTE_NUMA_NUM; j++) {
        numaInfo->usedInfo[j].size = CalcRemoteBorrowPages(numaInfo->sharedSize[j]);
        for (int i = 0; i < GetNrLocalNuma(); i++) {
            numaInfo->usedInfo[j].size += CalcRemoteBorrowPages(numaInfo->privateSize[i][j]);
        }
        SMAP_LOGGER_INFO("Numa usedInfo[%d] %u.", j, numaInfo->usedInfo[j].size);
    }
    SMAP_LOGGER_DEBUG("Exit RecoverNumaConfig.");
    return 0;
}

static void WriteOneNumaConfig(char *numaBase, struct NumaPayload *np)
{
    struct NumaPayload *payload;
    int nrLocalNuma = GetNrLocalNuma();
    size_t offset;

    SMAP_LOGGER_DEBUG("Enter WriteOneNumaPayload.");
    payload = (struct NumaPayload *)JumpToNumaPayload(numaBase);

    // Calculate numa payload pointer offset according to local and remote numa
    if (np->local < 0) {
        offset = nrLocalNuma * REMOTE_NUMA_NUM + np->remote - nrLocalNuma;
    } else {
        offset = (np->local * REMOTE_NUMA_NUM) + (np->remote - nrLocalNuma);
    }
    SMAP_LOGGER_DEBUG("Numa payload pointer offset: %zu.", offset);
    payload += offset;

    payload->size = np->size;
    SMAP_LOGGER_DEBUG("Exit WriteOneNumaPayload.");
}

static void AssignProcessAttr(ProcessAttr *attr, struct ProcessPayload *payload)
{
    attr->pid = payload->pid;
    attr->numaAttr.numaNodes = payload->numaNodes;
    attr->remoteNumaCnt = payload->count;
    int nrLocalNuma = GetNrLocalNuma();
    int totalRatio = 0;
    for (int i = 0; i < payload->count; i++) {
        attr->migrateParam[i].nid = payload->migrateParam[i].nid;
        attr->migrateParam[i].memSize = payload->migrateParam[i].memSize;
        for (int j = 0; j < GetNrLocalNuma(); j++) {
            if (EqualToAttrL1(attr, j)) {
                int l2Index = payload->migrateParam[i].nid - nrLocalNuma;
                attr->strategyAttr.initRemoteMemRatio[j][l2Index] = payload->migrateParam[i].ratio;
                attr->strategyAttr.memSize[j][l2Index] = payload->migrateParam[i].memSize;
            }
        }
        totalRatio += payload->migrateParam[i].ratio;
    }

    attr->initLocalMemRatio = HUNDRED - totalRatio;
    attr->type = payload->type;
    attr->state = payload->state;
    attr->scanType = payload->scanType;
    attr->scanTime = payload->scanTime;
    attr->migrateMode = payload->migrateMode;
    attr->duration = payload->duration;
    attr->enableSwap = true;
    if (time(&attr->scanStart) == (time_t)-1) {
        SMAP_LOGGER_ERROR("get time error.");
    }
}

/*
 * IsProcessConfigValid - check whether process config is valid
 *
 * @processBase: process config processBase addr
 * @totalLen: smap config total length
 */
static bool IsProcessConfigValid(char *processBase, size_t totalLen)
{
    struct PayloadHeader *header = (struct PayloadHeader *)processBase;
    size_t processConfigLen;

    if (header->len % CONFIG_PROC_LEN != 0) {
        SMAP_LOGGER_ERROR("Process config has wrong length: %u.", header->len);
        return false;
    }
    processConfigLen = GetProcessConfigLen(header);
    return (CONFIG_HEADER_LEN + CalcNumaConfigLen() + processConfigLen) <= totalLen;
}

static bool IsGroupProcessConfigValid(char *groupBase, char *addr, size_t totalLen)
{
    struct PayloadHeader *header = (struct PayloadHeader *)groupBase;
    size_t groupOffset = groupBase - addr;
    size_t groupConfigLen;

    if (header->len % CONFIG_GROUP_PROC_LEN != 0) {
        SMAP_LOGGER_ERROR("Group process config has wrong length: %u.", header->len);
        return false;
    }
    groupConfigLen = GetProcessConfigLen(header);
    return groupOffset + groupConfigLen == totalLen;
}

static inline bool HasProcessConfig(size_t totalLen)
{
    SMAP_LOGGER_DEBUG("HasProcessConfig, totalLen %zu.", totalLen);
    SMAP_LOGGER_DEBUG("HasProcessConfig, header + numa config %zu.", CONFIG_HEADER_LEN + CalcNumaConfigLen());
    return totalLen > CONFIG_HEADER_LEN + CalcNumaConfigLen();
}

static int RecoverProcessConfig(char *processBase)
{
    struct PayloadHeader *header = (struct PayloadHeader *)processBase;
    struct ProcessManager *manager = GetProcessManager();
    struct ProcessPayload *payload;
    ProcessAttr *attr;
    uint32_t nrPayload;
    bool errFlag = false;

    SMAP_LOGGER_DEBUG("Enter RecoverProcessConfig.");
    nrPayload = header->len / CONFIG_PROC_LEN;
    payload = (struct ProcessPayload *)JumpToProcessPayload(processBase);
    for (uint32_t i = 0; i < nrPayload; i++, payload++) {
        if (payload->type != GetPidType(manager)) {
            SMAP_LOGGER_WARNING("Pid %d type %d is different from %d.", payload->pid, payload->type,
                                GetPidType(manager));
            continue;
        }
        attr = calloc(1, sizeof(ProcessAttr));
        if (!attr) {
            SMAP_LOGGER_ERROR("Malloc for process %d failed.", payload->pid);
            errFlag = true;
            break;
        }
        AssignProcessAttr(attr, payload);
        SMAP_LOGGER_INFO(
            "ProcessPayload %d, type %hu, numaNodes %#x, state %hu, scan type %hu, scan time %u.",
            payload->pid, payload->type, payload->numaNodes, payload->state, payload->scanType,
            payload->scanTime);
        for (int j = 0; j < payload->count; j++) {
            SMAP_LOGGER_INFO(
                "ProcessPayload %d, destNid %d, ratio %d, memsize %llu.", payload->pid,
                payload->migrateParam[j].nid, payload->migrateParam[j].ratio, payload->migrateParam[j].memSize);
        }
        LinkedListAdd(&manager->processes, &attr);
        manager->nr[attr->type]++;
    }
    if (errFlag) {
        attr = manager->processes;
        while (attr) {
            LinkedListRemove(&attr, &manager->processes);
            attr = manager->processes;
        }
        manager->nr[VM_TYPE] = manager->nr[PROCESS_TYPE] = 0;
        return -ENOMEM;
    }
    SMAP_LOGGER_DEBUG("Exit RecoverProcessConfig.");
    return 0;
}

static void ClearRecoveredProcesses(struct ProcessManager *manager)
{
    ProcessAttr *attr = manager->processes;

    while (attr) {
        LinkedListRemove(&attr, &manager->processes);
        attr = manager->processes;
    }
    manager->nr[VM_TYPE] = 0;
    manager->nr[PROCESS_TYPE] = 0;
}

static bool HasRecoveredProcessPid(struct ProcessManager *manager, pid_t pid)
{
    for (ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (attr->pid == pid) {
            return true;
        }
    }
    return false;
}

static bool HasDuplicateConfigInt(const int *arr, int count)
{
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (arr[i] == arr[j]) {
                return true;
            }
        }
    }
    return false;
}

static inline bool IsRecoveredGroupTargetValid(int nid, int nrLocalNuma)
{
    return nid >= nrLocalNuma && nid < nrLocalNuma + REMOTE_NUMA_NUM && nid < MAX_NODES;
}

static bool IsGroupProcessPayloadValid(struct GroupProcessPayload *payload)
{
    bool localUsed[MAX_NODES] = { 0 };

    if (payload->groupCount <= 0 || payload->groupCount > MAX_MIGRATION_GROUP_NUM) {
        SMAP_LOGGER_WARNING("grouped pid %d group count %d invalid.", payload->pid, payload->groupCount);
        return false;
    }

    int nrLocalNuma = GetNrLocalNuma();
    for (int i = 0; i < payload->groupCount; i++) {
        struct GroupPayload *group = &payload->groups[i];
        if (group->localCount <= 0 || group->localCount > MAX_GROUP_LOCAL_NUMA ||
            group->targetCount <= 0 || group->targetCount > MAX_GROUP_REMOTE_NUMA) {
            SMAP_LOGGER_WARNING("grouped pid %d group %d count invalid.", payload->pid, i);
            return false;
        }
        int localNids[MAX_GROUP_LOCAL_NUMA] = { 0 };
        for (int j = 0; j < group->localCount; j++) {
            int nid = group->locals[j].nid;
            if (group->locals[j].localReservePages == 0) {
                SMAP_LOGGER_WARNING("grouped pid %d group %d local %d reserve is zero.", payload->pid, i, nid);
                return false;
            }
            if (nid < 0 || nid >= nrLocalNuma || nid >= MAX_NODES) {
                SMAP_LOGGER_WARNING("grouped pid %d group %d local nid %d invalid.", payload->pid, i, nid);
                return false;
            }
            localNids[j] = nid;
            if (localUsed[nid]) {
                SMAP_LOGGER_WARNING("grouped pid %d local nid %d is used by another group.", payload->pid, nid);
                return false;
            }
            localUsed[nid] = true;
        }
        if (HasDuplicateConfigInt(localNids, group->localCount)) {
            SMAP_LOGGER_WARNING("grouped pid %d group %d local nids duplicate.", payload->pid, i);
            return false;
        }

        int targets[MAX_GROUP_REMOTE_NUMA] = { 0 };
        for (int j = 0; j < group->targetCount; j++) {
            int nid = group->targets[j].nid;
            if (!IsRecoveredGroupTargetValid(nid, nrLocalNuma) || group->targets[j].quotaPages == 0) {
                SMAP_LOGGER_WARNING("grouped pid %d group %d target nid %d invalid.", payload->pid, i, nid);
                return false;
            }
            targets[j] = nid;
        }
        if (HasDuplicateConfigInt(targets, group->targetCount)) {
            SMAP_LOGGER_WARNING("grouped pid %d group %d target nids duplicate.", payload->pid, i);
            return false;
        }
    }
    return true;
}

static int BuildGroupPolicyFromPayload(struct GroupProcessPayload *payload, GroupMigrationPolicy *policy)
{
    uint64_t numaPages[MAX_NODES] = { 0 };

    if (!IsGroupProcessPayloadValid(payload)) {
        return -EINVAL;
    }

    policy->enabled = true;
    policy->groupCount = payload->groupCount;
    for (int i = 0; i < payload->groupCount; i++) {
        struct GroupPayload *group = &payload->groups[i];
        MigrationGroupAttr *attr = &policy->groups[i];
        attr->localCount = group->localCount;
        attr->targetCount = group->targetCount;
        for (int j = 0; j < group->localCount; j++) {
            attr->locals[j].nid = group->locals[j].nid;
            attr->locals[j].localReservePages = group->locals[j].localReservePages;
        }
        for (int j = 0; j < group->targetCount; j++) {
            attr->targets[j].nid = group->targets[j].nid;
            attr->targets[j].quotaPages = group->targets[j].quotaPages;
            attr->targets[j].usedPages = 0;
        }
    }

    int ret = GetPidNumaPagesFromNumaMaps(payload->pid, numaPages);
    if (ret) {
        SMAP_LOGGER_WARNING("Get grouped pid %d numa pages from config failed: %d.", payload->pid, ret);
        return ret;
    }
    return InitGroupedUsedPages(payload->pid, policy, numaPages);
}

static void AssignGroupProcessAttr(ProcessAttr *attr, struct GroupProcessPayload *payload,
                                   GroupMigrationPolicy *policy)
{
    attr->pid = payload->pid;
    attr->scanType = payload->scanType;
    attr->type = payload->type;
    attr->state = payload->state;
    attr->migrateMode = payload->migrateMode;
    attr->numaAttr.numaNodes = payload->numaNodes;
    attr->scanTime = payload->scanTime;
    attr->duration = payload->duration;
    attr->remoteNumaCnt = GetL2Count(payload->numaNodes);
    attr->enableSwap = true;
    attr->initLocalMemRatio = HUNDRED;
    attr->groupPolicy = *policy;
    if (time(&attr->scanStart) == (time_t)-1) {
        SMAP_LOGGER_ERROR("get time error.");
    }
}

static int RecoverGroupProcessConfig(char *groupBase)
{
    struct PayloadHeader *header = (struct PayloadHeader *)groupBase;
    struct ProcessManager *manager = GetProcessManager();
    struct GroupProcessPayload *payload;
    uint32_t nrPayload = header->len / CONFIG_GROUP_PROC_LEN;
    bool errFlag = false;

    SMAP_LOGGER_DEBUG("Enter RecoverGroupProcessConfig.");
    payload = (struct GroupProcessPayload *)(groupBase + PAYLOAD_HEADER_LEN);
    for (uint32_t i = 0; i < nrPayload; i++, payload++) {
        if (payload->type != GetPidType(manager)) {
            SMAP_LOGGER_WARNING("Grouped pid %d type %d is different from %d.", payload->pid, payload->type,
                                GetPidType(manager));
            continue;
        }
        if (!PidIsValid(payload->pid)) {
            SMAP_LOGGER_WARNING("Grouped pid %d from config doesn't exist.", payload->pid);
            continue;
        }
        if (HasRecoveredProcessPid(manager, payload->pid)) {
            SMAP_LOGGER_WARNING("Grouped pid %d from config is duplicated.", payload->pid);
            continue;
        }

        GroupMigrationPolicy policy = { 0 };
        int ret = BuildGroupPolicyFromPayload(payload, &policy);
        if (ret) {
            SMAP_LOGGER_WARNING("Recover grouped pid %d policy failed: %d.", payload->pid, ret);
            continue;
        }

        ProcessAttr *attr = calloc(1, sizeof(ProcessAttr));
        if (!attr) {
            SMAP_LOGGER_ERROR("Malloc for grouped process %d failed.", payload->pid);
            errFlag = true;
            break;
        }
        AssignGroupProcessAttr(attr, payload, &policy);
        LinkedListAdd(&manager->processes, &attr);
        manager->nr[attr->type]++;
        SMAP_LOGGER_INFO("Recover grouped pid %d success, group count %d.", payload->pid, payload->groupCount);
    }
    if (errFlag) {
        ClearRecoveredProcesses(manager);
        return -ENOMEM;
    }
    SMAP_LOGGER_DEBUG("Exit RecoverGroupProcessConfig.");
    return 0;
}

static int WriteProcessConfig(char *processBase, struct ProcessPayload *p, int nrPayload)
{
    int ret;
    struct PayloadHeader *header = (struct PayloadHeader *)processBase;
    struct ProcessPayload *payload;

    SMAP_LOGGER_DEBUG("Enter WriteProcessConfig.");
    header->len = CONFIG_PROC_LEN * nrPayload;
    if (header->len == 0) {
        return 0;
    }
    payload = (struct ProcessPayload *)JumpToProcessPayload(processBase);
    // memcpy_s return value is positive, so we use -ret
    ret = memcpy_s(payload, header->len, p, header->len);
    if (ret) {
        SMAP_LOGGER_ERROR("WriteProcessConfig memcpy_s failed: %d.", -ret);
    }
    return -ret;
}

static int WriteGroupProcessConfig(char *groupBase, struct GroupProcessPayload *p, int nrPayload)
{
    int ret;
    struct PayloadHeader *header = (struct PayloadHeader *)groupBase;
    struct GroupProcessPayload *payload;

    SMAP_LOGGER_DEBUG("Enter WriteGroupProcessConfig.");
    header->len = CONFIG_GROUP_PROC_LEN * nrPayload;
    if (header->len == 0) {
        return 0;
    }
    payload = (struct GroupProcessPayload *)(groupBase + PAYLOAD_HEADER_LEN);
    ret = memcpy_s(payload, header->len, p, header->len);
    if (ret) {
        SMAP_LOGGER_ERROR("WriteGroupProcessConfig memcpy_s failed: %d.", -ret);
    }
    return -ret;
}

static uint8_t GetAttrRemoteMemRatio(ProcessAttr *attr, int nid)
{
    int nrLocalNuma = GetNrLocalNuma();
    int l1Node = GetAttrL1(attr);
    for (int i = 0; i < REMOTE_NUMA_NUM; i++) {
        if (nid == i + nrLocalNuma) {
            return (uint8_t)attr->strategyAttr.initRemoteMemRatio[l1Node][i];
        }
    }
    return 0;
}

static int BuildAllProcessPayload(struct ProcessPayload **payload, int *len)
{
    struct ProcessManager *manager = GetProcessManager();
    struct ProcessPayload *p, *tmp;
    int nrPayload = 0;

    for (ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (!attr->groupPolicy.enabled || attr->groupPolicy.groupCount <= 0) {
            nrPayload++;
        }
    }

    SMAP_LOGGER_DEBUG("BuildAllProcessPayload nrPayload %d.", nrPayload);
    if (nrPayload == 0) {
        *payload = NULL;
        *len = 0;
        SMAP_LOGGER_ERROR("Number of process payload is 0.");
        return 0;
    }

    p = malloc(nrPayload * CONFIG_PROC_LEN);
    if (!p) {
        SMAP_LOGGER_ERROR("BuildAllProcessPayload calloc failed.");
        return -ENOMEM;
    }

    tmp = p;
    for (ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (attr->groupPolicy.enabled && attr->groupPolicy.groupCount > 0) {
            continue;
        }
        tmp->pid = attr->pid;
        tmp->scanType = attr->scanType;
        tmp->type = attr->type;
        tmp->state = attr->state;
        tmp->migrateMode = attr->migrateMode;
        tmp->numaNodes = attr->numaAttr.numaNodes;
        tmp->scanTime = attr->scanTime;
        tmp->duration = attr->duration;
        tmp->count = attr->remoteNumaCnt;
        for (int i = 0; i < tmp->count; i++) {
            tmp->migrateParam[i].nid = attr->migrateParam[i].nid;
            tmp->migrateParam[i].memSize = attr->migrateParam[i].memSize;
            tmp->migrateParam[i].ratio = GetAttrRemoteMemRatio(attr, tmp->migrateParam[i].nid);
        }
        tmp++;
    }

    *payload = p;
    *len = nrPayload;

    return 0;
}

static void AssignGroupProcessPayload(struct GroupProcessPayload *payload, ProcessAttr *attr)
{
    payload->pid = attr->pid;
    payload->scanType = attr->scanType;
    payload->type = attr->type;
    payload->state = attr->state;
    payload->migrateMode = attr->migrateMode;
    payload->numaNodes = attr->numaAttr.numaNodes;
    payload->scanTime = attr->scanTime;
    payload->duration = attr->duration;
    payload->groupCount = attr->groupPolicy.groupCount;
    for (int i = 0; i < attr->groupPolicy.groupCount; i++) {
        MigrationGroupAttr *group = &attr->groupPolicy.groups[i];
        struct GroupPayload *groupPayload = &payload->groups[i];
        groupPayload->localCount = group->localCount;
        groupPayload->targetCount = group->targetCount;
        for (int j = 0; j < group->localCount; j++) {
            groupPayload->locals[j].nid = group->locals[j].nid;
            groupPayload->locals[j].localReservePages = group->locals[j].localReservePages;
        }
        for (int j = 0; j < group->targetCount; j++) {
            groupPayload->targets[j].nid = group->targets[j].nid;
            groupPayload->targets[j].quotaPages = group->targets[j].quotaPages;
        }
    }
}

static int BuildAllGroupProcessPayload(struct GroupProcessPayload **payload, int *len)
{
    struct ProcessManager *manager = GetProcessManager();
    struct GroupProcessPayload *p, *tmp;
    int nrPayload = 0;

    for (ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (attr->groupPolicy.enabled && attr->groupPolicy.groupCount > 0) {
            nrPayload++;
        }
    }

    SMAP_LOGGER_DEBUG("BuildAllGroupProcessPayload nrPayload %d.", nrPayload);
    if (nrPayload == 0) {
        *payload = NULL;
        *len = 0;
        return 0;
    }

    p = calloc(nrPayload, CONFIG_GROUP_PROC_LEN);
    if (!p) {
        SMAP_LOGGER_ERROR("BuildAllGroupProcessPayload calloc failed.");
        return -ENOMEM;
    }

    tmp = p;
    for (ProcessAttr *attr = manager->processes; attr; attr = attr->next) {
        if (!attr->groupPolicy.enabled || attr->groupPolicy.groupCount <= 0) {
            continue;
        }
        AssignGroupProcessPayload(tmp, attr);
        tmp++;
    }

    *payload = p;
    *len = nrPayload;
    return 0;
}

static int MapAndWriteProcessConfig(int fd, size_t mapLen, struct ProcessPayload *payload, int nrPayload,
                                    struct GroupProcessPayload *groupPayload, int nrGroupPayload)
{
    int ret;
    char *addr;
    char *processBase;
    char *groupBase;
    RunMode runMode = GetRunMode();

    addr = MapConfig(fd, mapLen, PROT_WRITE, MAP_SHARED);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }

    processBase = JumpToProcessConfig(addr);
    ret = WriteProcessConfig(processBase, payload, nrPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Write process config failed: %d.", ret);
        UnmapConfig(addr, mapLen);
        return ret;
    }
    groupBase = JumpToGroupProcessConfig(processBase);
    ret = WriteGroupProcessConfig(groupBase, groupPayload, nrGroupPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Write group process config failed: %d.", ret);
        UnmapConfig(addr, mapLen);
        return ret;
    }
    SMAP_LOGGER_INFO("All process config written.");

    WriteHeader(addr, runMode, mapLen);
    UnmapConfig(addr, mapLen);
    return 0;
}

static int ChangeProcessConfig(int fd)
{
    int ret, err;
    int nrPayload;
    size_t oldConfigLen, newConfigLen;
    struct ProcessPayload *payload = NULL;
    struct GroupProcessPayload *groupPayload = NULL;
    struct SmapConfigHeader header;
    int nrGroupPayload;

    SMAP_LOGGER_DEBUG("Enter ReadConfig.");
    ret = ParseHeader(fd, &header);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse smap config header failed: %d.", ret);
        return ret;
    }
    oldConfigLen = header.totalLen;

    ret = BuildAllProcessPayload(&payload, &nrPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Build all process payload failed: %d.", ret);
        return ret;
    }
    ret = BuildAllGroupProcessPayload(&groupPayload, &nrGroupPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Build all group process payload failed: %d.", ret);
        free(payload);
        return ret;
    }

    newConfigLen = CalcConfigLen(nrPayload, nrGroupPayload);
    SMAP_LOGGER_DEBUG("New config length with %d processes: %d.", nrPayload, newConfigLen);

    // Extend config before writing process config
    if (newConfigLen > oldConfigLen) {
        SMAP_LOGGER_INFO("New len > old len, %zu > %zu, extend file.", newConfigLen, oldConfigLen);
        ret = TruncateConfig(fd, newConfigLen);
        if (ret) {
            SMAP_LOGGER_ERROR("Extend smap config to %zu failed: %d.", newConfigLen, ret);
            free(payload);
            free(groupPayload);
            return ret;
        }
    }

    ret = MapAndWriteProcessConfig(fd, newConfigLen, payload, nrPayload, groupPayload, nrGroupPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Map and write process config failed: %d.", ret);
        free(payload);
        free(groupPayload);
        return ret;
    }

    // Shrink config file after process config has been written successfully
    if (newConfigLen < oldConfigLen) {
        SMAP_LOGGER_INFO("New len < old len, %zu < %zu, shrink file.", newConfigLen, oldConfigLen);
        err = TruncateConfig(fd, newConfigLen);
        if (err) {
            SMAP_LOGGER_WARNING("Shrink smap config to %zu failed: %d.", newConfigLen, err);
        }
    }

    free(payload);
    free(groupPayload);
    return ret;
}

static int ParseConfig(int fd, struct SmapConfigHeader *header)
{
    int ret;
    char *addr;
    char *numaBase;
    char *processBase;
    char *groupBase;
    size_t mapLen = header->totalLen;

    SMAP_LOGGER_DEBUG("Enter ParseConfig.");
    addr = MapConfig(fd, mapLen, PROT_READ, MAP_PRIVATE);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }

    if (!IsRunModeValid(header->runMode)) {
        SMAP_LOGGER_ERROR("Detected invalid run mode: %d.", header->runMode);
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }
    SMAP_LOGGER_INFO("Detected run mode: %d.", header->runMode);
    RecoverRunMode(header->runMode);

    // Parse NumaPayload
    numaBase = JumpToNumaConfig(addr);
    if (!IsNumaConfigValid(numaBase, mapLen)) {
        SMAP_LOGGER_ERROR("Detected invalid numa config.");
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }
    ret = RecoverNumaConfig(numaBase);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse numa config failed: %d.", ret);
        UnmapConfig(addr, mapLen);
        return ret;
    }

    if (header->ver == SMAP_CONFIG_VER_V1) {
        // Parse v1 ProcessConfig
        if (!HasProcessConfig(mapLen)) {
            UnmapConfig(addr, mapLen);
            SMAP_LOGGER_DEBUG("Exit ParseConfig.");
            return 0;
        }
        processBase = JumpToProcessConfig(addr);
        if (!IsProcessConfigValid(processBase, mapLen)) {
            SMAP_LOGGER_ERROR("Detected invalid process config.");
            UnmapConfig(addr, mapLen);
            return -EINVAL;
        }
        ret = RecoverProcessConfig(processBase);
        if (ret) {
            SMAP_LOGGER_ERROR("Parse process config failed: %d.", ret);
            UnmapConfig(addr, mapLen);
            return ret;
        }
        UnmapConfig(addr, mapLen);
        SMAP_LOGGER_DEBUG("Exit ParseConfig.");
        return 0;
    }

    // Parse v2 ProcessConfig and GroupProcessConfig
    processBase = JumpToProcessConfig(addr);
    if (!IsProcessConfigValid(processBase, mapLen)) {
        SMAP_LOGGER_ERROR("Detected invalid process config.");
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }
    ret = RecoverProcessConfig(processBase);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse process config failed: %d.", ret);
        UnmapConfig(addr, mapLen);
        return ret;
    }

    groupBase = JumpToGroupProcessConfig(processBase);
    if ((size_t)(groupBase - addr) + PAYLOAD_HEADER_LEN > mapLen) {
        SMAP_LOGGER_ERROR("Detected missing group process config.");
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }
    if (!IsGroupProcessConfigValid(groupBase, addr, mapLen)) {
        SMAP_LOGGER_ERROR("Detected invalid group process config.");
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }
    ret = RecoverGroupProcessConfig(groupBase);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse group process config failed: %d.", ret);
        UnmapConfig(addr, mapLen);
        return ret;
    }

    UnmapConfig(addr, mapLen);
    SMAP_LOGGER_DEBUG("Exit ParseConfig.");
    return 0;
}

static int ReadConfig(int fd)
{
    int ret;
    struct SmapConfigHeader header;

    SMAP_LOGGER_DEBUG("Enter ReadConfig.");
    ret = ParseHeader(fd, &header);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse smap config header failed: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_INFO("ver: %hu, header len: %hu, total len: %u.", header.ver, header.headerLen, header.totalLen);

    ret = ParseConfig(fd, &header);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse smap config payload failed: %d.", ret);
        return ret;
    }
    SMAP_LOGGER_DEBUG("Exit ReadConfig.");
    return 0;
}

static int ChangeRunMode(int fd, RunMode runMode)
{
    int ret;
    char *addr;
    char *numaBase;
    size_t mapLen = CONFIG_HEADER_LEN;
    struct SmapConfigHeader header;

    SMAP_LOGGER_DEBUG("Enter ChangeRunMode.");
    addr = MapConfig(fd, mapLen, PROT_WRITE, MAP_SHARED);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }

    WriteRunMode(addr, runMode);

    UnmapConfig(addr, mapLen);
    SMAP_LOGGER_DEBUG("Exit ChangeRunMode.");
    return 0;
}

static int ChangeOneNumaConfig(int fd, struct NumaPayload *payload)
{
    int ret;
    char *addr;
    char *numaBase;
    size_t mapLen;
    struct SmapConfigHeader header;

    SMAP_LOGGER_DEBUG("Enter ChangeOneNumaConfig.");
    ret = ParseHeader(fd, &header);
    if (ret) {
        SMAP_LOGGER_ERROR("Parse smap config header failed: %d.", ret);
        return ret;
    }

    mapLen = header.totalLen;
    addr = MapConfig(fd, mapLen, PROT_WRITE, MAP_SHARED);
    if (!addr) {
        SMAP_LOGGER_ERROR("Map smap config failed.");
        return -EBADF;
    }
    numaBase = JumpToNumaConfig(addr);
    if (!IsNumaConfigValid(numaBase, mapLen)) {
        SMAP_LOGGER_ERROR("Detected invalid numa config.");
        UnmapConfig(addr, mapLen);
        return -EINVAL;
    }

    WriteOneNumaConfig(numaBase, payload);

    UnmapConfig(addr, mapLen);
    SMAP_LOGGER_DEBUG("Exit ChangeOneNumaConfig.");
    return 0;
}

int RecoverFromConfig(void)
{
    int ret;
    int fd;
    bool exist;

    exist = DoesConfigExist();
    SMAP_LOGGER_DEBUG("Check smap config existence: %d.", exist);

    fd = RemoveAndOpenConfig(false);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
        return fd;
    }

    if (exist) {
        ret = ReadConfig(fd);
        close(fd);
        if (ret == 0) {
            SMAP_LOGGER_INFO("Read config done.");
            return 0;
        }

        // Create a new smap config if read is failed
        SMAP_LOGGER_ERROR("Read config error: %d.", ret);
        fd = RemoveAndOpenConfig(true);
        if (fd < 0) {
            SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
            return fd;
        }
    }

    ret = InitSmapConfig(fd);
    if (ret) {
        SMAP_LOGGER_ERROR("Init smap config error: %d.", ret);
    } else {
        SMAP_LOGGER_INFO("Init smap config done.");
    }
    close(fd);
    return ret;
}

int SyncRunMode(RunMode runMode)
{
    int ret;
    int fd;
    int nrLocalNuma = GetNrLocalNuma();
    bool exist;

    if (!IsRunModeValid(runMode)) {
        SMAP_LOGGER_ERROR("SyncRunMode runMode %d is invalid.", runMode);
        return -EINVAL;
    }

    exist = DoesConfigExist();
    SMAP_LOGGER_DEBUG("Check smap config existence: %d.", exist);

    SMAP_LOGGER_INFO("Sync run mode to %d.", runMode);
    fd = RemoveAndOpenConfig(false);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
        return fd;
    }

    if (!exist) {
        ret = InitSmapConfig(fd);
        if (ret) {
            SMAP_LOGGER_ERROR("Init smap config error: %d.", ret);
            close(fd);
            return ret;
        }
        SMAP_LOGGER_INFO("Init smap config done.");
    }

    ret = ChangeRunMode(fd, runMode);
    if (ret) {
        SMAP_LOGGER_ERROR("Change run mode failed: %d.", ret);
    } else {
        SMAP_LOGGER_INFO("Change run mode done.");
    }
    close(fd);
    return ret;
}

int SyncOneNumaConfig(int local, int remote, size_t size)
{
    int ret;
    int fd;
    int nrLocalNuma = GetNrLocalNuma();
    bool exist;
    struct NumaPayload config = { local, remote, size };

    if (local >= nrLocalNuma) {
        SMAP_LOGGER_ERROR("SyncOneNumaConfig local %d is invalid.", local);
        return -EINVAL;
    }
    if (remote < nrLocalNuma || remote >= nrLocalNuma + REMOTE_NUMA_NUM) {
        SMAP_LOGGER_ERROR("SyncOneNumaConfig remote %d is invalid.", remote);
        return -EINVAL;
    }

    exist = DoesConfigExist();
    SMAP_LOGGER_DEBUG("Check smap config existence: %d.", exist);

    SMAP_LOGGER_INFO("Synchronize numa config %d-%d to %zu.", local, remote, size);
    fd = RemoveAndOpenConfig(false);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
        return fd;
    }

    if (!exist) {
        ret = InitSmapConfig(fd);
        if (ret) {
            SMAP_LOGGER_ERROR("Init smap config error: %d.", ret);
            close(fd);
            return ret;
        }
        SMAP_LOGGER_INFO("Init smap config done.");
    }

    ret = ChangeOneNumaConfig(fd, &config);
    if (ret) {
        SMAP_LOGGER_ERROR("Change one numa config failed: %d.", ret);
    } else {
        SMAP_LOGGER_INFO("Change one numa config done.");
    }
    close(fd);
    return ret;
}

int SyncAllProcessConfig(void)
{
    int ret;
    int fd;
    bool exist;

    exist = DoesConfigExist();
    SMAP_LOGGER_DEBUG("Check smap config existence: %d.", exist);

    SMAP_LOGGER_INFO("Synchronize process config begin.");
    fd = RemoveAndOpenConfig(false);
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open smap config failed: %d.", fd);
        return fd;
    }

    if (!exist) {
        ret = InitSmapConfig(fd);
        if (ret) {
            SMAP_LOGGER_ERROR("Init smap config error: %d.", ret);
            close(fd);
            return ret;
        }
        SMAP_LOGGER_INFO("Init smap config done.");
    }

    ret = ChangeProcessConfig(fd);
    if (ret) {
        SMAP_LOGGER_ERROR("Change process config failed: %d.", ret);
    } else {
        SMAP_LOGGER_INFO("Change process config done.");
    }
    close(fd);
    return ret;
}
