### ubturbo_smap_start

#### 函数定义

初始化SMAP，设置场景及迁移页面类型，虚拟化场景对应2M大页迁移，通算场景对应4K页迁移。

#### 实现方法

<pre class="screen"><p class="p" id="p294571314185">int ubturbo_smap_start(uint32_t pageType, Logfunc extlog);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| pageType | uint32\_t | {0,1} | 入参 | 页面类型：0：4K页。1：2M页。 |
| extlog | typedef void (\*Logfunc)(int level, const char \*str, const char \*moduleName); | 与数据类型匹配 | 入参 | Logfunc日志函数。 |

#### 返回值

* 成功返回0。
* 同进程重复初始化返回-1。
* 其它进程已初始化返回-13。
* SMAP初始化异常返回-9。
* 内存申请失败返回-12。
* SMAP内核驱动未安装返回-19。
* 参数错误返回-22。
* SMAP日志初始化失败返回-5。

#### 约束和注意事项

* 不能重复初始化。
* pageType需要和当前场景匹配。

### ubturbo_smap_stop

#### 约束和注意事项

初始化SMAP才能调用停止SMAP。

#### 函数定义

停止SMAP，释放资源（包含移除管理的pid迁出列表、enable状态位等）。

#### 实现方法

<pre class="screen"><p class="p" id="p199921138182">int ubturbo_smap_stop(void);</p></pre>

#### 参数说明

N/A

#### 返回值

* 成功返回0。
* 已停止返回-1。

### ubturbo_smap_migrate_out

#### 函数定义

配置虚拟机/进程的普通迁出目标。一个 PID 可同时配置多个远端 NUMA；
SMAP自动发现并维护该 PID 的受管本地 NUMA 集合，再按 local -> remote Pair 执行迁出、迁回和冷热优化。

#### 实现方法

<pre class="screen"><p class="p" id="p987171415182">int ubturbo_smap_migrate_out(struct MigrateOutMsg *msg, int pageType);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct MigrateOutMsg\* | 单次最多配置 40 个 PID；同一请求中的 PID 不能重复。每个 `payload` 的 `count` 取值为 0 到 `REMOTE_NUMA_NUM`，各 `inner[].destNid` 必须是在线且互不重复的远端 NUMA。一个 PID 的所有目标必须使用同一种 `migrateMode`；ratio 模式下各 remote 的 ratio 总和不超过 100；memSize 单位为 KB，必须按当前页型对齐（4K 或 2M）。 | 入参 | 普通迁出配置，详细语义见下文。 |
| pageType | int | {0,1}，且必须与 `ubturbo_smap_start` 的当前页型一致 | 入参 | 0：4K 普通进程；1：2M 虚机。 |

```
#define MAX_NR_MIGOUT 40 

struct MigrateOutPayloadInner {
    int destNid;
    int ratio;
    uint64_t memSize;
    MigrateMode migrateMode;
};

struct MigrateOutPayload {
    int srcNid; // 普通 migrate-out 忽略该字段，仅保留 ABI 兼容
    pid_t pid;
    int count;
    struct MigrateOutPayloadInner inner[REMOTE_NUMA_NUM];
};

struct MigrateOutMsg {
    int count;
    struct MigrateOutPayload payload[MAX_NR_MIGOUT];
};
```

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 内存申请失败返回-12。
* 参数错误返回-22。
* 进程不存在返回-3。

#### 约束和注意事项

