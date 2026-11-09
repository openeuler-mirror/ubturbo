/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef __NUMA_NODES_H__
#define __NUMA_NODES_H__

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "securec.h"
#include "common.h"

#define NUMA_NO_NODE (-1)

/* 运行期节点空间上限（g_numaCap 定义见 manage.c）。manage.h 依赖本头文件，
 * 此处就地定义避免循环包含；inline 化以消除 NM_AT/NM_COUNT 等宏在海量内层
 * 循环中反复调用外部访问器的函数调用开销 */
extern uint32_t g_numaCap;

static inline uint32_t GetNumaCap(void)
{
    return g_numaCap;
}

/* 单个 unsigned long 承载的位数（LP64 = 64） */
#define NUMA_BITS_PER_WORD ((int)(sizeof(unsigned long) * 8))

/*
 * NumaBitmap: 变长位图句柄。nWords 为 bits 数组长度（由运行期 cap 折算，
 * 见 NumaWordsForCap），bits 指向 nWords 个 unsigned long；不再按编译期
 * SMAP_MAX_NUMNODES 定长。
 * 句柄可按值浅拷贝（结构体内嵌/局部变量），但独立副本须经 NumaBitmapCopy
 * 深拷贝，所有权释放统一走 NumaBitmapFree（幂等）。
 * nid 作为全局位号（0 ~ cap-1）直接操作，本地/远端边界由运行期 nrLocalNuma 确定。
 */
typedef struct {
    uint32_t nWords; // bits 数组长度（unsigned long 个数），0 表示未绑定
    unsigned long *bits; // 指向 nWords 个 word；NULL 表示空位图
} NumaBitmap;

/* ---------- 位号换算与生命周期管理 ---------- */

#define NID_WORD(nid) ((nid) / NUMA_BITS_PER_WORD)
#define NID_MASK(nid) (1UL << ((nid) % NUMA_BITS_PER_WORD))

/* 运行期 cap 对应的位图字数 */
static inline uint32_t NumaWordsForCap(void)
{
    uint32_t cap = GetNumaCap();
    return (cap + (uint32_t)NUMA_BITS_PER_WORD - 1U) / (uint32_t)NUMA_BITS_PER_WORD;
}

/* 按运行期 cap 分配并清零位图；重复调用前须先 NumaBitmapFree */
static inline bool NumaBitmapAlloc(NumaBitmap *bm)
{
    if (bm == NULL) {
        return false;
    }
    uint32_t nWords = NumaWordsForCap();
    bm->bits = (unsigned long *)calloc(nWords, sizeof(unsigned long));
    if (bm->bits == NULL) {
        bm->nWords = 0;
        return false;
    }
    bm->nWords = nWords;
    return true;
}

/* 借用外部缓冲区（wire/持久化解码用），不接管所有权 */
static inline void NumaBitmapBind(NumaBitmap *bm, unsigned long *buf, uint32_t nWords)
{
    if (bm == NULL) {
        return;
    }
    bm->bits = buf;
    bm->nWords = nWords;
}

/* 释放位图并复位句柄（幂等，可重复调用） */
static inline void NumaBitmapFree(NumaBitmap *bm)
{
    if (bm == NULL) {
        return;
    }
    free(bm->bits);
    bm->bits = NULL;
    bm->nWords = 0;
}

/* 深拷贝：使 dst 持有与 src 等长的独立缓冲区并复制内容 */
static inline bool NumaBitmapCopy(NumaBitmap *dst, const NumaBitmap *src)
{
    if (dst == NULL || src == NULL) {
        return false;
    }
    if (src->bits == NULL || src->nWords == 0) {
        NumaBitmapFree(dst);
        return true;
    }
    if (dst->bits == src->bits) {
        /* 与 src 共享同一缓冲区（浅拷贝后未脱离）：先脱离，避免误释放 src 的缓冲区 */
        dst->bits = NULL;
        dst->nWords = 0;
    }
    size_t bytes = (size_t)src->nWords * sizeof(unsigned long);
    unsigned long *buf = (unsigned long *)malloc(bytes);
    if (buf == NULL) {
        return false;
    }
    if (memcpy_s(buf, bytes, src->bits, bytes) != EOK) {
        free(buf);
        return false;
    }
    free(dst->bits);
    dst->bits = buf;
    dst->nWords = src->nWords;
    return true;
}

/* ---------- 位操作基础函数 ---------- */

