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
#ifndef __MANAGE_H__
#define __MANAGE_H__

#include "smap_env.h"
#include "numa_nodes.h"
#include "advanced-strategy/scene_info.h"
#include "smap_ioctl.h"

#ifndef MAX_NR_GROUPED_MIGOUT
#define MAX_NR_GROUPED_MIGOUT MAX_NR_MIGOUT
#endif
#ifndef MAX_MIGRATION_GROUP_NUM
#define MAX_MIGRATION_GROUP_NUM 8
#endif
#ifndef MAX_GROUP_LOCAL_NUMA
#define MAX_GROUP_LOCAL_NUMA LOCAL_NUMA_NUM
#endif
#ifndef MAX_GROUP_REMOTE_NUMA
#define MAX_GROUP_REMOTE_NUMA REMOTE_NUMA_NUM
#endif
#define MAX_4K_PROCESSES_CNT 300
#define MAX_2M_PROCESSES_CNT 100
#define MAX_PAIR_TARGET_COUNT (MAX_4K_PROCESSES_CNT * LOCAL_NUMA_NUM * REMOTE_NUMA_NUM)
#define MAX_THREADS 10
#define MAX_RES_LEN 4
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define DEFAULT_FD (-1)

#define RSS_LINE_PREFIX "Rss:"
#define RSS_LINE_PREFIX_LENGTH 4
#define HUGETLB_LINE_PREFIX "Private_Hugetlb:"
#define HUGETLB_LINE_PREFIX_LENGTH 16

#define PRESENT (1ULL << 63)
#define PRN_SHIFT ((1ULL << 55) - 1)
#define MAPS_LIN_LEN 2
#define MAPS_MAX_LEN 256

#define BUFFER_SIZE 256
#define PAGEMAP_ENTRY_SIZE 8

#define DEFAULT_L1_NODE (-1)
#define DEFAULT_L2_NODE (-1)
#define DEFAULT_DEST_NODE (-1)

#define BIT_TO_BYTE 8
#define CPU_NUMA_PATH "/sys/devices/system/cpu/cpu%d/node%d"
#define NUMAMAP_HUGE_2M_SUBSTR "kernelpagesize_kB=2048"

#define MMAP_TYPE_STRING_LEN 20

#define STRATEGY_CONFIG_PATH "/opt/ubturbo/conf/smap/period.config"
#define DEFAULT_NMEMB 1
#define MAX_MIGRATE_BACK_WAIT_TIME 60
#define MIGRATE_BACK_CHECK_PERIOD 1000
#define MAX_FRESH_USED_TIME 20
#define WAIT_FRESH_USED_PERIOD 200
#define MAX_CHECK_ALREADY_FORBIDDEN_TIME 100
#define WAIT_CHECK_ALREADY_FORBIDDEN_PERIOD 200

#define MANAGED_LOCAL_AFFINITY_REFRESH_INTERVAL_MS 30000U

#define WAIT_PROC_STATE_PERIOD 100
#define WAIT_PROC_STATE_MAX_RETRY 300
#define MAX_NR_MIGRATE_NUMA_RANGE 50

#define PID_CMD_LENGTH 64
#define MAX_LINE_LENGTH 1024
#define NUMA_MAPS_MAX_PATTERN_LEN 20

#define FREQ_BUCKETS_SIZE 256

extern EnvAtomic g_forbiddenNodes[MAX_NODES];

#define NODE_FORBIDDEN_USER (1 << 0)
#define NODE_FORBIDDEN_MIGBACK_DONE (1 << 1)
#define NODE_FORBIDDEN_MIGBACK_BUSY (1 << 2)

/* 导出/传输类型：经内核 sqrt 压缩后落在 0..255，对齐 FREQ_BUCKETS_SIZE(256) */
typedef uint8_t actc_t;

typedef enum {
    WATERLINE_MODE = 0,
    MEM_POOL_MODE,
    MAX_RUN_MODE,
} RunMode;

typedef enum {
    PROCESS_TYPE = 0,
    VM_TYPE,
    TYPE_MAX,
} PidType;