* SMAP初始化后才能调用。
* pageType需要和当前场景匹配。
* 远端 NUMA 使用当前机器的动态拓扑范围校验，不存在固定的 NID 上限；目标必须是在线 remote NUMA。
* `srcNid` 不参与普通 migrate-out 的目标选择；本地 NUMA 由 SMAP 根据 CPU affinity、页面驻留和已有 Pair 账本自动维护，调用方无需、也不能通过本接口指定 local NUMA。
* 每次调用都是该 PID 普通迁出目标的**全量替换**：新请求中未列出的 remote 目标会清零；`payload.count == 0` 表示清空该 PID 的全部普通迁出目标。配置更新发生在该 PID 正在迁移时，新配置会在本轮迁移结果结算后原子生效。
* ratio 表示该 PID 在指定 remote 上的聚合目标比例；memSize 表示该 PID 在指定 remote 上的最终聚合驻留量，而不是本轮增量。两种模式均可用于普通多 local、多 remote 迁移，不能混用。
* 远端 NUMA 被禁用时，不能为其配置非零新目标；已配置目标和已有远端页会被保留，禁用期间不再向该 remote 迁出，但允许按需要迁回本地。
* 调用成功表示请求已保存，不表示本轮一定能迁出页面。远端容量不足、Pair 暂时无法分配或本地回迁空闲页不足时，SMAP 保留用户目标，并在容量或页面条件恢复后继续收敛。
* 配置pid内存迁出后，由SMAP线程异步迁移，在迁移周期到来时才会执行迁移操作。
* 配置 PID 内存迁出后，PID 会被 SMAP 纳管并参与后续周期冷热迁移。远端容量由`ubturbo_smap_remote_numa_info_set` 的 private/shared 容量合同跨 PID 统一仲裁；不再按 PID 配置顺序直接消费容量。
* Pair 未收敛时优先执行净迁出或迁回，不执行冷热 swap；多个 local 共享同一 remote 且无法可靠识别页面来源时也不执行该 Pair 的 swap。以上限制不影响净迁出和迁回。
* 建议在相关本地 NUMA 和远端 NUMA 上预留不低于计划迁移规模 5%~10% 的空闲内存；对于2M huge page 虚机应预留相应数量的空闲 2M huge page。该预留值是部署建议，不是接口校验条件。
* 迁移会过滤掉共享页。

### ubturbo_smap_migrate_out_grouped

#### 函数定义

配置大规格弹性虚机场景的组级迁出策略。同一PID可配置多个migration group，每个group包含source local NUMA集合、target remote NUMA集合、每个target的quota以及每个source local NUMA的本地保留水线。

#### 实现方法

<pre class="screen"><p class="p" id="pGroupedMigrateOut">int ubturbo_smap_migrate_out_grouped(struct GroupedMigrateOutMsg *msg, int pageType);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct GroupedMigrateOutMsg\* | 单次最多配置MAX_NR_GROUPED_MIGOUT个PID；同一次调用内PID不能重复。 | 入参 | grouped迁出配置。 |
| msg.count | int | 1到MAX_NR_GROUPED_MIGOUT的整数。 | 入参 | 配置PID数量。 |
| msg.payload[].pid | pid\_t | 有效虚机PID。 | 入参 | 虚机PID。 |
| msg.payload[].groupCount | int | 1到MAX_MIGRATION_GROUP_NUM的整数，当前最大为8。 | 入参 | 当前PID的group数量。 |
| msg.payload[].groups[].localCount | int | 1到MAX_GROUP_LOCAL_NUMA的整数，当前最大为4。 | 入参 | 当前group的source local NUMA数量。 |
| msg.payload[].groups[].locals[].nid | int | 当前机器有效local NUMA。 | 入参 | source local NUMA ID。 |
| msg.payload[].groups[].locals[].size | uint64\_t | 大于0，单位KB。 | 入参 | 当前local NUMA的本地保留水线。 |
| msg.payload[].groups[].targetCount | int | 1到MAX_GROUP_REMOTE_NUMA的整数。 | 入参 | 当前group的target remote NUMA数量。 |
| msg.payload[].groups[].targets[].nid | int | 当前机器有效remote NUMA，且未被禁用。 | 入参 | target remote NUMA ID。 |
| msg.payload[].groups[].targets[].size | uint64\_t | 单位KB，至少为2MB。 | 入参 | 当前target remote NUMA的最大驻留容量quota。 |
| pageType | int | 1 | 入参 | 进程pid类型，仅支持虚机2M页类型。 |

