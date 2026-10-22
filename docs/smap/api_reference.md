# SMAP API 参考

本文在新版文档结构中完整保留原 API 参考内容。各接口的参数、返回值、错误、约束、附注和示例均按原文呈现，文末保留新版新增接口及补充说明。

## 阅读导航

- [生命周期](#ubturbo_smap_start-初始化smap)
- [迁出与迁回](#ubturbo_smap_migrate_out-配置进程迁出)
- [NUMA 与进程管理](#ubturbo_smap_remote_numa_info_set-设置远端numa内存可用量)
- [查询与跟踪](#ubturbo_smap_freq_query-查询进程冷热信息)

## ubturbo_smap_start: 初始化SMAP

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_start(uint32_t pageType, Logfunc extlog);
```

### 描述 DESCRIPTION

初始化SMAP，设置场景及迁移页面类型，虚拟化场景对应2M大页迁移，通算场景对应4K页迁移。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| pageType | IN | 页面类型：0：4K页。1：2M页。 |
| extlog | IN | Logfunc日志函数。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | 同进程重复初始化 |
| -EIO | 日志初始化失败 |
| -EBADF | 初始化异常 |
| -ENOMEM | 内存申请失败 |
| -EACCES | 其它进程已初始化 |
| -ENODEV | 内核驱动未安装 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* 不能重复初始化。
* pageType需要和当前场景匹配。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成SMAP的初始化。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret = SmapInit(1, NULL);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_stop: 停止SMAP

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_stop(void);
```

### 描述 DESCRIPTION

停止SMAP，释放资源（包含移除管理的pid迁出列表、enable状态位等）。

### 参数 Parameters

不涉及。

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | 已停止 |

### 约束 CONSTRAINTS

不涉及。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成SMAP的停止。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret = ubturbo_smap_stop();
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_migrate_out: 配置进程迁出

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_migrate_out(struct MigrateOutMsg *msg, int pageType);
```

### 描述 DESCRIPTION

配置虚拟机/进程的远端NUMA和迁出比例。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.count | IN | 配置数量。 |
| msg.payload | IN | 配置数组。 |
| msg.payload.srcNid | IN | 近端迁出NUMA。 |
| msg.payload.pid | IN | 进程PID。 |
| msg.payload.count | IN | PID迁出配置数量。 |
| msg.payload.inner | IN | PID迁出配置数组。 |
| msg.payload.inner.destNid | IN | 远端numa。 |
| msg.payload.inner.ratio | IN | 迁出比例。 |
| msg.payload.inner.memSize | IN | 迁出大小，单位KB。 |
| msg.payload.inner.migrateMode | IN | 迁移模式，0：按比例， 1：按大小。 |
| pageType | IN | 页面类型：0=4K页，1=2M页。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -ESRCH | 部分进程不存在但其余进程配置成功 |
| -ENOMEM | 内存申请失败 |
| -EINVAL | 参数错误 |
| -EBADF | 内核驱动访问失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* pageType需要和当前场景匹配。
* HCCS代际远端NUMA最大值为21。
* UB代际远端NUMA最大值为21。
* 远端NUMA被禁用时无法配置迁出（调用SmapMigrateBack接口时会默认禁用远端NUMA）。
* 如果已配置某虚机的远端NUMA，后续配置不能改变虚机的远端NUMA，只能通过SmapMigratePidRemoteNuma接口改变远端NUMA。
* 配置pid内存迁出后，由SMAP线程异步迁移，在迁移周期到来时才会执行迁移操作。
* 配置PID内存迁出后，PID会被SMAP纳管并参与后续周期冷热迁移；冷热迁移依赖迁移目标NUMA存在可用空闲内存。对于2M huge page虚机场景，本地NUMA和远端NUMA均需要存在可用的空闲2M huge page；若本地NUMA空闲2M huge page不足，远端热页回迁或冷热交换可能无法执行；若远端NUMA空闲2M huge page不足，初始迁出或后续冷页迁出可能无法执行，实际迁移效果会受限。
* 建议在PID相关的本地NUMA和远端NUMA上均预留不低于计划迁移规模5%~10%的空闲内存；对于2M huge page虚机，该预留量应换算为对应数量的空闲2M huge page。上述预留值为部署建议，不是接口入参校验条件；调用成功仅表示策略配置成功，不保证后续每轮冷热迁移都能实际迁移页面。
* 4K进程迁移不支持远端多NUMA。
* 迁移会过滤掉共享页。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序配置PID为10253的虚机进程迁出到NUMA4上，迁出比例为25%。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct MigrateOutMsg payload = {
        .srcNid = 0,
        .pid = 10253,
        .count = 1,
        .inner = {
            .destNid = 4,
            .ratio = 25,
            .memSize = 0,
            .migrateMode = 0,
        };
    };
    struct MigrateOutMsg msg = {
        .count = 1,
        .payload = payload,
    };

    ret = ubturbo_smap_migrate_out(&msg, 1);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_migrate_out_grouped: 配置组级迁出策略

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_migrate_out_grouped(struct GroupedMigrateOutMsg *msg, int pageType);
```

### 描述 DESCRIPTION

为大规格弹性虚机场景配置组级内存迁出策略。同一PID可配置多个migration group，每个group包含source local NUMA集合、target remote NUMA集合、每个target的quota以及每个source local NUMA的本地保留水线。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.count | IN | 配置PID数量，取值范围为1到MAX_NR_GROUPED_MIGOUT。 |
| msg.payload | IN | grouped配置数组。 |
| msg.payload.pid | IN | 虚机PID。 |
| msg.payload.groupCount | IN | PID下的group数量，取值范围为1到MAX_MIGRATION_GROUP_NUM。 |
| msg.payload.groups | IN | migration group数组。 |
| msg.payload.groups.localCount | IN | 当前group的source local NUMA数量，取值范围为1到MAX_GROUP_LOCAL_NUMA。 |
| msg.payload.groups.locals | IN | 当前group的source local NUMA数组。 |
| msg.payload.groups.locals.nid | IN | source local NUMA ID。 |
| msg.payload.groups.locals.size | IN | 当前local NUMA的本地保留水线，单位KB。 |
| msg.payload.groups.targetCount | IN | 当前group的target remote NUMA数量，取值范围为1到MAX_GROUP_REMOTE_NUMA。 |
| msg.payload.groups.targets | IN | 当前group的target remote NUMA数组。 |
| msg.payload.groups.targets.nid | IN | target remote NUMA ID。 |
| msg.payload.groups.targets.size | IN | 当前target remote NUMA的最大驻留容量quota，单位KB。 |
| pageType | IN | 页面类型，需与当前场景匹配。 |

grouped迁出ABI结构如下：

```c
struct MigrationNode {
    int nid;
    uint64_t size; // locals: 本地最低保留水线; targets: 远端最大驻留容量, 单位KB
};

struct MigrationGroup {
    int localCount;
    struct MigrationNode locals[MAX_GROUP_LOCAL_NUMA];
    int targetCount;
    struct MigrationNode targets[MAX_GROUP_REMOTE_NUMA];
};

struct GroupedMigrateOutPayload {
    pid_t pid;
    int groupCount;
    struct MigrationGroup groups[MAX_MIGRATION_GROUP_NUM];
};

struct GroupedMigrateOutMsg {
    int count;
    struct GroupedMigrateOutPayload payload[MAX_NR_GROUPED_MIGOUT];
};
```

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EAGAIN | 远端NUMA被禁用，或已有grouped PID处于非IDLE状态暂不能更新 |
| -ESRCH | PID不存在，本接口会回滚已添加配置，不提供部分成功语义 |
| -ENOMEM | 内存申请失败 |
| -EINVAL | 参数错误 |
| -EBADF | 内核驱动访问失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 仅支持2M huge page虚机，不支持4K进程或普通进程。
* pageType需要和当前场景匹配。
* UB代际远端NUMA最大值为21。
* 远端NUMA被禁用时无法配置为grouped target（调用ubturbo_smap_migrate_back接口时会默认禁用远端NUMA）。
* 同一次调用内不能传入重复PID。
* group policy不能和普通ubturbo_smap_migrate_out policy混用；已按普通迁出接口管理的PID，不能再配置grouped policy。
* 已存在grouped policy的PID只有在进程状态为IDLE时才能更新配置。
* 同一PID内不同group之间不能复用同一个local NUMA。
* 同一个group内不能配置重复target NUMA。
* 每个target quota至少为2MB，小于2MB会返回-EINVAL。
* group policy配置时会基于/proc/&lt;pid&gt;/numa_maps初始化远端target的usedPages账本。
* 如果管理前PID已经使用remote NUMA，该remote NUMA必须被当前grouped policy的target管理，且remote resident pages不能超过对应target quota或shared target的quota总和，否则返回-EINVAL。
* 允许多个group共享同一个remote target；当前只维护容量级账本，不保证页级ownership。
* 配置PID内存迁出后，由SMAP线程异步迁移，在迁移周期到来时才会执行迁移操作。
* grouped policy配置后，PID会被SMAP纳管并参与后续周期冷热迁移；每个group的source local NUMA集合和target remote NUMA集合都需要存在可用空闲内存。若source local NUMA空闲2M huge page不足，远端热页回迁、冷热交换或group内promote可能无法执行；若target remote NUMA空闲2M huge page不足，初始迁出、冷页迁出或group内demote可能无法执行。
* 建议每个group的source local NUMA集合和target remote NUMA集合均预留不低于该group target quota总量5%~10%的空闲内存；对于2M huge page虚机，该预留量应换算为对应数量的空闲2M huge page。
* group内target remote NUMA还受groups[].targets[].size quota约束；即使target remote NUMA存在物理空闲内存，如果该group在对应target上的quota已满，也不会继续向该target迁入页面。
* groups[].locals[].size表示对应source local NUMA的本地保留水线，不代表系统实际可用空闲内存；部署或调度侧仍需保证对应local NUMA有足够空闲内存。
* 上述预留值为部署建议，不是接口入参校验条件；调用成功仅表示策略配置成功，不保证后续每轮冷热迁移都能实际迁移页面。
* grouped policy当前不支持smap_config持久化与恢复，SMAP重启后需要重新下发配置。
* 页面迁移会过滤掉共享页。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序为PID 10253配置两个migration group：group 0从本地NUMA0迁往远端NUMA4，group 1从本地NUMA1迁往远端NUMA5。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct GroupedMigrateOutMsg msg = {
        .count = 1,
        .payload = {
            {
                .pid = 10253,
                .groupCount = 2,
                .groups = {
                    {
                        .localCount = 1,
                        .locals = { { .nid = 0, .size = 1048576 } },
                        .targetCount = 1,
                        .targets = { { .nid = 4, .size = 2097152 } },
                    },
                    {
                        .localCount = 1,
                        .locals = { { .nid = 1, .size = 1048576 } },
                        .targetCount = 1,
                        .targets = { { .nid = 5, .size = 2097152 } },
                    },
                },
            },
        },
    };

    ret = ubturbo_smap_migrate_out_grouped(&msg, 1);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_remote_numa_info_set: 设置远端NUMA内存可用量

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_remote_numa_info_set(struct SetRemoteNumaInfoMsg *msg);
```

### 描述 DESCRIPTION

设置本地NUMA对于远端NUMA的内存可用量。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.srcNid | IN | 本地NUMA，-1代表所有本地NUMA共享。 |
| msg.destNid | IN | 远端NUMA。 |
| msg.size | IN | 内存可用量，单位MB。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |
| -EBADF | 配置同步到内核失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* HCCS代际远端NUMA最大值为21。
* UB代际远端NUMA最大值为21。
* 如果已配置某虚机的远端NUMA，后续配置不能改变虚机的远端NUMA，只能通过SmapMigratePidRemoteNuma接口改变远端NUMA。
* 当配置的pid可迁出的量大于借用内存量时，SMAP不会使用完所有的借用量，每个本地NUMA对应的远端借用都有MIN[借用量的5%，200MB]的量不会使用，这是为了迁移内存时能申请到新的内存页面。例如：numa0 numa1分别借用了2G和6G共8G，numa0借的2G的预留按5%来算是100M，numa1借的6G的预留是200M，合计一共300M。
* 此接口仅在水线场景中生效，且水线场景调用SmapMigrateOut接口前需通过此接口设置远端NUMA使用量才能迁出pid的内存。
* 如果未调用SetSmapRemoteNumaInfo接口，默认初始化size值为0。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序设置NUMA0可以使用NUMA4的内存量为1024MB。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct SetRemoteNumaInfoMsg msg = {
        .srcNid = 0,
        .destNid = 4,
        .size = 1024,
    };

    ret = ubturbo_smap_remote_numa_info_set(&msg);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_migrate_back: 迁回远端内存

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_migrate_back(struct MigrateBackMsg *msg);
```

### 描述 DESCRIPTION

将指定远端NUMA的内存迁移到同一远端NUMA的其他地址段或迁回本地NUMA。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.taskID | IN | 任务ID。 |
| msg.count | IN | 子任务数量。 |
| msg.payload | IN | 子任务配置。 |
| msg.payload.srcNid | IN | 源NUMA，远端NUMA。 |
| msg.payload.destNid | IN | 目的NUMA，本地NUMA，-1代表由SMAP决定目的NUMA。 |
| msg.payload.memid | IN | 迁回地址段的memid。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBADF | 系统调用失败 |
| -EAGAIN | 超时 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 不支持并发调用此接口，否则会引起内存归还失败。
* 远端NUMA和传入的地址段需要匹配。
* 调用此接口后，SMAP默认禁止指定远端NUMA的冷热流动，只允许迁回任务中的迁移。
* 若远端NUMA的其他地址段的空闲页面不够，迁移任务会失败。
* 虚拟化水线场景下，请调用此接口前，调用SetSmapRemoteNumaInfo接口通知SMAP更新借用内存量，保证远端NUMA有足够的内存空间。
* 内存碎片场景下，调用此接口，需由调用方预留足够内存空间，否则会迁回失败。
* 迁回任务为异步执行，执行状态在/sys/kernel/debug/smap/mb\_[taskID]中进行查询。
* 同一个远端NUMA不支持并发调用，并发调用可能导致迁移数据无法迁移干净。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将memid为1的借用内存从NUMA4迁回，目的NUMA由SMAP决定。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct MigrateBackMsg msg = {
        .taskId = 1,
        .count = 1,
        .payload = {
            .srcNid = 4,
            .destNid = -1,
            .memid = 1,
        }
    };

    ret = ubturbo_smap_migrate_back(&msg);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_remove: 移除SMAP对进程的管理

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_remove(struct RemoveMsg *msg, int pageType);
```

### 描述 DESCRIPTION

移除指定的虚机/进程的远端numa，当远端numa全被移除时，整个进程被移除SMAP管理。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.count | IN | 进程数量。 |
| msg.payload | IN | 进程信息。 |
| msg.payload.pid | IN | 进程PID。 |
| msg.payload.count | IN | 要移除进程的远端numa个数。 |
| msg.payload.nid | IN | 要移除进程的远端numa数组 |
| pageType | IN | 页面类型：0=4K页，1=2M页。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBADF | 处理异常 |
| -ENOMEM | 申请内存失败 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* pageType需要和当前场景匹配。
* 当调用SmapMigrateBack接口迁回完所有地址后，需使用SmapRemove接口移除虚机管理，保证后续流程正常。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将PID为10253的虚机进程，虚机远端numa为4，从SMAP管理中移除。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct RemoveMsg msg = {
        .count = 1,
        .payload = {
            .pid = 10253,
            .count = 1,
            .nid = { 4 },
        }
    };

    ret = ubturbo_smap_remove(&msg, 1);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_node_enable: 启用NUMA迁移

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_node_enable(struct EnableNodeMsg *msg);
```

### 描述 DESCRIPTION

启用NUMA迁入，允许其它NUMA的内存向该NUMA进行迁移。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.enable | IN | 0：停用，1：启用。 |
| msg.nid | IN | 远端NUMA。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 此接口与SmapMigrateBack接口配合使用，目的是在SmapMigrateBack接口调用后，恢复对应远端NUMA的冷热流动。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序禁用了NUMA4上内存相关的迁移。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct EnableNodeMsg msg = {
        .enable = 0,
        .nid = 4,
    };

    ret = ubturbo_smap_node_enable(&msg);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_freq_query: 查询进程冷热信息

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_freq_query(int pid, uint16_t *data, uint32_t lengthIn, uint32_t *lengthOut, int dataSource);
```

### 描述 DESCRIPTION

查询进程冷热信息。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| pid | IN | 进程PID。 |
| data | OUT | 访存数组。 |
| lengthIn | IN | 数组长度。 |
| lengthOut | OUT | 实际数组长度。 |
| dataSource | IN | 数据来源，0代表数据来自周期性冷热迁移时的统计，1代表数据来自独立的统计。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |
| -EAGAIN | 统计模式扫描时长未达到预期 |
| -ENOMEM | 内核态内存申请失败 |
| -EBADF | 内核ioctl访问失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* dataSource为0表示先调用ubturbo_smap_migrate_out接口, 后续可获取到最近一个周期的冷热数据。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序查询了PID为10253，虚机内存规格为2GB的虚机进程的访存数据。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    int pid = 10253;
    uint16_t data[1024] = { 0 };
    uint32_t lengthIn = 1024;
    uint32_t lengthOut;

    ret = ubturbo_smap_freq_query(pid, data, lengthIn, &lengthOut, 1);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_run_mode_set: 设置SMAP运行模式

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_run_mode_set(int runMode);
```

### 描述 DESCRIPTION

设置SMAP运行模式。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| runMode | IN | 运行模式，0：水线场景，1：内存碎片场景。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBADF | 同步配置文件失败 |
| -EINVAL | 参数错误或非大页场景设置内存碎片模式 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 未设置的情况下，默认为水线场景。
* 如果是4K场景，不支持设置SMAP运行模式为内存碎片模式。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序设置了SMAP运行模式为水线场景。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;

    ret = ubturbo_smap_run_mode_set(0);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_process_migrate_enable: 启用/禁用进程的内存迁移

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_process_migrate_enable(pid_t *pidArr, int len, int enable, int flags);
```

### 描述 DESCRIPTION

启用/禁用PID对应虚机的冷热迁移和迁回。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| pidArr | IN | 进程PID数组。 |
| len | IN | 数组长度。 |
| enable | IN | 0：禁用，1：启用。 |
| flags | IN | 保留字段。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |
| -ETIMEDOUT | 超时 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* flags为保留字段，暂未使用。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序禁用了PID为10253的进程的内存迁移。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    pid_t pidArr[1] = { 10253 };

    ret = ubturbo_smap_process_migrate_enable(pidArr, 1, 0, 0);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_remote_numa_migrate: 迁移远端NUMA到另一远端NUMA

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_remote_numa_migrate(struct MigrateNumaMsg *msg);
```

### 描述 DESCRIPTION

迁移远端NUMA到另一远端NUMA。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.srcNid | IN | 源NUMA。 |
| msg.destNid | IN | 目的NUMA。 |
| msg.count | IN | memid数量。 |
| msg.memids | IN | memid数组。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBADF | 迁移成功但修改进程远端NUMA失败 |
| -ENOMEM | 迁移失败 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 传入的地址段需要和源NUMA ID对应的地址段匹配。
* 调用该接口前必须调用SmapEnableProcessMigrate禁用pid迁移功能。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将NUMA4上memid为1的内存迁移到NUMA5上。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct MigrateNumaMsg msg = {
        .srcNid = 4,
        .destNid = 5,
        .count = 1,
        .memids = { 1 },
    };

    ret = ubturbo_smap_remote_numa_migrate(&msg);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_pid_remote_numa_migrate: 迁移进程远端NUMA到另一远端NUMA

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_pid_remote_numa_migrate(pid_t *pidArr, int len, int srcNid, int destNid);
```

### 描述 DESCRIPTION

迁移进程远端NUMA到另一远端NUMA。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 迁移PID远端NUMA的消息。|
| msg.count | IN | 进程数量。 |
| msg.payload | IN | 进程迁移配置 |
| msg.payload[].pid | IN | 进程PID |
| msg.payload[].srcNid | IN | PID源远端NUMA |
| msg.payload[].destNid | IN | PID目的端远端NUMA |
| msg.payload[].ratio | IN | 迁移比例 |
| msg.payload[].srcNid | IN | 迁移大小 |
| msg.payload[].migrateMode | IN | 迁移模式 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -ENXIO | srcNid不是PID的远端NUMA |
| -EBADF | 迁移成功但修改进程远端NUMA失败 |
| -ENOMEM | 内存申请失败 |
| -EINVAL | 参数错误 |
| -REMOTE_MIG_FAIL | 迁移失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 目的NUMA ID内存余量充足。
* 不支持重复调用。
* 调用该接口前必须调用SmapEnableProcessMigrate禁用pid迁移功能。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将PID为10253的进程在NUMA4上的内存迁移进程总内存的25%到NUMA5上。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    pid_t pidArr[1] = { 10253 };
    struct MigrateEscapeMsg msg =  {
        .count = 1,
        .payload = {
            .pid = 10253,
            .srcNid = 4;
            .destNid = 5,
            .ratio = 25,
            .memSize = 0,
            .migrateMode = 0,
        };
    };

    ret = ubturbo_smap_pid_remote_numa_migrate(&msg);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_process_tracking_add: 添加进程扫描

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_process_tracking_add(pid_t *pidArr, uint32_t *scanTime, uint32_t *duration, int len, int scanType);
```

### 描述 DESCRIPTION

通知SMAP添加进程扫描，并设置扫描周期参数。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| pidArr | IN | 进程PID数组。 |
| scanTime | IN | 扫描间隔，单位ms，最大2000。 |
| duration | IN | 扫描持续时长，scanType为2时有效。 |
| len | IN | 数组长度。 |
| scanType | IN | 0：将进程设置为只扫描状态，1：将进程恢复为冷热扫描加迁移状态，2：表示进程设置为统计特定时长冷热信息状态。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBADF | 内核调用失败 |
| -ENOMEM | 内存申请失败 |
| -EINVAL | 参数错误 |
| -EBUSY | 进程状态非PROC_MOVE无法切换扫描类型 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 当进程未被SMAP纳管时，可以调用该接口，此时scanType不能传1。
* 当进程已经被SMAP纳管时，须先停止冷热迁移，然后才可以调用该接口，scanType可以传0/1/2。
* scanType传1的情况为进程已被smap纳管，需要从只扫描状态恢复到冷热扫描加迁移状态。
* 当进程未被SMAP纳管时，只允许进程使用本地numa。
* scanType传2为统计扫描频次场景，如果查询到的频次数据与期望相差较大，
  同时dmesg日志有"pid[xx] scan cost xxms exceeded expected scan time:xxms"，
  表示当前扫描耗时已超过配置的scanTime，需要重新配置合适的scanTime。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将PID为10253的进程添加到扫描中，每50ms扫描一次，持续3s。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    pid_t pidArr[1] = { 10253 };
    uint32_t scanTime[1] = { 50 };
    uint32_t duration[1] = { 3 };

    ret = ubturbo_smap_process_tracking_add(pidArr, scanTime, duration, 1, 2);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_process_tracking_remove: 移除进程扫描

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_process_tracking_remove(pid_t *pidArr, int len, int flags);
```

### 描述 DESCRIPTION

通知SMAP移除进程扫描。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| pidArr | IN | 进程PID数组。 |
| len | IN | 数组长度。 |
| flags | IN | 保留字段。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |
| -ENOMEM | 内存申请失败 |
| -EBADF | 内核ioctl移除PID失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 只有通过SmapAddProcessTracking接口设置的scanType为0或2的pid才能被这个接口移除。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序将PID为10253的进程从扫描中移除。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    pid_t pidArr[1] = { 10253 };

    ret = ubturbo_smap_process_tracking_remove(pidArr, 1, 0);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_migrate_out_sync: 配置进程迁出（同步）

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_migrate_out_sync(struct MigrateOutMsg *msg, int pageType, uint64_t maxWaitTime);
```

### 描述 DESCRIPTION

通知SMAP调用内存同步迁出接口。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| msg | IN | 配置参数。 |
| msg.count | IN | 配置数量。 |
| msg.payload | IN | 配置数组。 |
| msg.payload.destNid | IN | 远端NUMA。 |
| msg.payload.pid | IN | 进程PID。 |
| msg.payload.ratio | IN | 迁出比例。 |
| msg.payload.memSize | IN | 迁出大小，单位KB。 |
| msg.payload.migrateMode | IN | 迁移模式，0：按比例， 1：按大小。 |
| pageType | IN | 页面类型：0=4K页，1=2M页。 |
| maxWaitTime | IN | 最大等待时间，单位ms。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EBUSY | 超时 |
| -EINVAL | 参数错误 |
| -ESRCH | pid无效或部分pid无效 |
| -ENOMEM | 内存申请失败 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 只支持在虚拟化场景调用。
* 只支持内存池化场景。
* 本接口同步完成初始迁出后，PID仍会被SMAP纳管并参与后续周期冷热迁移；冷热迁移依赖迁移目标NUMA存在可用空闲内存。对于2M huge page虚机场景，本地NUMA和远端NUMA均需要存在可用的空闲2M huge page；若本地NUMA空闲2M huge page不足，远端热页回迁或冷热交换可能无法执行；若远端NUMA空闲2M huge page不足，后续冷页迁出可能无法执行。
* 建议在PID相关的本地NUMA和远端NUMA上均预留不低于计划迁移规模5%~10%的空闲内存；对于2M huge page虚机，该预留量应换算为对应数量的空闲2M huge page。上述预留值为部署建议，不是接口入参校验条件；调用成功仅表示策略配置成功，不保证后续每轮冷热迁移都能实际迁移页面。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序配置PID为10253的虚机进程迁出到NUMA4上，迁出大小为100MB，最多等待60s。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct MigrateOutMsg payload = {
        .srcNid = 0,
        .pid = 10253,
        .count = 1,
        .inner = {
            .destNid = 4,
            .ratio = 0,
            .memSize = 102400,
            .migrateMode = 1,
        };
    };
    struct MigrateOutMsg msg = {
        .count = 1,
        .payload = payload,
    };

    ret = ubturbo_smap_migrate_out_sync(&msg, 1, 60000);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_process_config_query: 根据远端NUMA查询进程配置

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
int ubturbo_smap_process_config_query(int nid, struct ProcessPayload *result, int inLen, int *outLen);
```

### 描述 DESCRIPTION

根据远端NUMA查询进程配置。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| nid | IN | 远端NUMA。 |
| result | OUT | 保存结果的数组。 |
| inLen | IN | 数组长度。 |
| outLen | OUT | 实际长度。 |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

| Error | Description |
| --- | --- |
| -EPERM | SMAP未初始化 |
| -EINVAL | 参数错误 |

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 切换场景时需要删除SMAP配置文件`/dev/shm/smap_config`。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序查询配置了远端NUMA为4的进程配置。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;
    struct ProcessPayload result[100] = { 0 };

    ret = ubturbo_smap_process_config_query(4, &result, 100, &len);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_urgent_migrate_out: 紧急迁移

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
void ubturbo_smap_urgent_migrate_out(uint64_t size);
```

### 描述 DESCRIPTION

紧急迁移。

### 参数 Parameters

| name | IN/OUT | description |
| --- | --- | --- |
| size | IN | 内存迁移量，单位为字节。 |

### 返回值 RETURN VALUE

不涉及。

### 错误 ERRORS

不涉及。

### 约束 CONSTRAINTS

* SMAP初始化后才能调用。
* 在OOM场景下由上层组件调用。
* 紧急迁出按 numa\_maps 段级过滤收集候选页，无法识别共享页归属，段内共享页可能被一并迁到远端；OOM 场景首要目标是压低本地水线、避免 kill，允许共享页短暂误迁。水线下降后，调用方应把相关 pid 重新加入 SMAP 管理，SMAP 管理态扫描会按 pidType/pageType 纠正共享页归属。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序紧急迁出1MB内存到远端。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    int ret;

    ret = ubturbo_smap_urgent_migrate_out(1048576);
    if (ret != 0) {
        return ret;
    }

    return 0;
}
```

## ubturbo_smap_is_running: 查询SMAP的运行状态

### 库 LIBRARY

SMAP库 (libsmap.so)

### 摘要 SYNOPSIS

```c
#include "smap_interface.h"
bool ubturbo_smap_is_running(void);
```

### 描述 DESCRIPTION

查询SMAP是否正在运行。

### 参数 Parameters

不涉及。

### 返回值 RETURN VALUE

返回 `true` 正在运行，返回`false`表示未运行。

### 错误 ERRORS

不涉及。

### 约束 CONSTRAINTS

不涉及。

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序查询了SMAP的运行状态。

```c
#include <stdio.h>
#include "smap_interface.h"

int main(void)
{
    bool ret;

    ret = ubturbo_smap_is_running();

    return 0;
}
```

## 新版补充说明


### 基本约定

接口声明在 `src/user/smap_interface.h`，由 `libsmap.so` 提供。除特别说明外，返回 `0` 表示请求成功，
负 errno 或其他非零值表示失败。调用方必须使用与库同版本的头文件。

- `pageType=0`：4K 普通进程页面。
- `pageType=1`：2M 静态大页虚机。
- 所有 `msg`、数组和输出长度指针必须非空并满足头文件上限。
- “请求成功”不一定代表异步页面迁移已收敛到目标。

### 生命周期

```c
int ubturbo_smap_start(uint32_t pageType, Logfunc extlog);
int ubturbo_smap_stop(void);
bool ubturbo_smap_is_running(void);
```

`start` 初始化日志、配置、设备和工作线程。同一进程不得重复初始化；其他进程占用共享状态时可能返回
`-EACCES`。`extlog` 可以为空；非空回调必须线程安全且不能阻塞 SMAP。

`stop` 结束 SMAP 实例。停止前调用方应阻止新增操作。`is_running` 仅报告库内运行状态，不代表所有
内核模块和远端资源均健康。

### 迁出

```c
int ubturbo_smap_migrate_out(struct MigrateOutMsg *msg, int pageType);
int ubturbo_smap_migrate_out_grouped(struct GroupedMigrateOutMsg *msg,
                                     int pageType);
int ubturbo_smap_migrate_out_sync(struct MigrateOutMsg *msg, int pageType,
                                  uint64_t maxWaitTime);
void ubturbo_smap_urgent_migrate_out(uint64_t size);
```

#### 普通迁出消息

`MigrateOutMsg.count` 是 PID 数量，不能超过 `MAX_NR_MIGOUT`。每个
`MigrateOutPayload` 包含 PID 和多个远端目标；`inner[].memSize` 的单位是 KB，迁移模式决定使用比例
还是大小。

- 同一 PID 可以配置多个远端 NUMA。
- 一次调用完整替换该 PID 的普通迁出目标；目标数量为 0 表示清空。
- 普通路径中的 `srcNid` 仅为 ABI 兼容保留，不参与本地 NUMA 选择。
- 容量不足时配置可被保存，但页面按可用容量逐步收敛。

分组迁出使用 `GroupedMigrateOutMsg` 描述 local 集合、目标集合、本地保留水线和远端容量，目前仅用于
接口声明支持的 2M 虚机场景。同步接口的 `maxWaitTime` 单位为 ms：`0` 表示不设等待上限，调用线程
可能长期阻塞；非零值必须在 10000–180000 ms 范围内。

紧急迁出的 `size` 单位为字节。该接口无返回值，不能写成通过返回码判断成功。它以缓解本地内存压力为优先，可能按
`numa_maps` 段收集页面；调用方需在压力解除后恢复常规管理并核对共享页归属。

### 迁回与移除

```c
int ubturbo_smap_migrate_back(struct MigrateBackMsg *msg);
int ubturbo_smap_remove(struct RemoveMsg *msg, int pageType);
```

`MigrateBackMsg` 通过 `taskID` 和不超过 `MAX_NR_MIGBACK` 的地址段描述迁回任务。移除消息以 PID 为
单位；payload 中 NUMA 数量为 0 表示整体删除，大于 0 表示删除指定远端 NUMA 的普通配置。

迁回与移除不是同义操作：迁回改变页面位置，移除改变 SMAP 管理状态。

### NUMA 容量与开关

```c
int ubturbo_smap_remote_numa_info_set(struct SetRemoteNumaInfoMsg *msg);
int ubturbo_smap_node_enable(struct EnableNodeMsg *msg);
int ubturbo_smap_run_mode_set(int runMode);
```

`SetRemoteNumaInfoMsg.size` 表示可使用容量，单位为 MiB。`srcNid=-1` 表示该远端目标可供每个本地
NUMA 使用。容量、拓扑和开关由可信资源管理器维护。

### 进程跟踪

```c
int ubturbo_smap_process_tracking_add(pid_t *pidArr, uint32_t *scanTime,
                                      uint32_t *duration, int len,
                                      int scanType);
int ubturbo_smap_process_tracking_remove(pid_t *pidArr, int len, int flag);
int ubturbo_smap_process_migrate_enable(pid_t *pidArr, int len, int enable,
                                        int flags);
```

三个输入数组的有效元素数均由 `len` 决定。`scanTime` 是扫描间隔，单位为 ms，范围为 50–2000 且
必须是 50 的倍数；`duration` 是统计持续时间，单位为秒，范围为 1–300。PID 退出、重复添加和停止
期间添加都应作为可预期错误处理。

### 查询

```c
int ubturbo_smap_freq_query(int pid, uint16_t *data, uint32_t lengthIn,
                            uint32_t *lengthOut, int dataSource);
int ubturbo_smap_process_config_query(int nid,
                                      struct OldProcessPayload *result,
                                      int inLen, int *outLen);
int ubturbo_smap_remote_numa_freq_query(uint16_t *numa, uint64_t *freq,
                                       uint16_t length);
```

`freq_query` 的 `dataSource` 可选普通管理数据或显式统计数据。输出数组由调用方提供；调用前设置容量，
返回后使用实际长度，不能在失败时读取数组。

`remote_numa_freq_query` 为每个输入 NUMA 写入对应频率，是现有头文件公开接口，旧文档曾遗漏。

### 远端到远端迁移

```c
int ubturbo_smap_remote_numa_migrate(struct MigrateNumaMsg *msg);
int ubturbo_smap_same_remote_numa_migrate(struct MigrateNumaMsg *msg);
int ubturbo_smap_pid_remote_numa_migrate(struct MigrateEscapeMsg *msg);
```

`MigrateNumaMsg` 包含源/目的 NUMA 及不超过 `MAX_NR_MIGNUMA` 的 `memid` 列表。PID 版本使用
`MigrateEscapeMsg` 描述进程、源/目的 NUMA、比例或大小。

`ubturbo_smap_same_remote_numa_migrate` 是当前公开头文件中的独立接口，旧文档曾遗漏；调用方不能用
普通远端迁移接口名替代它。

### 主要上限

| 常量 | 值/来源 | 作用 |
| --- | --- | --- |
| `MAX_NR_MIGRATE_ESCAPE` | 300 | PID 远端迁移 payload 上限 |
| `MAX_NR_MIGBACK` | 50 | 迁回 payload 上限 |
| `MAX_NR_MIGNUMA` | 50 | NUMA 迁移 memid 上限 |
| `MAX_MIGRATION_GROUP_NUM` | 8 | 每个 PID 的迁移分组上限 |
| `MIN_WAIT_TIME` | 10000 ms | 同步等待下限常量 |
| `MAX_WAIT_TIME` | 180000 ms | 同步等待上限常量 |
| `MIN_SCAN_TIME` | 50 ms | 扫描间隔下限常量 |
| `MAX_SCAN_TIME` | 2000 ms | 扫描间隔上限常量 |

其他数组上限由 `smap_env.h` 与 `smap_config.h` 提供，应使用宏而不是在调用方复制数字。

### 最小生命周期示例

```c
#include "smap_interface.h"

int main(void)
{
    int ret = ubturbo_smap_start(0, NULL);
    if (ret != 0) {
        return ret;
    }

    /* Configure authorized NUMA capacity and PIDs here. */

    return ubturbo_smap_stop();
}
```
