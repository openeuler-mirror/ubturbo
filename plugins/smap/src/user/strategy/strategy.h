/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef __STRATEGY_H__
#define __STRATEGY_H__

#include "manage/manage.h"
#include "smap_user_log.h"

#define FREE_BYTES_INDEX 3
#define KB_TO_PAGE_SHIFT 2

static inline void CalcMaxMigrate(uint divisor, uint64_t total, uint64_t *res)
{
    *res = total / divisor;
}
static inline uint32_t NrLocalPage(ProcessAttr *process)
{
    uint32_t pages = 0;
    /* 本地节点数提到循环外：避免循环条件重复调用 GetNrLocalNuma() */
    int nrLocalNuma = GetNrLocalNuma();
    for (int n = 0; n < nrLocalNuma; n++) {
        if (NotInAttrL1(process, n)) {
            continue;
        }
        pages += process->walkPage.nrPages[n];
    }
    return pages;
}

static inline uint32_t NrRemotePage(ProcessAttr *process)
{
    uint32_t pages = 0;
    int cap = (int)GetNumaCap();
    for (int n = GetNrLocalNuma(); n < cap; n++) {
        if (NotInAttrL2(process, n)) {
            continue;
        }
        pages += process->walkPage.nrPages[n];
    }
    return pages;
}

static inline int CheckActcDataValid(ProcessAttr *process)
{
    bool nullFlag = true;
    int cap = (int)GetNumaCap();
    for (int n = 0; n < cap; n++) {
        nullFlag &= !process->scanAttr.actcData[n];
    }
    if (nullFlag) {
        SMAP_LOGGER_INFO("The actcData is null, pid %d stops migrate out.", process->pid);
        return -ENODATA;
    }
    return 0;
}

/*
 * 迁移列表矩阵按策略实际访问的方向做稀疏布局，不再分配 cap × cap 全矩阵：
 *  - 本地行（from < nrLocalNuma）：分配全部 cap 列，覆盖 local->local 组内重平衡与 local->remote 迁出；
 *  - 远端行（from >= nrLocalNuma）：只分配本地列，覆盖 remote->local 迁回；
 *  - remote->remote 不会被策略产生，因此不分配。
 * 元素数 = nrLocal*cap + (cap-nrLocal)*nrLocal；cap=1024、nrLocal=4 时约 8176，为原 cap*cap 的 1/128。
 */
static inline size_t MigListMatrixCount(void)
{
    size_t nrLocal = (size_t)GetNrLocalNuma();
    size_t cap = (size_t)GetNumaCap();
    if (nrLocal > cap) {
        nrLocal = cap;
    }
    return nrLocal * cap + (cap - nrLocal) * nrLocal;
}

/* 取 (from, to) 对应的迁移列表单元，调用方必须保证该方向已在布局中分配
 * （from < nrLocalNuma 或 to < nrLocalNuma） */
static inline struct MigList *MigListCell(struct MigList *base, int from, int to)
{
    size_t nrLocal = (size_t)GetNrLocalNuma();
    size_t cap = (size_t)GetNumaCap();
    if ((size_t)from < nrLocal) {
        return base + (size_t)from * cap + (size_t)to;
    }
    return base + nrLocal * cap + ((size_t)from - nrLocal) * nrLocal + (size_t)to;
}

int RunStrategy(ProcessAttr *process, struct MigList *mlist, size_t mlistSize);
uint64_t GetNrFreePagesByNode(int nid);
uint64_t GetNrFreeHugePagesByNode(int nid);

#endif /* __STRATEGY_H__ */