```
#define MAX_NR_GROUPED_MIGOUT MAX_NR_MIGOUT
#define MAX_MIGRATION_GROUP_NUM 8
#define MAX_GROUP_LOCAL_NUMA 4
#define MAX_GROUP_REMOTE_NUMA REMOTE_NUMA_NUM

struct MigrationNode {
    int nid;
    uint64_t size;
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

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 远端NUMA被禁用，或已有grouped PID处于非IDLE状态暂不能更新时返回-11。
* PID不存在返回-3，本接口会回滚已添加配置，不提供部分成功语义。
* 内存申请失败返回-12。
* 参数错误返回-22。
* 内核驱动访问失败返回-9。

#### 约束和注意事项

* SMAP初始化后才能调用。
* 仅支持2M huge page虚机，不支持4K进程或普通进程。
* pageType需要和当前场景匹配。
* UB代际远端NUMA最大值为21。
* 远端NUMA被禁用时无法配置为grouped target（调用ubturbo_smap_migrate_back接口时会默认禁用远端NUMA）。
* group policy不能和普通ubturbo_smap_migrate_out policy混用；已按普通迁出接口管理的PID，不能再配置grouped policy。
* 已存在grouped policy的PID只有在进程状态为IDLE时才能更新配置。
* 同一PID内不同group之间不能复用同一个local NUMA。
* 同一个group内不能配置重复target NUMA。
* 每个target quota至少为2MB。
* group policy配置时会基于/proc/&lt;pid&gt;/numa_maps初始化远端target的usedPages账本。
* 如果管理前PID已经使用remote NUMA，该remote NUMA必须被当前grouped policy的target管理，且remote resident pages不能超过对应target quota或shared target的quota总和，否则返回-22。
* 允许多个group共享同一个remote target；当前只维护容量级账本，不保证页级ownership。
* 配置PID内存迁出后，由SMAP线程异步迁移，在迁移周期到来时才会执行迁移操作。
* grouped policy配置后，PID会被SMAP纳管并参与后续周期冷热迁移；每个group的source local NUMA集合和target remote NUMA集合都需要存在可用空闲内存。若source local NUMA空闲2M huge page不足，远端热页回迁、冷热交换或group内promote可能无法执行；若target remote NUMA空闲2M huge page不足，初始迁出、冷页迁出或group内demote可能无法执行。
* 建议每个group的source local NUMA集合和target remote NUMA集合均预留不低于该group target quota总量5%~10%的空闲内存；对于2M huge page虚机，该预留量应换算为对应数量的空闲2M huge page。
* group内target remote NUMA还受groups[].targets[].size quota约束；即使target remote NUMA存在物理空闲内存，如果该group在对应target上的quota已满，也不会继续向该target迁入页面。
* groups[].locals[].size表示对应source local NUMA的本地保留水线，不代表系统实际可用空闲内存；部署或调度侧仍需保证对应local NUMA有足够空闲内存。
* 上述预留值为部署建议，不是接口入参校验条件；调用成功仅表示策略配置成功，不保证后续每轮冷热迁移都能实际迁移页面。
* grouped policy当前不支持smap_config持久化与恢复，SMAP重启后需要重新下发配置。
* 页面迁移会过滤掉共享页。

### ubturbo_smap_remote_numa_info_set

#### 函数定义

配置普通迁出的远端容量合同，而不是直接发起迁移。该容量在所有普通 PID 间统一仲裁。

#### 实现方法

<pre class="screen"><p class="p" id="p41243146185">int ubturbo_smap_remote_numa_info_set(struct SetRemoteNumaInfoMsg *msg);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct SetRemoteNumaInfoMsg\* | `srcNid` 为本地 NUMA ID 或 -1；`destNid` 为在线远端 NUMA ID；`size` 单位为 MB。 | 入参 | 配置一份 private 或 shared 远端容量。 |

```
struct SetRemoteNumaInfoMsg {
    int srcNid;
    int destNid;
    uint32_t size;
};
```

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 配置同步到内核失败返回-9。

#### 约束和注意事项

* SMAP初始化后才能调用。
* `destNid` 按当前机器的远端 NUMA 范围校验，并且必须能由 `numastat` 识别为在线节点；不存在固定的 NID 上限。
* `srcNid >= 0` 配置该 local -> remote Pair 的 private 容量；`srcNid == -1` 配置该 remote可由全部受管 local 共享的 shared 容量。
* 每个远端的普通迁出容量由所有 private 容量和 shared 容量组成。SMAP 先在对应 Pair 间仲裁private 容量，再将未满足需求参与同一 remote 的 shared 容量仲裁；容量不足会裁剪有效目标，但不会删除 PID 的原始 migrate-out 请求。
* 本接口可在普通 migrate-out 配置前后调用；未配置的 private/shared 容量默认为 0。容量恢复后，已保存的普通迁出目标会在后续周期自动重新参与分配。

### ubturbo_smap_migrate_back

#### 函数定义

将指定远端 NUMA 上、由 `memid` 标识的内存段迁回本地 NUMA。

#### 实现方法

<pre class="screen"><p class="p" id="p9161121421813">int ubturbo_smap_migrate_back(struct MigrateBackMsg *msg);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct MigrateBackMsg\* | `count` 为 1 到 `MAX_NR_MIGBACK`（当前为 50）。每个 payload 的 `srcNid` 必须是当前机器的远端 NUMA；`destNid` 必须是有效本地 NUMA，或为 -1 以由内核按任务 affinity/轮询选择本地 NUMA；`memid` 必须能解析为属于 `srcNid` 的已登记远端内存段。 | 入参 | 详细如下 |

```
#define MAX_NR_MIGBACK 50

