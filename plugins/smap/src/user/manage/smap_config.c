/*
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

/* GetProcessConfigLen - Get process config length from process payload header */
static inline size_t GetProcessConfigLen(struct PayloadHeader *header)
{
    return PAYLOAD_HEADER_LEN + header->len;
}

static inline size_t CalcConfigLen(int nrProcess)
{
    return CONFIG_HEADER_LEN + CalcNumaConfigLen() + CalcProcessConfigLen(nrProcess);
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

    processHeader->len = 0;
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
    size_t mapLen = CalcConfigLen(0);
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
    if (header->ver != SMAP_CONFIG_VER) {
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
            numaInfo->privateUsedInfo[i][j].size = MBToPage(payload->size);
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
        numaInfo->usedInfo[j].size = MBToPage(numaInfo->sharedSize[j]);
        for (int i = 0; i < GetNrLocalNuma(); i++) {
            numaInfo->usedInfo[j].size += MBToPage(numaInfo->privateSize[i][j]);
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

static int RestoreProcessTargetConfig(ProcessTargetConfig *config, uint8_t migrateMode, int count,
                                      const struct PersistRemoteTarget targets[])
{
    if (!config || !targets || count < 0 || count > REMOTE_NUMA_NUM ||
        (migrateMode != MIG_RATIO_MODE && migrateMode != MIG_MEMSIZE_MODE)) {
        return -EINVAL;
    }

    InitProcessTargetConfig(config);
    config->migrateMode = (MigrateMode)migrateMode;
    uint64_t totalRatio = 0;
    uint64_t pageSizeKB = GetPageSize() / KIB;
    bool remoteSeen[REMOTE_NUMA_NUM] = { false };
    for (int i = 0; i < count; i++) {
        int remoteIndex;
        const struct PersistRemoteTarget *target = &targets[i];
        bool valid = RemoteNidToIndex(target->nid, GetNrLocalNuma(), &remoteIndex) == 0 && !remoteSeen[remoteIndex];
        if (valid && migrateMode == MIG_RATIO_MODE) {
            valid = target->ratio <= HUNDRED && totalRatio + target->ratio <= HUNDRED;
        }
        if (valid && migrateMode == MIG_MEMSIZE_MODE) {
            valid = pageSizeKB != 0 && target->memSize % pageSizeKB == 0 && target->memSize / pageSizeKB <= UINT32_MAX;
        }
        if (!valid) {
            SMAP_LOGGER_ERROR("Invalid persisted V1 target nid %d.", target->nid);
            return -EINVAL;
        }

        remoteSeen[remoteIndex] = true;
        config->targets[config->count++] = (ProcessRemoteTarget){
            .remoteNid = target->nid,
            .ratio = target->ratio,
            .memSizeKB = target->memSize,
        };
        totalRatio += target->ratio;
    }
    return 0;
}

static void ApplyRecoveredTargetConfig(ProcessAttr *attr, const ProcessTargetConfig *config)
{
    int totalRatio = 0;
    attr->targetConfig = *config;
    attr->migrateMode = config->migrateMode;
    attr->remoteNumaCnt = config->count;
    for (uint32_t i = 0; i < config->count; i++) {
        int remoteIndex = config->targets[i].remoteNid - GetNrLocalNuma();
        attr->migrateParam[i].nid = config->targets[i].remoteNid;
        attr->migrateParam[i].memSize = config->targets[i].memSizeKB;
        bool firstL1 = true;
        for (int localNid = 0; localNid < GetNrLocalNuma(); localNid++) {
            if (!InAttrL1(attr, localNid)) {
                continue;
            }
            attr->strategyAttr.initRemoteMemRatio[localNid][remoteIndex] = config->targets[i].ratio;
            attr->strategyAttr.memSize[localNid][remoteIndex] = firstL1 ? config->targets[i].memSizeKB : 0;
            firstL1 = false;
        }
        AddAttrL2(attr, config->targets[i].remoteNid);
        totalRatio += config->targets[i].ratio;
    }
    attr->initLocalMemRatio = config->migrateMode == MIG_RATIO_MODE ? HUNDRED - totalRatio : HUNDRED;
}

static bool IsRecoveredTargetConfigZero(const ProcessTargetConfig *config)
{
    if (config->count == 0) {
        return true;
    }
    for (uint32_t i = 0; i < config->count; i++) {
        if ((config->migrateMode == MIG_RATIO_MODE && config->targets[i].ratio != 0) ||
            (config->migrateMode == MIG_MEMSIZE_MODE && config->targets[i].memSizeKB != 0)) {
            return false;
        }
    }
    return true;
}

static int AssignProcessAttr(ProcessAttr *attr, const struct ProcessPayload *payload)
{
    ProcessTargetConfig activeConfig;
    int ret = RestoreProcessTargetConfig(&activeConfig, payload->migrateMode, payload->count, payload->migrateParam);
    if (ret) {
        return ret;
    }

    attr->pid = payload->pid;
    attr->numaAttr.numaNodes = payload->numaNodes;
    attr->type = payload->type;
    attr->state = payload->state == PROC_MIGRATE ? PROC_IDLE : payload->state;
    attr->scanType = payload->scanType;
    attr->scanTime = payload->scanTime;
    attr->duration = payload->duration;
    attr->enableSwap = true;
    attr->isFirstScan = true;
    ApplyRecoveredTargetConfig(attr, &activeConfig);
    attr->autoRemoveWhenRemoteEmpty = attr->scanType == NORMAL_SCAN && IsRecoveredTargetConfigZero(&activeConfig);
    if (time(&attr->scanStart) == (time_t)-1) {
        SMAP_LOGGER_ERROR("get time error.");
    }
    return 0;
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
    ProcessAttr *attr;
    uint32_t nrPayload;
    bool errFlag = false;

    SMAP_LOGGER_DEBUG("Enter RecoverProcessConfig.");
    nrPayload = header->len / CONFIG_PROC_LEN;
    struct ProcessPayload *payload = (struct ProcessPayload *)JumpToProcessPayload(processBase);
    for (uint32_t i = 0; i < nrPayload; i++, payload++) {
        /* 页类型一致性校验：pid 实际页类型须与 smap 当前模式匹配，否则内核扫描返回 -EINVAL */
        if (IsPidUsingHugePages(payload->pid) != IsHugeMode()) {
            SMAP_LOGGER_WARNING("pid %d page type mismatch smap mode, skip recover.", payload->pid);
            continue;
        }
        /* pidType 与全局 pageType 解耦：按 payload 携带的 per-pid type 恢复，不再做全局一致性过滤 */
        attr = calloc(1, sizeof(ProcessAttr));
        if (!attr) {
            SMAP_LOGGER_ERROR("Malloc for process %d failed.", payload->pid);
            errFlag = true;
            break;
        }
        InitProcessMigrationTargetState(attr);
        int ret = AssignProcessAttr(attr, payload);
        if (ret) {
            SMAP_LOGGER_ERROR("Skip invalid persisted process %d: %d.", payload->pid, ret);
            free(attr);
            continue;
        }
        SMAP_LOGGER_INFO("ProcessPayload %d, type %hu, numaNodes %#x, state %hu, scan type %hu, scan time %u.",
                         attr->pid, attr->type, attr->numaAttr.numaNodes, attr->state, attr->scanType, attr->scanTime);
        for (uint32_t j = 0; j < attr->targetConfig.count; j++) {
            SMAP_LOGGER_INFO("ProcessPayload %d, destNid %d, ratio %u, memsize %llu.", attr->pid,
                             attr->targetConfig.targets[j].remoteNid, attr->targetConfig.targets[j].ratio,
                             attr->targetConfig.targets[j].memSizeKB);
        }
        PidSlotAdd(manager, attr);
        manager->nr[attr->type]++;
    }
    if (errFlag) {
        for (int i = 0; i < MAX_PID_SLOTS; i++) {
            if (EnvAtomicRead(&manager->slots[i].state) == PID_SLOT_INUSE) {
                PidSlotRemove(manager, manager->slots[i].pid);
            }
        }
        manager->nr[VM_TYPE] = manager->nr[PROCESS_TYPE] = 0;
        return -ENOMEM;
    }
    SMAP_LOGGER_DEBUG("Exit RecoverProcessConfig.");
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

static int WriteTargetConfigToPayload(struct PersistRemoteTarget payloadTargets[], int *payloadCount,
                                      uint8_t *payloadMode, const ProcessTargetConfig *config)
{
    if (!payloadTargets || !payloadCount || !payloadMode || !config || config->count > REMOTE_NUMA_NUM ||
        (config->migrateMode != MIG_RATIO_MODE && config->migrateMode != MIG_MEMSIZE_MODE)) {
        return -EINVAL;
    }
    *payloadCount = (int)config->count;
    *payloadMode = (uint8_t)config->migrateMode;
    uint64_t totalRatio = 0;
    uint64_t pageSizeKB = GetPageSize() / KIB;
    bool remoteSeen[REMOTE_NUMA_NUM] = { false };
    for (uint32_t i = 0; i < config->count; i++) {
        int remoteIndex;
        const ProcessRemoteTarget *target = &config->targets[i];
        if (RemoteNidToIndex(target->remoteNid, GetNrLocalNuma(), &remoteIndex) || remoteSeen[remoteIndex] ||
            (config->migrateMode == MIG_RATIO_MODE &&
             (target->ratio > HUNDRED || totalRatio + target->ratio > HUNDRED)) ||
            (config->migrateMode == MIG_MEMSIZE_MODE &&
             (pageSizeKB == 0 || target->memSizeKB % pageSizeKB != 0 || target->memSizeKB / pageSizeKB > UINT32_MAX))) {
            return -EINVAL;
        }
        remoteSeen[remoteIndex] = true;
        totalRatio += target->ratio;
        payloadTargets[i] = (struct PersistRemoteTarget){
            .nid = target->remoteNid,
            .ratio = (uint8_t)target->ratio,
            .memSize = target->memSizeKB,
        };
    }
    return 0;
}

static int BuildAllProcessPayload(struct ProcessPayload **payload, int *len)
{
    struct ProcessManager *manager = GetProcessManager();
    struct ProcessPayload *p, *tmp;
    int nrPayload = 0;
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);

    for (size_t i = 0; i < n; i++) {
        if (!all[i]->attr->groupPolicy.enabled || all[i]->attr->groupPolicy.groupCount <= 0) {
            nrPayload++;
        }
    }

    SMAP_LOGGER_DEBUG("BuildAllProcessPayload nrPayload %d.", nrPayload);
    if (nrPayload == 0) {
        *payload = NULL;
        *len = 0;
        SMAP_LOGGER_INFO("Number of process payload is 0.");
        PidSlotReleaseRefs(all, n);
        return 0;
    }

    p = calloc(nrPayload, CONFIG_PROC_LEN);
    if (!p) {
        SMAP_LOGGER_ERROR("BuildAllProcessPayload calloc failed.");
        PidSlotReleaseRefs(all, n);
        return -ENOMEM;
    }

    tmp = p;
    for (size_t i = 0; i < n; i++) {
        ProcessAttr *attr = all[i]->attr;
        if (attr->groupPolicy.enabled && attr->groupPolicy.groupCount > 0) {
            continue;
        }
        tmp->pid = attr->pid;
        tmp->scanType = attr->scanType;
        tmp->type = attr->type;
        const ProcessTargetConfig *effectiveConfig = attr->pendingTargetConfigValid ? &attr->pendingTargetConfig :
                                                                                      &attr->targetConfig;
        tmp->state = attr->state == PROC_MIGRATE ? PROC_IDLE : attr->state;
        tmp->numaNodes = attr->pendingTargetConfigValid && attr->pendingTargetNumaNodes != 0 ?
                             attr->pendingTargetNumaNodes :
                             attr->numaAttr.numaNodes;
        tmp->scanTime = attr->scanTime;
        tmp->duration = attr->duration;
        int ret = WriteTargetConfigToPayload(tmp->migrateParam, &tmp->count, &tmp->migrateMode, effectiveConfig);
        if (ret) {
            SMAP_LOGGER_ERROR("Build pid %d effective target config payload failed: %d.", attr->pid, ret);
            free(p);
            PidSlotReleaseRefs(all, n);
            return ret;
        }
        tmp++;
    }

    *payload = p;
    *len = nrPayload;

    PidSlotReleaseRefs(all, n);
    return 0;
}

static int MapAndWriteProcessConfig(int fd, size_t mapLen, struct ProcessPayload *payload, int nrPayload)
{
    int ret;
    char *addr;
    char *processBase;
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
    struct SmapConfigHeader header;

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

    newConfigLen = CalcConfigLen(nrPayload);
    SMAP_LOGGER_DEBUG("New config length with %d processes: %d.", nrPayload, newConfigLen);

    // Extend config before writing process config
    if (newConfigLen > oldConfigLen) {
        SMAP_LOGGER_INFO("New len > old len, %zu > %zu, extend file.", newConfigLen, oldConfigLen);
        ret = TruncateConfig(fd, newConfigLen);
        if (ret) {
            SMAP_LOGGER_ERROR("Extend smap config to %zu failed: %d.", newConfigLen, ret);
            free(payload);
            return ret;
        }
    }

    ret = MapAndWriteProcessConfig(fd, newConfigLen, payload, nrPayload);
    if (ret) {
        SMAP_LOGGER_ERROR("Map and write process config failed: %d.", ret);
        free(payload);
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
    return ret;
}

static int ParseConfig(int fd, struct SmapConfigHeader *header)
{
    int ret;
    char *addr;
    char *numaBase;
    char *processBase;
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

    // Parse ProcessConfig
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