static inline bool NumaBitmapTest(const NumaBitmap *bm, int nid)
{
    int w = NID_WORD(nid);
    if (bm == NULL || bm->bits == NULL || nid < 0 || w >= (int)bm->nWords) {
        return false;
    }
    return (bm->bits[w] & NID_MASK(nid)) != 0;
}

static inline void NumaBitmapSet(NumaBitmap *bm, int nid)
{
    int w = NID_WORD(nid);
    if (bm == NULL || bm->bits == NULL || nid < 0 || w >= (int)bm->nWords) {
        return;
    }
    bm->bits[w] |= NID_MASK(nid);
}

static inline void NumaBitmapClear(NumaBitmap *bm, int nid)
{
    int w = NID_WORD(nid);
    if (bm == NULL || bm->bits == NULL || nid < 0 || w >= (int)bm->nWords) {
        return;
    }
    bm->bits[w] &= ~NID_MASK(nid);
}

static inline void NumaBitmapZero(NumaBitmap *bm)
{
    if (bm == NULL || bm->bits == NULL) {
        return;
    }
    (void)memset_s(bm->bits, (size_t)bm->nWords * sizeof(unsigned long), 0, (size_t)bm->nWords * sizeof(unsigned long));
}

/* ---------- L1（本地）/ L2（远端）辅助函数 ---------- */

/* 获取第一个 L1 nid（find first local），未命中返回 NUMA_NO_NODE */
static inline int NumaGetFirstL1(const NumaBitmap *bm, int nrLocalNuma)
{
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        if (NumaBitmapTest(bm, nid))
            return nid;
    }
    return NUMA_NO_NODE;
}

/* 获取第一个 L2 nid（find first remote），未命中返回 NUMA_NO_NODE */
static inline int NumaGetFirstL2(const NumaBitmap *bm, int nrLocalNuma)
{
    /* cap 之外位图恒为 0（写入侧均按 cap 界内），按 cap 扫描即可 */
    int cap = (int)GetNumaCap();
    for (int nid = nrLocalNuma; nid < cap; nid++) {
        if (NumaBitmapTest(bm, nid))
            return nid;
    }
    return NUMA_NO_NODE;
}

/* 判断 nid 是否在 L1 中 */
static inline bool NumaInL1(const NumaBitmap *bm, int nid, int nrLocalNuma)
{
    return nid >= 0 && nid < nrLocalNuma && NumaBitmapTest(bm, nid);
}

/* 判断 nid 是否在 L2 中 */
static inline bool NumaInL2(const NumaBitmap *bm, int nid, int nrLocalNuma)
{
    return nid >= nrLocalNuma && nid < (int)GetNumaCap() && NumaBitmapTest(bm, nid);
}

/* 清理 nid 对应位 */
static inline void NumaClearBit(NumaBitmap *bm, int nid)
{
    NumaBitmapClear(bm, nid);
}

/* 仅保留 L1（清除 L2） */
static inline void NumaClearL2(NumaBitmap *bm, int nrLocalNuma)
{
    int cap = (int)GetNumaCap();
    for (int nid = nrLocalNuma; nid < cap; nid++) {
        NumaBitmapClear(bm, nid);
    }
}

/* 仅保留 L2（清除 L1） */
static inline void NumaClearL1(NumaBitmap *bm, int nrLocalNuma)
{
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        NumaBitmapClear(bm, nid);
    }
}

/* 获取 L1 中节点数量 */
static inline int NumaGetL1Count(const NumaBitmap *bm, int nrLocalNuma)
{
    int count = 0;
    for (int nid = 0; nid < nrLocalNuma; nid++) {
        if (NumaBitmapTest(bm, nid))
            count++;
    }
    return count;
}

/* 获取 L2 中节点数量 */
static inline int NumaGetL2Count(const NumaBitmap *bm, int nrLocalNuma)
{
    int count = 0;
    int cap = (int)GetNumaCap();
    for (int nid = nrLocalNuma; nid < cap; nid++) {
        if (NumaBitmapTest(bm, nid))
            count++;
    }
    return count;
}

/* 遍历所有远端在线节点（定义见 manage.c，内容先写、g_nrRemote 最后发布） */
extern int g_remoteNids[SMAP_NODE_CAP_LIMIT];
extern int g_nrRemote;

#define for_each_remote_nid(nid)                                                                                 \
    for (int __remote_nid_i__ = 0; __remote_nid_i__ < g_nrRemote && ((nid) = g_remoteNids[__remote_nid_i__], 1); \
         __remote_nid_i__++)

#endif /* __NUMA_NODES_H__ */
