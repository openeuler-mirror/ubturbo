/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared SMAP kernel/user constants.
 */
#ifndef SMAP_COMMON_H
#define SMAP_COMMON_H

#define SMAP_ACCESS_MAGIC 0xBB
#define SMAP_MIGRATE_MAGIC 0xB9

#define DECIMAL 10
#define MAX_4K_PROCESSES_CNT 300
#define MAX_2M_PROCESSES_CNT 100
#define MAX_PER_PID_MIG_LIST_COUNT 8
#define MAX_NR_MIGOUT 40
#define MAX_NR_REMOVE MAX_NR_MIGOUT
#define MAX_NR_MIGBACK 50
#define MAX_NR_MIGNUMA 50
#define MAX_SCAN_DURATION_SEC 300
#define NON_EXIST_PID (-1)

/* 内核节点空间绝对上界：主线 CONFIG_NODES_SHIFT 最大为 10（1024 节点），
 * 用于用户态对 Mems_allowed 宽度解析 cap 的 sanity 校验（见 doc/runtime-numa-cap-design.md） */
#define SMAP_NODE_CAP_LIMIT 1024

/* 每个十六进制字符承载的位宽：Mems_allowed 打印宽度解析的换算系数（cap = 字符数 × 该值） */
#define SMAP_BITS_PER_HEX_CHAR 4

/* ABI 节点空间定维 = 节点空间绝对上界（同宽原则：内核定维数组、readback 序列化
 * 与用户态解析共用同一常量，杜绝两端宽度漂移）。
 * 内核侧 BUILD_BUG_ON(SMAP_MAX_NUMNODES < MAX_NUMNODES) 保证可承载全部节点；
 * 运行期有效范围以 Mems_allowed 打印宽度解析的 cap（= MAX_NUMNODES）为界，见 GetNumaCap()。 */
#define SMAP_MAX_NUMNODES SMAP_NODE_CAP_LIMIT

/* numa_nodes bitmap words（LP64 下 = 16） */
#define NUMA_NODES_BIT_WORDS                                   \
	((SMAP_MAX_NUMNODES + sizeof(unsigned long) * 8 - 1) / \
	 (sizeof(unsigned long) * 8))

/* [split-compat] 旧编译期节点宏暂时保留，供尚未迁移的 user 态与 DT 引用；
 * 待用户态节点维度全部切换为运行期 cap 后统一删除 */
#ifndef USE_DT
#define SMAP_MAX_LOCAL_NUMA_NODES 8
#else
#define SMAP_MAX_LOCAL_NUMA_NODES 4
#endif
#define SMAP_MAX_REMOTE_NUMA_NODES 18
#define SMAP_MAX_NUMA_NODES \
	(SMAP_MAX_LOCAL_NUMA_NODES + SMAP_MAX_REMOTE_NUMA_NODES)

#endif /* SMAP_COMMON_H */