typedef enum {
    L1,
    L2,
    NR_LEVEL,
} NodeLevel;

typedef enum {
    DEMOTE,
    PROMOTE,
    SWAP,
} MigrateDirection;

typedef enum {
    UB_BW_NORMAL = 0,
    UB_BW_SWAP_STOP,
} UbBwRestrictType;

typedef enum { MMAP_PARIVATE, MMAP_SHARED, NR_MMAP_TYPE } MmapType;

enum {
    DISABLE_PROCESS_MIGRATE,
    ENABLE_PROCESS_MIGRATE,
};

typedef struct {
    actc_t freq;
    uint8_t isWhiteListPage : 1; // bit 0
    uint8_t isSelected : 1; // bit 1
    uint8_t prior : 6; // bits 2-7
} __attribute__((packed)) ActcData;

typedef struct {
    uint64_t addr;
    actc_t freq;
    int node;
    uint8_t prior;
} LevelActcData;

typedef struct {
    uint8_t freqMin;
    uint8_t freqMax;
    uint32_t freqZero;
    uint64_t freqNum;
    uint64_t pageNum;
    uint64_t freqSum;
    uint64_t remoteHotNum;
    uint64_t whiteNum;
    uint32_t freqBuckets[FREQ_BUCKETS_SIZE]; /* 频次桶：freqBuckets[freq] = 频次为freq的非白名单页面数 */
} ActCount;

typedef struct {
    uint64_t maxMigrate;
    uint32_t freqWt;
    uint32_t slowThred;
} SeparateParam;

typedef struct {
    int nid;
    uint64_t quotaPages;
    uint64_t usedPages;
} GroupTargetAttr;

typedef struct {
    int nid;
    uint64_t localReservePages;
} GroupLocalAttr;

typedef struct {
    int localCount;
    GroupLocalAttr locals[MAX_GROUP_LOCAL_NUMA];
    int targetCount;
    GroupTargetAttr targets[MAX_GROUP_REMOTE_NUMA];
    uint8_t swapCandidateRounds;
} MigrationGroupAttr;

typedef struct {
    bool enabled;
    int groupCount;
    MigrationGroupAttr groups[MAX_MIGRATION_GROUP_NUM];
} GroupMigrationPolicy;

typedef struct {
    /* Runtime-only grouped policy staged while the active policy is migrating. */
    bool valid;
    uint32_t nodeBitmap;
    GroupMigrationPolicy policy;
} PendingGroupMigrationPolicy;

typedef struct {
    int index;
    unsigned short nrVcpu;
    unsigned long long *cpuTime[MAX_RES_LEN];
    struct timeval realTime[MAX_RES_LEN];
    bool isHeavyLoad;
} ResourceInfo;

typedef enum {
    HAM_SCAN,
    NORMAL_SCAN,
    STATISTIC_SCAN,
    SCAN_TYPE_MAX,
} ScanType;

enum ProcessState {
    PROC_IDLE, // 空闲
    PROC_MIGRATE, // 冷热迁移
    PROC_BACK, // 迁回
    PROC_MOVE, // 逃生
};

typedef struct {
    uint32_t localNrPages;
    uint32_t remoteNrPages;
} PagePair;

typedef struct {
    SceneInfo sceneInfo; // 场景：轻载/重载/稳态/非稳态，扫描周期等
    bool enableAdaptMem; // 是否使能自适应，仅对虚机开启
} AdaptMem;

typedef struct {
    uint32_t nrPage; // 进程使用的page数量
    uint32_t nrPages[MAX_NODES]; // 进程在近远端使用的page数量
} WalkPage;

typedef struct {
    uint32_t cpuMask[LOCAL_NUMA_NUM]; // CPU绑定情况
    uint32_t numaNodes; // numa bitmap: 0-unused, 1-used
} NumaAttribute;

/*
 * User-requested aggregate target for one remote NUMA node. The process-level
 * migrate mode determines whether ratio or memSizeKB is effective.
 */