struct MigrateBackPayload { 
    int srcNid; 
    int destNid; 
    uint64_t memid;
};

struct MigrateBackMsg { 
    unsigned long long taskID; 
    int count; 
    struct MigrateBackPayload payload[MAX_NR_MIGBACK]; 
};
```

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 系统调用失败返回-9。
* 参数错误返回-22。
* 超时返回-11。

#### 约束和注意事项

* SMAP初始化后才能调用。
* 不支持并发调用此接口，否则会引起内存归还失败。
* `srcNid` 是待迁回的远端 NUMA，不使用固定的 `[0,9]` 范围；有效范围随当前机器的local/remote NUMA 拓扑确定。
* `memid` 由内核解析为物理地址范围，调用方无需也不能传入 `paStart`/`paEnd`；该内存段必须属于 `srcNid`，否则调用失败。
* `destNid == -1` 时，内核优先按页面所属任务的 affinity 选择本地 NUMA，无法确定时在有足够空闲页的本地 NUMA 间轮询选择。
* 调用此接口后，SMAP默认禁止指定远端NUMA的冷热流动，只允许迁回任务中的迁移。
* 若目标本地 NUMA 的空闲页面不足，迁回任务会失败。
* 虚拟化水线场景下，如迁回会改变借用容量，应配合`ubturbo_smap_remote_numa_info_set` 更新相应的容量合同；需要预留的是目标本地 NUMA 的空间，而不是源远端 NUMA 的空间。
* 内存碎片场景下，调用方同样需要在目标本地 NUMA 预留足够内存空间，否则会迁回失败。
* 迁回任务为异步执行，执行状态在/sys/kernel/debug/smap/mb\_[taskID]中进行查询。
* 同一个远端NUMA不支持并发调用，并发调用可能导致迁移数据无法迁移干净。

### ubturbo_smap_remove

#### 函数定义

移除 PID 的 SMAP 管理状态，或仅删除普通迁出策略中的指定远端 NUMA 目标。该接口只更新管理、跟踪和配置状态，不会主动迁回页面。

#### 实现方法

<pre class="screen"><p class="p" id="p3301101481814">int ubturbo_smap_remove(struct RemoveMsg *msg, int pageType);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct RemoveMsg\* | `msg.count` 为 1 到 `MAX_NR_REMOVE`（当前为 40），同一请求中的 PID 不能重复。每个 `payload.count` 为 0 到 `REMOTE_NUMA_NUM`；大于 0 时 `nid[]` 中的项必须是互不重复的有效远端 NUMA。 | 入参 | 详细语义见下文。 |
| pageType | int | {0,1}，且必须与 `ubturbo_smap_start` 的当前页型一致 | 入参 | 0：4K 普通进程；1：2M 虚机。 |

```
#define MAX_NR_REMOVE 40

struct RemovePayload {
    pid_t pid;
    int count; // 0：整个 PID；大于 0：仅删除 nid[] 指定的 remote
    int nid[REMOTE_NUMA_NUM];
};

