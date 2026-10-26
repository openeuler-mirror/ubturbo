/*
 * node_matrix.h - 节点维度二维矩阵原语（运行期 cap 定维，行主序扁平存储）
 *
 * 背景：节点维度数组禁止再引入 SMAP_MAX_NUMNODES 编译期定维（见
 * doc/runtime-numa-cap-design.md 3.5.1）。二维矩阵的列跨度若用编译期常量，
 * 则 cap<SMAP_MAX_NUMNODES 时列向内存不收敛；而运行期 cap 无法作为结构体
 * 成员的变长列宽，因此统一改为「行主序扁平存储（T *）+ 运行期列跨度 cap」，
 * 经本头文件宏访问，列向内存随 cap 收敛（square 矩阵由 cap×SMAP_MAX_NUMNODES
 * 收敛为 cap×cap）。
 *
 * 约束：cap 在初始化（InitTrackingDev）后只读，访问宏无锁、无分支；i/j 必须
 * 已通过 < cap 校验（入口校验不变式）。
 * 典型用法：
 *     uint32_t *m = calloc(NM_COUNT(rows), sizeof(*m));   // rows × cap
 *     NM_AT(m, i, j) = x;        // 等价于原 m[i][j] = x;
 *     memset_s(NM_ROW(m, i), NM_ROW_BYTES(m), 0, NM_ROW_BYTES(m)); // 单行清零
 *     free(m);
 */
#ifndef SMAP_NODE_MATRIX_H
#define SMAP_NODE_MATRIX_H

#include <stddef.h>

#include "manage.h"

/* 行跨度 = cap（列数） */
#define NM_STRIDE() ((size_t)GetNumaCap())

/* (i, j) 的扁平下标 */
#define NM_INDEX(i, j) ((size_t)(i)*NM_STRIDE() + (size_t)(j))

/* 元素访问：等价于原 data[i][j] */
#define NM_AT(data, i, j) ((data)[NM_INDEX(i, j)])

/* 第 i 行首地址（行级 memcpy/memset 用） */
#define NM_ROW(data, i) ((data) + (size_t)(i)*NM_STRIDE())

/* 单行字节数 / 分配元素总数（rows 行 × cap 列） */
#define NM_ROW_BYTES(data) (NM_STRIDE() * sizeof((data)[0]))
#define NM_COUNT(rows) ((size_t)(rows)*NM_STRIDE())

#endif /* SMAP_NODE_MATRIX_H */