typedef struct {
    int remoteNid;
    uint32_t ratio;
    uint64_t memSizeKB;
} ProcessRemoteTarget;

/*
 * Source of truth for a process's normal migrate-out request. Pair-level
 * runtime matrices must not be used to reconstruct this configuration.
 */
typedef struct {
    MigrateMode migrateMode;
    uint32_t count;
    ProcessRemoteTarget targets[REMOTE_NUMA_NUM];
} ProcessTargetConfig;

typedef struct {
    uint32_t managedLocalMask;
    uint32_t observedLocalMask;
    uint32_t residentLocalMask;
    uint32_t affinityLocalMask;
    uint32_t affinityRefreshElapsedMs;
    uint32_t accountLocalMask[REMOTE_NUMA_NUM];
    bool affinityValid;
    bool affinitySampled;
} ManagedLocalState;

/* Runtime-only assigned request and capacity-clipped target for one Pair. */
typedef struct {
    pid_t pid;
    int localNid;
    int remoteNid;
    uint32_t requestedPages;
    uint32_t targetPages;
} PairTarget;

/*
 * Explicit, immutable inputs used while deriving Pair requested targets.
 * capacityLocalMask only identifies eligible pairs; it does not reserve or
 * consume any private/shared capacity.
 */
typedef struct {
    int nrLocalNuma;
    uint64_t pageSizeKB;
    uint32_t capacityLocalMask[REMOTE_NUMA_NUM];
} PairRequestContext;

typedef struct {
    uint64_t managedTotalPages;
    /* Original aggregate request derived from ProcessTargetConfig. */
    uint64_t requestedRemotePages[REMOTE_NUMA_NUM];
    /* Aggregate request limited only by the process's managed pages. */
    uint64_t effectiveRemotePages[REMOTE_NUMA_NUM];
    /*
     * Portion of effectiveRemotePages that cannot currently be assigned to
     * any eligible local -> remote Pair. The aggregate request remains in
     * ProcessTargetConfig and can be assigned when Pair eligibility returns.
     */
    uint64_t unassignedRequestedPages[REMOTE_NUMA_NUM];
} PairRequestSummary;

/* Runtime-only migration decision for one local-to-remote pair. */
typedef struct {
    pid_t pid;
    int localNid;
    int remoteNid;
    int remoteIndex;
    uint32_t targetPages;
    uint32_t actualPages;
    uint32_t demotePages;
    uint32_t promotePages;
    uint32_t swapPages;
} PairPlan;

/* Per-cycle destination-node budget shared by all processes and Pairs. */
typedef struct {
    int nrLocalNuma;
    uint64_t freePages[MAX_NODES];
    uint64_t safetyReservePages[MAX_NODES];
    uint64_t plannedPages[MAX_NODES];
} PairPlanContext;

/* Per-cycle migration budget shared by all Pairs of one process. */
typedef struct {
    pid_t pid;
    uint64_t maxMigratePages;
    uint64_t plannedPages;
} PairPidBudget;

