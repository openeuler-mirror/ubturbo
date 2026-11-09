/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef __CONFIG_FILE_H__
#define __CONFIG_FILE_H__

#include <stdint.h>

#include "manage.h"

#define NORMAL_ERR (-1)

#define SMAP_CONFIG_PATH "/dev/shm/smap_config"
#define SMAP_CONFIG_MODE (0600)

/*
 * Layout of config is as follows:
 * | VER | HEADER_LEN | TOTAL_LEN | = 2B + 2B + 4B
 * | NUMA_PAYLOAD_LEN | NUMA_PAYLOAD | = 4B + 4B * nrLocalNuma * SMAP_MAX_NUMNODES
 * | PROCESS_PAYLOAD_LEN | PROCESS_PAYLOAD | = 4B + Σ V3 记录字节数
 *
 * V2 起进程记录为定长；V3 起改为「定长头 + 尾部柔性区」，节点位图不再按
 * 编译期 NUMA_NODES_BIT_WORDS 定宽，字数由写入侧运行期 cap 折算。
 */
#define SMAP_CONFIG_VER_V1 1
#define SMAP_CONFIG_VER_V2 2
#define SMAP_CONFIG_VER_V3 3
#define SMAP_CONFIG_VER SMAP_CONFIG_VER_V3
#define CONFIG_HEADER_LEN sizeof(struct SmapConfigHeader)
#define PAYLOAD_HEADER_LEN sizeof(struct PayloadHeader)
#define CONFIG_NUMA_LEN sizeof(struct NumaPayload)
#define CONFIG_PROC_HEADER_LEN sizeof(struct ProcessPayloadHeader)

struct SmapConfigHeader {
    uint8_t ver;
    uint8_t runMode;
    uint16_t headerLen;
    uint32_t totalLen;
};

struct PayloadHeader {
    uint32_t len;
};

struct NumaPayload {
    int16_t local; // L1 NUMA ID，0-255
    int16_t remote; // L2 NUMA ID，0-255
    uint32_t size; // Available size, unit is MB
};

struct OldProcessPayload {
    pid_t pid;
    uint8_t ratio; // remote ratio set by upstream component
    uint8_t scanType;
    uint8_t type;
    uint8_t state;
    uint8_t migrateMode;
    int16_t l1Node[4];
    int16_t l2Node[4];
    uint32_t scanTime;
    uint64_t memSize;
};

struct PersistRemoteTarget {
    int nid;
    uint8_t ratio; // remote ratio set by upstream component
    uint64_t memSize;
};

/*
 * V3 记录定长头（32B，8 字节对齐）。尾部柔性区依次为：
 *   unsigned long numaNodes[nrWords]                          // 变长节点位图
 *   struct PersistRemoteTarget migrateParam[count]            // 活跃 Pair 目标
 * 单条记录字节数 = CONFIG_PROC_HEADER_LEN + nrWords * 8 + count * sizeof(struct PersistRemoteTarget)。
 */
struct ProcessPayloadHeader {
    pid_t pid;
    uint8_t scanType;
    uint8_t type;
    uint8_t state;
    uint8_t migrateMode;
    int srcNid; // 迁出源本地 NUMA，NUMA_NO_NODE 表示未指定
    uint32_t scanTime;
    uint32_t duration;
    uint32_t nrWords; // 尾部 numaNodes 字数（按写入侧运行期 cap 折算）
    int count; // 尾部 migrateParam 条数
    uint8_t ignoreRemoteCapacity; // sync 旁路容量标记
    /* 对齐填充：定长头保持 8 字节对齐，尾部柔性区（unsigned long 位图）起始地址仍 8 字节对齐 */
    uint8_t reserved[3];
};

/* 进程内构建载荷（含变长位图句柄）；序列化后落盘为 V3 记录 */
struct ProcessPayload {
    pid_t pid;
    uint8_t scanType;
    uint8_t type;
    uint8_t state;
    uint8_t migrateMode;
    int srcNid;
    uint32_t scanTime;
    uint32_t duration;
    NumaBitmap numaNodes;
    int count;
    struct PersistRemoteTarget migrateParam[SMAP_MAX_NUMNODES];
    uint8_t ignoreRemoteCapacity; /* 新增：sync 旁路容量标记 */
};

int RecoverFromConfig(void);
int SyncRunMode(RunMode runMode);
int SyncOneNumaConfig(int local, int remote, size_t size);
int SyncAllProcessConfig(void);

#endif /* __CONFIG_FILE_H__ */
