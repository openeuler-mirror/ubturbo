/*
 * Description: Scene-related operations
 * Create: 2025-6-17
 */
#include <stdint.h>
#include <stdio.h>

#include "user/manage/manage.h"
#include "manage/node_matrix.h"
#include "common.h"

#define LOCAL_NUMA_BITS 4      // 保留供测试逻辑使用
#define STUB_REMOTE_NUMA_NUM 18 // 与旧矩阵远端数量一致，BuildMigrationMsg 统计 4*18*2=144 页

int RunStrategyStub(ProcessAttr *process, struct MigList *mlist, size_t mlistSize)
{
    int i, j;

    /* 只填充本地 0..3 与远端 4..21 的组合（迁出与回迁双向），模拟真实策略输出范围 */
    for (i = 0; i < LOCAL_NUMA_BITS; i++) {
        for (j = LOCAL_NUMA_BITS; j < LOCAL_NUMA_BITS + STUB_REMOTE_NUMA_NUM; j++) {
            NM_AT(mlist, i, j).nr = 1;
            NM_AT(mlist, j, i).nr = 1;
        }
    }

    return 0;
}