typedef struct {
    /*
     * Pair-level compatibility fields generated from ProcessTargetConfig.
     * They are runtime state, not the source of the user-requested target.
     */
    double initRemoteMemRatio[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    uint64_t memSize[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    uint32_t allocRemoteNrPages[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM]; // 根据账本计算的，各本地numa对应的远端page的数量
    uint32_t nrPagesPerLocalNuma[LOCAL_NUMA_NUM]; // 根据账本计算的，各本地numa可支配的内存
    double l2RemoteMemRatio[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    double l3RemoteMemRatio[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    uint32_t nrMigratePages[MAX_NODES][MAX_NODES]; // 水线场景：消减后的迁移量；密度场景：接口设置的比例
    uint32_t remoteNrPagesAfterMigrate[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM]; // 迁移后记录账本
    MigrateDirection dir[MAX_NODES]; // 算法决策各numa的迁出的方向 demote/promote/swap
    UbBwRestrictType ubBwRestrict[MAX_NODES]; // 各NUMA的UB带宽限制策略
    SeparateParam separateParam;
} StrategyAttribute;

typedef struct {
    uint32_t scanTime;
    ScanType scanType; // 标识添加进程组件 HAM/普通冷热
    uint64_t actcLen[MAX_NODES];
    ActcData *actcData[MAX_NODES]; // actc数据
    ActCount actCount[MAX_NODES]; // 统计数据
    uint32_t selectedBuckets[MAX_NODES][FREQ_BUCKETS_SIZE]; // 已选频次为freq的页面数
} ScanAttribute;

typedef struct {
    MmapType mmapType; // 内存映射模式SHARED/PRIVATE
} VMPidAttribute;

struct ProcessAttribute {
    PidType type; // VM/PID
    pid_t pid;
    enum ProcessState state;
    uint32_t scanTime;
    uint32_t duration; // scanType为统计模式时记录统计时长
    ScanType scanType; // 标识添加进程组件
    time_t scanStart;
    SceneInfo sceneInfo; // 场景：轻载/重载/稳态/非稳态，扫描周期等
    MigrateMode migrateMode; // 内存迁移模式，按照比例或是大小
    int initLocalMemRatio; // 接口设置的内存比例
    int remoteNumaCnt; // 远端numa数量
    bool isLowMem; // 多numa虚机场景，表示目的端内存不够
    bool enableSwap; // 控制是否开启交换，默认开启
    bool isFirstScan; // 标记首次扫描，需要恢复扫描周期
    bool autoRemoveWhenRemoteEmpty; // 上层将远端目标调为0后，远端页清空时自动移除纳管
    bool syncWaitRemoteEmpty; // 同步迁移等待远端页清空时，临时保护进程不被自动移除
    /* Runtime-only compatibility mode for migrate_out_sync targets. */
    bool ignoreRemoteCapacity;
    bool pendingIgnoreRemoteCapacity;
    struct { // 迁移相关参数
        int nid;
        uint64_t memSize; // 迁移内存大小,单位为KB
    } migrateParam[REMOTE_NUMA_NUM];
    ProcessTargetConfig targetConfig;
    ProcessTargetConfig pendingTargetConfig;
    bool pendingTargetConfigValid;
    uint32_t pendingTargetNumaNodes;
    ManagedLocalState managedLocalState;
    SeparateParam separateParam;
    NumaAttribute numaAttr;
    WalkPage walkPage;
    AdaptMem adaptMem;
    GroupMigrationPolicy groupPolicy;
    uint64_t groupSwapLastTotalPages;
    uint8_t groupSwapStableTotalRounds;
    bool groupSwapTotalPagesValid;
    bool groupSwapFrozen;
    PendingGroupMigrationPolicy pendingGroupPolicy;
    StrategyAttribute strategyAttr;
    ScanAttribute scanAttr;
    VMPidAttribute vmPidAttr;
    struct ProcessAttribute *next;
};
typedef struct ProcessAttribute ProcessAttr;

/*
 * A prepared process update that is published only after access tracking has
 * accepted prepared->numaAttr.numaNodes. The prepared ProcessAttr is the
 * candidate introduced by the target-configuration transaction.
 */
typedef struct {
    ProcessAttr *active;
    ProcessAttr *prepared;
    bool isNew;
    bool isPending;
} ProcessManageCandidate;

typedef struct {
    uint16_t nrSegment;
    uint32_t nrPages;
    uint64_t startPa;
    uint64_t endPA;
} NodeMem;

struct MigList {
    bool successToUser;
    uint64_t nr;
    uint64_t failedMigNr;
    uint64_t failedIsolatedNr;
    pid_t pid;
    int from;
    int to;
    uint64_t *addr;
};

struct MigPra {
    int pageSize;
    int nrThread;
    bool isMulThread;
};

struct MigrateMsg {
    int cnt;
    struct MigPra mulMig;
    struct MigList *migList;
};

struct MigrateNumaIoctlMsg {
    int srcNid;
    int destNid;
    int count;
    uint64_t memids[MAX_NR_MIGRATE_NUMA_RANGE];
};

struct MigPayload {
    pid_t pid;
    int srcNid;
    int destNid;
    int ratio;
    int keepRatio;
    uint64_t memSize;
    bool isRatioMode;
    uint64_t successCnt;
};

struct MigPidRemoteNumaIoctlMsg {
    int pidCnt;
    struct MigPayload *payloads;
    int *migResArray; // 迁移结果
};

// 反向扫描参数，所有process共享
typedef struct {
    uint32_t pageSize;
    bool trackingEnabled; // tracking当前是否处于enable状态
} TrackingAttr;

typedef struct { // tracking设备与迁移设备的fd
    int nodes[MAX_NODES]; // 每个tracking-node设备的fd
    int migrate; // 迁移字符设备fd
    int access; // access设备
    int lock; // 文件锁，使SmapStart只被初始化一次
} DevFds;

struct RemoteNumaUsedInfo {
    uint64_t size;
    uint64_t used;
    bool ifUsedFreshed;
};

struct RemoteNumaInfo {
    EnvMutex lock;
    uint64_t privateSize[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
    uint64_t sharedSize[REMOTE_NUMA_NUM];
    struct RemoteNumaUsedInfo usedInfo[REMOTE_NUMA_NUM];
    struct RemoteNumaUsedInfo privateUsedInfo[LOCAL_NUMA_NUM][REMOTE_NUMA_NUM];
};

struct UbBwMonitor {
    uint32_t ubBwThreshold; // UB带宽阈值(MB/s)
    struct UbFluxMbStatistic currentFluxMb; // 当前周期UB带宽数据
    int currentFluxRet; // 当前周期UB带宽查询结果
};

struct ProcessManager {
    ProcessAttr *processes;
    SceneInfo sceneInfo;
    uint16_t nr[TYPE_MAX];
    uint16_t nrThread; // 线程数量
    uint16_t nrLocalNuma; // local numa数量
    DevFds fds;
    TrackingAttr tracking; // 反向扫描参数
    void *threadCtx[MAX_THREADS]; // 管理的线程上下文
    struct RemoteNumaInfo remoteNumaInfo; // 借用远端内存数量
    EnvMutex lock;
    EnvMutex threadLock;
    struct UbBwMonitor ubBwMonitor; // UB带宽监控
};

struct ProcessMemBitmap {
    pid_t pid;
    size_t nrPages[MAX_NODES];
};

typedef struct {
    pid_t pid;
    uint32_t scanTime;
    uint32_t duration;
    int scanType;
    int count;
    struct {
        int nid;
        int ratio;
        uint64_t memSize;
        MigrateMode migrateMode;
    } numaParam[REMOTE_NUMA_NUM];
    bool targetConfigValid;
    bool ignoreRemoteCapacity;
    ProcessTargetConfig targetConfig;
} ProcessParam;

void DebugProcessAttr(struct ProcessManager *manager);

int GetNrLocalNuma(void);

/* Range-check a remote NUMA id against the manager's local/remote layout. */
bool IsRemoteNidValid(int nid);

void InitProcessTargetConfig(ProcessTargetConfig *config);
void ClearProcessTargetConfig(ProcessTargetConfig *config);
int CopyProcessTargetConfig(ProcessTargetConfig *dest, const ProcessTargetConfig *src);
bool RemoveProcessRemoteTarget(ProcessTargetConfig *config, int remoteNid);
int MoveProcessRemoteTarget(ProcessTargetConfig *config, int srcNid, int destNid, uint64_t memSizeKB, int ratio);
const ProcessRemoteTarget *FindProcessRemoteTarget(const ProcessTargetConfig *config, int remoteNid);
int RemoteNidToIndex(int remoteNid, int nrLocalNuma, int *remoteIndex);
void InitProcessMigrationTargetState(ProcessAttr *attr);
int ProcessManagerInit(uint32_t pageType);

int DestroyProcessManager(void);

int LoadMangerNrProcessNum(void);

int LoadMangerNrVmNum(void);

bool PidIsValid(pid_t pid);

int IsQemuTask(pid_t pid);

PidType GetPidType(struct ProcessManager *manager);

uint32_t GetNormalPageSize(void);

uint32_t GetHugePageSize(void);

uint32_t GetPageSize(void);

ProcessAttr *GetProcessAttr(pid_t pid);

int ReadCmdlineByPid(pid_t pid, char *buf, int len);

int VMPreprocess(pid_t pid, ProcessAttr *attr);

int SetProcessLocalNuma(pid_t pid, uint32_t *nodeBitmap, bool hugeFlag);
int SetLocalNumaByCpu(pid_t pid, uint32_t *nodeBitmap);

int PrepareProcessManageCandidate(ProcessParam *param, PidType type, ProcessManageCandidate *candidate);
void DiscardProcessManageCandidate(ProcessManageCandidate *candidate);
void PublishProcessManageCandidate(ProcessManageCandidate *candidate);
int ProcessAddManage(ProcessParam *param, uint32_t *nodeBitmap);
int UpdateManagedProcessTrackingMode(ProcessAttr *attr, ScanType scanType, uint32_t scanTime, uint32_t duration);
int ConfigureMigrationTargets(ProcessAttr *attr, const ProcessTargetConfig *config);
int ApplyPendingMigrationTargets(ProcessAttr *attr);
int ProcessAddGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);
int ProcessSetPendingGroupedManage(pid_t pid, uint32_t nodeBitmap, const GroupMigrationPolicy *policy);
int ApplyPendingGroupedPolicy(ProcessAttr *attr);

void CheckAndRemoveInvalidProcess(void);

void RemoveManagedProcess(int nr, pid_t *pidArr);

int MigrateMemoryBack(pid_t pid, int srcNid, int desNid, uint64_t paStart, uint64_t paEnd);

int BuildAllPidData(void);
void CalibratePairAccount(ProcessAttr *attr);
int BuildPairRequestedTargets(const ProcessAttr *attr, const PairRequestContext *context, PairTarget targets[],
                              size_t targetCap, size_t *targetCnt, PairRequestSummary *summary);
int BuildAllPairTargets(struct ProcessManager *manager, PairTarget targets[], size_t targetCap, size_t *targetCnt);
int BuildAllPairPlanInputs(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                           PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt);
int BuildAllPairPlanInputsForState(struct ProcessManager *manager, PairPlan plans[], size_t planCap, size_t *planCnt,
                                   PairPidBudget pidBudgets[], size_t pidBudgetCap, size_t *pidBudgetCnt,
                                   bool migrateOnly);

int SetRemoteNumaInfo(int srcNid, int destNid, uint64_t size);

struct ProcessManager *GetProcessManager(void);

unsigned long GetPidNrPages(pid_t pid);

int GetNumaNodesForPid(pid_t pid, int *node);

void RemoveAllManagedProcess(void);

bool IsHugeMode(void);

bool IsHugeAligned(uint64_t addr);

int IsHugePageRange(const char *line);

bool CheckReadyMigrateBack(int destNid);

RunMode GetRunMode(void);
void SetRunMode(RunMode runMode);

void LinkedListAdd(ProcessAttr **head, ProcessAttr **add);
void LinkedListRemove(ProcessAttr **remove, ProcessAttr **head);

static inline bool IsNodeInvalid(int nid)
{
    return nid < 0 || nid >= MAX_NODES;
}

static inline bool IsDestNodeInvalid(int nid)
{
    if (nid == DEFAULT_DEST_NODE) {
        return false;
    }
    return IsNodeInvalid(nid);
}

static inline void SetNodeForbidden(int nid)
{
    int oldValue;
    int newValue;

    do {
        oldValue = EnvAtomicRead(&g_forbiddenNodes[nid]);
        newValue = oldValue | NODE_FORBIDDEN_USER;
    } while (EnvAtomicCmpAndSwap(oldValue, newValue, &g_forbiddenNodes[nid]) != oldValue);
}

static inline void ClearNodeForbidden(int nid)
{
    EnvAtomicSet(&g_forbiddenNodes[nid], 0);
}

static inline bool IsNodeForbidden(int nid)
{
    return EnvAtomicRead(&g_forbiddenNodes[nid]);
}

static inline bool IsNodeForbiddenReason(int nid, int reason)
{
    return (EnvAtomicRead(&g_forbiddenNodes[nid]) & reason) != 0;
}

static inline void SetNodeForbiddenReason(int nid, int reason)
{
    int oldValue;
    int newValue;

    do {
        oldValue = EnvAtomicRead(&g_forbiddenNodes[nid]);
        newValue = oldValue | reason;
    } while (EnvAtomicCmpAndSwap(oldValue, newValue, &g_forbiddenNodes[nid]) != oldValue);
}

static inline void ClearNodeForbiddenReason(int nid, int reason)
{
    int oldValue;
    int newValue;

    do {
        oldValue = EnvAtomicRead(&g_forbiddenNodes[nid]);
        newValue = oldValue & (~reason);
    } while (EnvAtomicCmpAndSwap(oldValue, newValue, &g_forbiddenNodes[nid]) != oldValue);
}

static inline int TrySetNodeForbiddenReason(int nid, int reason)
{
    int oldValue;
    int newValue;

    do {
        oldValue = EnvAtomicRead(&g_forbiddenNodes[nid]);
        if (oldValue & reason) {
            return -EAGAIN;
        }
        newValue = oldValue | reason;
    } while (EnvAtomicCmpAndSwap(oldValue, newValue, &g_forbiddenNodes[nid]) != oldValue);

    return 0;
}

int EnableProcessMigrate(pid_t *pidArr, int len, int enable);
int IsRemoteNumaMigrateBackAllowed(int nid);
int IsRemoteNumaMoveAllowed(int nid);
int ChangePidRemoteByNuma(int srcNid, int destNid);
int IsPidArrayStateChangeReady(pid_t *pidArr, int len, int enable);
int IsPidArrInState(pid_t *pidArr, int len, enum ProcessState state);
bool IsAllL2NodePidInState(enum ProcessState state, int l2Node);
int ChangePidRemoteByPid(struct MigPidRemoteNumaIoctlMsg *msg);
ProcessAttr *GetProcessAttrLocked(pid_t pid);

bool MigOutIsDone(ProcessAttr *attr, bool *isMultiNumaPid);
FILE *OpenNumaMaps(pid_t pid);
int GetPidNumaPagesFromNumaMaps(pid_t pid, uint64_t numaPages[MAX_NODES], bool onlyHuge);
int InitGroupedUsedPages(pid_t pid, GroupMigrationPolicy *policy, const uint64_t numaPages[MAX_NODES]);

void UpdateRemoteNumaCriticalErr(void);
bool IsRemoteNumaCriticalErr(int nid);

static inline uint64_t KBToHugePageCeil(uint64_t memSize)
{
    int pageSizeKB = GetHugePageSize() / KIB;
    return (memSize + pageSizeKB - 1) / pageSizeKB;
}

static inline uint64_t KBToHugePage(uint64_t memSize)
{
    int size = GetHugePageSize();
    return memSize / (size / KIB);
}

static inline uint64_t HugePageToKB(uint64_t nr)
{
    int size = GetHugePageSize();
    return nr * (size / KIB);
}

static inline uint64_t KBToNormalPage(uint64_t memSize)
{
    int size = GetNormalPageSize();
    return memSize / (size / KIB);
}

static inline uint64_t NormalPageToKB(uint64_t nr)
{
    int size = GetNormalPageSize();
    return nr * (size / KIB);
}

static inline uint64_t KBToPage(uint64_t memSize)
{
    return IsHugeMode() ? KBToHugePage(memSize) : KBToNormalPage(memSize);
}

static inline uint64_t PageToKB(uint64_t nr)
{
    return IsHugeMode() ? HugePageToKB(nr) : NormalPageToKB(nr);
}

static inline uint64_t MBToPage(uint64_t memSize)
{
    return memSize * MIB / GetPageSize();
}

static inline int GetCurrentMaxNrPid(void)
{
    return IsHugeMode() ? MAX_2M_PROCESSES_CNT : MAX_4K_PROCESSES_CNT;
}

/* L1 numaNodes helper functions */
static inline int GetAttrL1(ProcessAttr *attr)
{
    return GetL1(attr->numaAttr.numaNodes);
}

static inline bool EqualToAttrL1(ProcessAttr *attr, int nid)
{
    return EqualToL1(attr->numaAttr.numaNodes, nid);
}

static inline bool InAttrL1(ProcessAttr *attr, int nid)
{
    return InL1(attr->numaAttr.numaNodes, nid);
}

static inline bool NotInAttrL1(ProcessAttr *attr, int nid)
{
    return !InAttrL1(attr, nid);
}

static inline uint64_t GetL1ActcLen(ProcessAttr *attr)
{
    int nid = GetAttrL1(attr);
    return (nid == NUMA_NO_NODE) ? 0 : attr->scanAttr.actcLen[nid];
}

static inline ActCount *GetL1ActCount(ProcessAttr *attr)
{
    int nid = GetAttrL1(attr);
    return (nid == NUMA_NO_NODE) ? NULL : &attr->scanAttr.actCount[nid];
}

static inline ActcData *GetL1ActcData(ProcessAttr *attr)
{
    int nid = GetAttrL1(attr);
    return (nid == NUMA_NO_NODE) ? NULL : attr->scanAttr.actcData[nid];
}

/* L2 numaNodes helper functions */
static inline int GetAttrL2(ProcessAttr *attr)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    return GetL2(attr->numaAttr.numaNodes) - offset;
}

static inline void SetAttrL2(ProcessAttr *attr, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    SetL2(&attr->numaAttr.numaNodes, nid + offset);
}

static inline void AddAttrL2(ProcessAttr *attr, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    AddL2(&attr->numaAttr.numaNodes, nid + offset);
}

static inline void SetL2ByNid(uint32_t *nodes, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    SetL2(nodes, nid + offset);
}

static inline void AddL2ByNid(uint32_t *nodes, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    AddL2(nodes, nid + offset);
}

static inline bool EqualToAttrL2(ProcessAttr *attr, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    return EqualToL2(attr->numaAttr.numaNodes, nid + offset);
}

static inline bool NotEqualToAttrL2(ProcessAttr *attr, int nid)
{
    return !EqualToAttrL2(attr, nid);
}

static inline bool InAttrL2(ProcessAttr *attr, int nid)
{
    int offset = LOCAL_NUMA_BITS - GetNrLocalNuma();
    return InL2(attr->numaAttr.numaNodes, nid + offset);
}

static inline bool NotInAttrL2(ProcessAttr *attr, int nid)
{
    return !InAttrL2(attr, nid);
}

static inline uint64_t GetL2ActcLen(ProcessAttr *attr)
{
    int nid = GetAttrL2(attr);
    return (nid == NUMA_NO_NODE) ? 0 : attr->scanAttr.actcLen[nid];
}

static inline ActCount *GetL2ActCount(ProcessAttr *attr)
{
    int nid = GetAttrL2(attr);
    return (nid == NUMA_NO_NODE) ? NULL : &attr->scanAttr.actCount[nid];
}

static inline ActcData *GetL2ActcData(ProcessAttr *attr)
{
    int nid = GetAttrL2(attr);
    return (nid == NUMA_NO_NODE) ? NULL : attr->scanAttr.actcData[nid];
}

static inline bool IsNumaMapLineHuge(char *line)
{
    char *substr = strstr(line, NUMAMAP_HUGE_2M_SUBSTR);
    return substr != NULL;
}

static inline bool IsMultiNumaVm(ProcessAttr *process)
{
    return process->type == VM_TYPE && (process->remoteNumaCnt > 1 || GetL1Count(process->numaAttr.numaNodes) > 1);
}

bool IsMemoryLow(pid_t pid);
#endif /* __MANAGE_H__ */