struct RemoveMsg { 
    int count; 
    struct RemovePayload payload[MAX_NR_REMOVE]; 
};
```

#### 返回值

* 成功返回0。
* SMAP未初始化或已停止返回-1。
* 参数错误、页型不匹配、remote NID 非法/重复、PID 重复，或对 grouped PID 执行局部删除时返回-22。
* 更新 tracking 设备失败时返回相应设备错误码。

#### 约束和注意事项

* SMAP初始化后才能调用。
* pageType需要和当前场景匹配。
* `payload.count == 0` 立即按 PID 整体删除 SMAP 管理状态；无需在请求中列出该 PID 的所有 remote。
* `payload.count > 0` 仅适用于普通 `ubturbo_smap_migrate_out` 策略，删除 `nid[]` 所列 remote 的目标、Pair 账本和 tracking 节点；删除后若该 PID 已没有 remote 目标，会自动按整体删除处理。
* grouped policy 只能使用 `payload.count == 0` 整体删除，不支持按 remote 局部删除。
* 本接口不迁回远端页面。调用局部或整体删除前，应先通过`ubturbo_smap_migrate_back` 或策略收敛将需要保留的远端页面迁回；否则删除后 SMAP 不再继续跟踪和管理这些页面。

### ubturbo_smap_node_enable

#### 函数定义

启用或禁用指定远端 NUMA 的普通迁移流动。

#### 实现方法

<pre class="screen"><p class="p" id="p183398143187">int ubturbo_smap_node_enable(struct EnableNodeMsg *msg);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct EnableNodeMsg\* | `nid` 必须是当前机器的远端 NUMA，即位于 `[nrLocalNuma, MAX_NODES)`；`enable` 仅允许 `DISABLE_NUMA_MIG`（0）或 `ENABLE_NUMA_MIG`（1）。 | 入参 | 详细如下 |

```
struct EnableNodeMsg { 
    int enable; // 0：禁用迁移，1：启用迁移
    int nid;    // 远端 NUMA ID
 };
```

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误（空消息、`nid` 非远端 NUMA 或 `enable` 非法）返回-22。
* 启用的 remote 仍有迁回任务在执行时返回-11。

#### 约束和注意事项

* SMAP初始化后才能调用。
* `enable == 0` 禁止向该 remote 产生新的迁出；已有页面仍可按需要迁回本地。
* `enable == 1` 清除用户禁用和已完成迁回留下的禁用状态，恢复对应 remote 的普通迁移流动。
* 本接口可与 `ubturbo_smap_migrate_back` 配合使用：迁回完成后重新启用该 remote；迁回任务未完成时启用会被拒绝，避免与迁回任务并发。

### ubturbo_smap_freq_query

#### 函数定义

查询进程冷热信息。

#### 实现方法

<pre class="screen"><p class="p" id="p1897593015511">int ubturbo_smap_freq_query(int pid, uint16_t *data, uint32_t lengthIn, uint32_t *lengthOut);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- |--- | --- |--- |
| pid | int | 对应pid需要存在。 | 入参 | 进程pid号。 |
| data | uint16\_t\* | 非空。 | 入参 | 存放冷热信息的数组。 |
| lengthIn | uint32\_t | 大于0 | 入参 | 指示data数组的大小。 |
| lengthOut | uint32\_t\* | 非空。 | 入参 | 返回实际写入data数组的大小。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 统计模式扫描时长未达到预期返回-11。
* 内核态内存申请失败返回-12。
* 内核ioctl访问失败返回-9。

#### 约束和注意事项

* SMAP初始化后才能调用。
* dataSource为0表示先调用ubturbo_smap_migrate_out接口, 后续可获取到最近一个周期的冷热数据。

### ubturbo_smap_run_mode_set

#### 函数定义

设置兼容运行模式。

#### 实现方法

<pre class="screen"><p class="p" id="p14201739155519">int ubturbo_smap_run_mode_set(int runMode);</p></pre>

#### 参数说明

**表1 ​**参数说明| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| - | - | - | - | - |
| ------------------------------------------------ |

| runMode | int | * 0：水线场景。* 1：内存碎片场景。* 其它值：返回错误。 | 入参 | 兼容已有场景配置。普通 `ubturbo_smap_migrate_out` 不再使用该值解释 ratio/memSize，也不按该值选择单 NUMA 或多 NUMA 路径。 |
| - | - | - | - | - |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 非大页场景设置内存碎片模式返回-22。
* 同步配置文件失败返回-9。

#### 约束和注意事项

* SMAP初始化后才能调用。
* 未设置的情况下，默认为水线场景。
* 如果是4K场景，不支持设置SMAP运行模式为内存碎片模式。
* `ubturbo_smap_migrate_out_sync` 仍要求内存池化模式；该约束不适用于普通异步 migrate-out。

### ubturbo_smap_process_migrate_enable

#### 函数定义

启用/禁用PID对应虚机的冷热迁移和迁回。

#### 实现方法

<pre class="screen" id="screen159847188323"><p class="p" id="p18353828165616">int ubturbo_smap_process_migrate_enable(pid_t *pidArr, int len, int enable, int flags);</p></pre>

#### 参数说明

**表1 ​**参数说明| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| - | - | - | - | - |
| ------------------------------------------------ |

| pidArr | pid\_t \* | NA | 入参 | 虚机PID数组。 |
| - | - | - | - | - |
| len | int | 1-100的整数 | 入参 | 虚机PID数组长度。 |
| enable | int | 0或1 | 入参 | * 0-禁用。* 1-启用。 |
| flags | int | NA | 入参 | 保留字段。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 超时返回-110。

#### 约束和注意事项

* flags为保留字段，暂未使用。

### ubturbo_smap_remote_numa_migrate

#### 函数定义

通知SMAP迁移远端NUMA的内存到另一个远端NUMA。

#### 实现方法

<pre class="screen" id="screen29172049124214"><p class="p" id="p522512464561">int ubturbo_smap_remote_numa_migrate(struct MigrateNumaMsg *msg);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct MigrateNumaMsg \* | NA | 入参 | 迁移远端NUMA的消息。 |
| msg.srcNid | int | 远端NUMA ID | 入参 | 源NUMA ID。 |
| msg.destNid | int | 远端NUMA ID | 入参 | 目的NUMA ID。 |
| msg.count | int | 1-50的整数 | 入参 | 源NUMA地址段数量。 |
| msg.payload | struct MigrateNumaPayload | 长度固定为50 | 入参 | 地址段信息。 |
| msg.payload[].paStart | uint64\_t | NA | 入参 | 地址段起始地址。 |
| msg.payload[].paEnd | uint64\_t | NA | 入参 | 地址段结束地址。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 迁移成功但修改进程远端NUMA失败返回-9。
* 迁移失败返回-12。
* 参数错误返回-22。

#### 约束和注意事项

* 传入的地址段需要和源NUMA ID对应的地址段匹配。
* 调用该接口前必须调用 `ubturbo_smap_process_migrate_enable` 禁用 PID 迁移功能。

### ubturbo_smap_pid_remote_numa_migrate

#### 函数定义

通知SMAP按PID级迁移远端内存到其他远端内存。

#### 实现方法

<pre class="screen"><p class="p" id="p53951577571">int ubturbo_smap_pid_remote_numa_migrate(struct MigrateEscapeMsg *msg);</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct MigrateEscapeMsg \* | NA | 入参 | 迁移PID远端NUMA的消息。|
| msg.count | int | 1-300的整数 | 入参 | 进程数量。|
| msg.payload | struct MigrateNumaPayload | 长度固定为300 | 入参 | 进程迁移配置 |
| msg.payload[].pid | pid\_t | NA | 入参 | 进程PID |
| msg.payload[].srcNid | int | NA | 入参 | PID源远端NUMA |
| msg.payload[].destNid | int| NA | 入参 | PID目的端远端NUMA |
| msg.payload[].ratio | int| NA | 入参 | 迁移比例 |
| msg.payload[].srcNid | memSize\_t | NA | 入参 | 迁移大小 |
| msg.payload[].migrateMode | int | NA | 入参 | 迁移模式 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 迁移成功但修改进程远端NUMA失败返回-9。
* 迁移失败返回-92。
* srcNid不是PID的源远端NUMA，返回-6。
* 参数错误返回-22。
* 内存申请失败返回-12。

#### 约束和注意事项

* 目的NUMA ID内存余量充足。
* 不支持重复调用。
* 调用该接口前必须调用 `ubturbo_smap_process_migrate_enable` 禁用 PID 迁移功能。

### ubturbo_smap_process_tracking_add

#### 函数定义

通知SMAP添加进程扫描，并设置扫描周期参数。

#### 实现方法

<pre class="screen"><p class="p" id="p181653418315">int ubturbo_smap_process_tracking_add(pid_t *pidArr, uint32_t *scanTime, uint32_t *duration, int len, int scanType)</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| pidArr | pid\_t \* | NA | 入参 | PID数组。 |
| scanTime | uint32_t \* | 50毫秒的倍数，最小值50毫秒，最大值为200毫秒。 | 入参 | 扫描周期数组。 |
| duration | uint32_t \* | 与len长度相符的扫描周期数组，取值[1,300]，单位秒，在scanType=2时使用。 | 入参 | 访存数据统计时长。 |
| len | int | 1-40整数。 | 入参 | PID数组长度。 |
| scanType | int | {0,1,2} | 入参 | * 0：将进程设置为只扫描状态，此时调用[/proc/ {PID}_t/tracking_info](#proc-pid_tracking_info)获取扫描频次信息。* 1：将进程恢复为冷热扫描加迁移状态。* 2：表示进程设置为统计特定时长冷热信息状态。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 进程状态非PROC_MOVE无法切换扫描类型返回-16。
* 内核态内存申请失败返回-9。
* 用户态内存申请失败返回-12。

#### 约束和注意事项

* scanType为0或1时只支持虚机场景
* 当进程未被SMAP纳管时，可以调用该接口，此时scanType不能传1。
* 当进程已经被SMAP纳管时，须先停止冷热迁移，然后才可以调用该接口，scanType可以传0/1/2。
* scanType传1的情况为进程已被smap纳管，需要从只扫描状态恢复到冷热扫描加迁移状态。
* 当进程未被SMAP纳管时，只允许进程使用本地numa。

### ubturbo_smap_process_tracking_remove

#### 函数定义

通知SMAP移除进程扫描。

#### 实现方法

<pre class="screen"><p class="p" id="p12683629436">int ubturbo_smap_process_tracking_remove(pid_t *pidArr, int len, int flags)</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| pidArr | pid\_t \* | NA | 入参 | PID数组。 |
| len | int | 1-100整数 | 入参 | PID数组长度。 |
| flags | int | NA | 入参 | 保留字段。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。
* 内存申请失败返回-12。
* 内核ioctl移除PID失败返回-9。

#### 约束和注意事项

* 只有通过 `ubturbo_smap_process_tracking_add` 接口设置 flag 为 0 的 PID 才能被本接口移除。

<a id="proc-pid_tracking_info"></a>
### /proc/ {PID}\_t/tracking\_info

#### 函数定义

通过文件方式查询虚机的访问频次信息。pid为{PID}的虚机的访存频次文件位置为：/proc/{PID}\_t/tracking\_info

#### 实现方法

<pre class="screen"><p class="p" id="p863415451833">// 第一次调用读取数量</p><p class="p" id="p1663494510312">int fread(int *num, sizeof(int), int n, FILE *file)</p><p class="p" id="p156343451733">// 第二次调用读取物理地址和频次信息</p><p class="p" id="p1863420453318">int fread(struct FreqInfo *info, sizeof(FreqInfo), int n, FILE *file)</p></pre>

**表1 ​**参数说明
| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| - | - | - | - | - |
| FreqInfo | struct FreqInfo {u64 paddr;u16 freq;} | NA | 入参 | 存储频次信息结构体。 |

#### 返回值

* 读取成功返回频次个数以及频次信息。
* 读取失败返回errno。

#### 约束和注意事项

* 只支持虚拟化场景下，成功添加仅扫描模式的虚机到SMAP后调用。

### ubturbo_smap_migrate_out_sync

#### 函数定义

通知SMAP调用内存同步迁出接口。

#### 实现方法

<pre class="screen"><p class="p" id="p7119930419">int ubturbo_smap_migrate_out_sync(struct MigrateOutMsg *msg, int pageType, uint64_t maxWaitTime)</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| msg | struct MigrateOutMsg \* | NA | 入参 | 迁移信息。 |
| msg.count | int | 1-40的整数。 | 入参 | 迁移数量。 |
| msg.payload[].pid | pid\_t | NA | 入参 | 进程pid。 |
| msg.payload[].count | int | 0 到 `REMOTE_NUMA_NUM` | 入参 | 该 PID 的远端目标数量。 |
| msg.payload[].inner[] | struct MigrateOutPayloadInner | 与普通 `ubturbo_smap_migrate_out` 相同：remote 唯一、模式一致；ratio 总和不超过 100；memSize 按当前页型对齐。 | 入参 | 多 remote 聚合迁出目标。 |
| pageType | int | 1 | 入参 | 进程pid类型，1表示虚机类型。 |
| maxWaitTime | uint64\_t | 10s-1min(单位ms) | 入参 | 一次调用最大等待时间。 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误（包含非法入参、非内存池化场景）返回-22。
* 等待超时返回-16。
* 内存申请失败返回-12。
* pid无效返回-3。
* 部分或全部pid无效返回-3。

#### 约束和注意事项

* 只支持在虚拟化场景调用。
* 只支持内存池化场景。
* 与普通 migrate-out 一样支持一个 PID 配置多个 remote；`srcNid` 忽略，local NUMA 由 SMAP 自动管理。
* 同步初始迁出绕过普通 remote 容量合同，不消耗 `ubturbo_smap_remote_numa_info_set` 配置的 private/shared 容量；后续周期策略仍按保存的目标继续运行。
* 本接口同步完成初始迁出后，PID仍会被SMAP纳管并参与后续周期冷热迁移；冷热迁移依赖迁移目标NUMA存在可用空闲内存。对于2M huge page虚机场景，本地NUMA和远端NUMA均需要存在可用的空闲2M huge page；若本地NUMA空闲2M huge page不足，远端热页回迁或冷热交换可能无法执行；若远端NUMA空闲2M huge page不足，后续冷页迁出可能无法执行。
* 建议在PID相关的本地NUMA和远端NUMA上均预留不低于计划迁移规模5%~10%的空闲内存；对于2M huge page虚机，该预留量应换算为对应数量的空闲2M huge page。上述预留值为部署建议，不是接口入参校验条件；调用成功仅表示策略配置成功，不保证后续每轮冷热迁移都能实际迁移页面。

### ubturbo_smap_process_config_query

#### 函数定义

查询SMAP进程配置的接口。

#### 实现方法

<pre class="screen" id="ZH-CN_TOPIC_0000002255419832__screen29172049124214"><p class="p" id="p12997132118410">int ubturbo_smap_process_config_query(int nid, struct ProcessPayload *result, int inLen, int *outLen)</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| nid | int | 有效的远端NUMA | 入参 | 远端NUMA NID |
| result | struct ProcessPayload | 非空数组 | 出参 | 保存结果的数组 |
| result[].pid | pid\_t | NA | 出参 | 进程pid |
| result[].ratio | uint8\_t | NA | 出参 | 进程内存本地比例 |
| result[].scanType | uint8\_t | NA | 出参 | 进程扫描类型 |
| result[].type | uint8\_t | NA | 出参 | 进程类型，0-PROCESS\_TYPE，1-VM\_TYPE |
| result[].state | uint8\_t | NA | 出参 | 进程状态，0-空闲，1-冷热迁移，2-迁回，3-远端搬迁 |
| result[].l1Node[4] | int16\_t | NA | 出参 | 进程L1 Node |
| result[].l2Node[4] | int16\_t | NA | 出参 | 进程L2 Node |
| result[].scanTime | uint32\_t | NA | 出参 | 进程扫描间隔 |
| inLen | int | 与数组长度一致， 虚机场景小于等于100，普通进程场景小于等于300 | 入参 | 数组长度 |
| outLen | int \* | 非空整型指针 | 出参 | 进程数量 |

#### 返回值

* 成功返回0。
* SMAP未初始化返回-1。
* 参数错误返回-22。

#### 约束和注意事项

* 切换场景时需要删除SMAP配置文件/dev/shm/smap\_config。

### ubturbo_smap_urgent_migrate_out

#### 函数定义

SMAP紧急迁移接口。

#### 实现方法

<pre class="screen"><p class="p" id="p921733917413">void ubturbo_smap_urgent_migrate_out(uint64_t size)</p></pre>

#### 参数说明

| 参数名 | 数据类型 | 有效性规格 | 参数类型 | 描述 |
| --- | --- | --- | --- | --- |
| size | uint64\_t | NA | 入参 | 内存迁移量，单位为字节 |

#### 返回值

* 无返回值。

#### 约束和注意事项

* 在OOM场景下由上层组件调用。
* 紧急迁出按 numa\_maps 段级过滤收集候选页，无法识别共享页归属，段内共享页可能被一并迁到远端；OOM 场景首要目标是压低本地水线、避免 kill，允许共享页短暂误迁。水线下降后，调用方应把相关 pid 重新加入 SMAP 管理，SMAP 管理态扫描会按 pidType/pageType 纠正共享页归属。
