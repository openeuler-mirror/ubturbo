# UBTurbo 配置说明

## 配置原则说明

1. UBTurbo 的配置能力通过文件的方式暴露，配置文件安装目录为 `/opt/ubturbo/conf/`。
2. UBTurbo 配置文件采用 ini 风格的键值对格式：

    - 由键（key）和值（value）构成，每行一条
    - 每个键值对用等号（**=**）进行连接；左边为键，作为配置项的名称；右边为值，作为配置项内容
    - 以 `#` 开头的行视为注释；空行将被忽略

3. 配置按职责分为多个文件，各文件独立维护：

    | 配置文件 | 作用 | 读取时 section 名 |
    | -------- | ---- | ----------------- |
    | `ubturbo.conf` | 主进程配置（日志等级等） | `ubturbo` |
    | `ubturbo_plugin_admission.conf` | 插件准入白名单（决定加载哪些插件） | — |
    | `plugin_rmrs.conf` | RMRS 插件配置 | `plugin_rmrs` |
    | `plugin_turbo_ucache.conf` | UCache 插件配置 | `plugin_turbo_ucache` |
    | `smap/period.config` | SMAP 策略配置（扫描/迁移周期、冷热阈值等） | —（SMAP 自有解析器，扁平键值，无 section） |

    >[!NOTE]说明
    > 上述前 4 个文件由 UBTurbo 框架的 `TurboConfManager` 统一加载，采用「section + key」定位；`smap/period.config` 由 SMAP 自身的策略解析器（`StrategyConfigRead`）按行匹配键名读取，不走 section 机制，详见下文「SMAP 策略配置说明」。

4. 配置项读取采用「section + key」定位：`TurboConfManager::Init` 在启动时加载主配置、准入白名单及各 `plugin_*.conf`；运行代码通过 `UBTurboGetStr / UBTurboGetUInt32 / UBTurboGetBool / UBTurboGetFloat / UBTurboGetUInt64` 等 API 按 section 与 key 读取。
5. UBTurbo 默认不启用任何插件。如需启用插件，必须在 `ubturbo_plugin_admission.conf` 中取消对应插件项的注释（`#`），并保证对应插件已安装且其 `plugin_*.conf` 配置完成，否则 UBTurbo 启动异常。
6. 修改任一配置文件后，需重启 ubturbo 服务生效：`systemctl restart ubturbo`。

UBTurbo 各配置文件样例如下：

```config
# ===== ubturbo.conf =====
log.level=INFO

# ===== ubturbo_plugin_admission.conf =====
# code 必须大于200
#rmrs=777
#turbo_ucache=778

# ===== plugin_rmrs.conf =====
turbo.plugin.name=rmrs
turbo.plugin.pkg=librmrs_ubturbo_plugin.so
rmrs.ucache.enable=false

# ===== plugin_turbo_ucache.conf =====
turbo.plugin.name=ucache
turbo.plugin.pkg=libucache_os_turbo_plugin.so
migrate.migrateInterval=1000

# ===== smap/period.config =====
smap.scan.period = 200
smap.migrate.period = 2000
smap.slow.threshold = 2
smap.remote.freq.percentile = 99
smap.freq.wt = 0
smap.remote.hot.threshold = 65535
smap.group.swap.ratio = 1
smap.group.swap.min.remote.freq = 0
smap.group.swap.min.freq.gain = 0
smap.group.swap.local.watermark.ratio = 95
smap.migrate.mode = 1
smap.migrate.mode.enable = false
smap.zero.freq.migrate.enable = true
smap.adaptive.ratio.enable = true
smap.period.file.config.switch = false
smap.scan.cpu = 0-127
smap.ub.bw.threshold = 0
```

## 主进程配置说明

配置文件：`ubturbo.conf`
section 取值：`ubturbo`

配置项说明：

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | log.level | 日志等级，决定主进程及已加载插件的日志输出等级。 | 默认值：INFO<br>取值范围：<br>- DEBUG<br>- INFO<br>- WARN<br>- ERROR<br>- CRIT<br>参数配置取值范围之外或错误值都会被重置为默认值 INFO。 |

>[!NOTE]说明
> 日志文件的路径、单文件大小上限与文件数量为内置默认值，不在 `ubturbo.conf` 中配置：
> - 日志目录：`/var/log/ubturbo/`，UBTurbo 框架与各插件日志相互独立、分文件输出。
> - 单个日志文件最大 200MB，最多保留 10 个文件，超过后按生成时间覆盖最早的日志文件。

## 插件准入配置说明

配置文件：`ubturbo_plugin_admission.conf`
作用：声明 UBTurbo 启动时通过 `dlopen` 加载的插件清单。仅在此文件中登记（且未注释）的插件才会被加载并调用其 `TurboPluginInit(moduleCode)` 入口。

格式为 `插件名=moduleCode`，每行一条，`#` 开头为注释（即不启用该插件）。

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | rmrs | 启用 RMRS 内存迁移调度插件，对应 `librmrs_ubturbo_plugin.so`。 | 固定值：777<br>⚠ moduleCode 必须 > 200。默认以 `#` 注释（不启用）。 |
| 2 | turbo_ucache | 启用 UCache 容器内存迁移插件，对应 `libucache_os_turbo_plugin.so`。 | 固定值：778<br>⚠ moduleCode 必须 > 200。默认以 `#` 注释（不启用）。 |

>[!CAUTION]注意
> - `moduleCode` 取值必须大于 200，低于 200 的值保留给框架内部模块，配置后将导致加载异常。
> - 启用插件前，必须确保对应插件已安装且其 `plugin_*.conf` 已就绪；否则 UBTurbo 启动时会因插件模块启动失败而退出（日志：`Start module failed`）。

## RMRS 插件配置说明

配置文件：`plugin_rmrs.conf`
section 取值：`plugin_rmrs`

配置项说明：

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | turbo.plugin.name | RMRS 模块插件名，由插件加载框架用于匹配准入白名单中的条目。 | 固定值：rmrs |
| 2 | turbo.plugin.pkg | RMRS 模块依赖的 so 文件名称，插件框架据此在 `/opt/ubturbo/lib/` 下 `dlopen` 加载。 | 固定值：librmrs_ubturbo_plugin.so |
| 3 | rmrs.ucache.enable | 是否启用 UCache 迁移模式。为 `true` 时，RMRS 经 `/dev/ucache` ioctl 执行容器/虚机文件页迁移，并启用 IO 瓶颈检测（L0-L4 分级）；为 `false` 时采用默认迁移路径。 | 默认值：false<br>取值范围：[true, false]<br>参数配置取值范围之外或错误值都会被重置为默认值 false。 |

>[!NOTE]说明
> - `rmrs.ucache.enable` 启用 UCache 模式依赖 `ucache.ko` 内核驱动（随 `ubturbo-smap` RPM 一并安装）已加载并生成 `/dev/ucache`。
> - RMRS 的运行场景（容器 4K / 虚机 2M）不在此文件配置，由 `ubturbo_smap_start(pageType)` 入参持久化到 `/dev/shm/ubturbo_page_type.dat` 决定，RMRS 启动时读取该文件惰性判定场景。

## UCache 插件配置说明

配置文件：`plugin_turbo_ucache.conf`
section 取值：`plugin_turbo_ucache`

配置项说明：

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | turbo.plugin.name | UCache 模块插件名。 | 固定值：ucache |
| 2 | turbo.plugin.pkg | UCache 模块依赖的 so 文件名称。 | 固定值：libucache_os_turbo_plugin.so |
| 3 | migrate.migrateInterval | 迁移执行周期。UCache 迁移线程按该周期检查本地 NUMA 内存水位，超过高水线按策略将容器内存页迁至远端 NUMA，低于低水线则迁回。 | 默认值：1000<br>单位：毫秒<br>取值范围：> 0 的正整数<br>参数配置为 0 或非法值将导致插件初始化失败。 |

>[!NOTE]说明
> - UCache 插件面向容器（docker）场景，与 RMRS 的 UCache 子模块共用同一 ioctl（`_IOWR(100, 1, MigrateInfo)`）；区别在于本插件面向容器、采用水位驱动迁移，RMRS 的 UCache 子模块面向 VM 并带 IO 瓶颈检测。
> - 启用本插件需在 `ubturbo_plugin_admission.conf` 中 `turbo_ucache=778`（取消注释），并确保 `ucache.ko` 已加载。

## SMAP 策略配置说明

配置文件：`smap/period.config`（路径 `/opt/ubturbo/conf/smap/period.config`）
作用：SMAP 分层内存引擎的扫描与迁移策略参数。

>[!NOTE]说明
> - 该文件不走 UBTurbo 框架的「section + key」机制，而由 SMAP 自身的策略解析器（`StrategyConfigRead`）逐行匹配键名读取，为扁平 `key = value` 格式，无 section。
> - SMAP 首次启动时若文件不存在会自动生成一份带默认值的配置；若已存在则不覆盖，用户可在其上按需修改。
> - 生效方式：SMAP 在每个迁移周期重新读取该文件，但仅当 `smap.period.file.config.switch = true` 时，文件中的配置才会逐周期覆盖内存中已生效的配置；该开关为 `false` 时仅读取不覆盖，SMAP 始终使用启动时加载的配置。
> - 该文件依赖 `ubturbo-rmrs` 组件；环境未安装时需手动创建 `/opt/ubturbo/conf` 目录并设置权限。
> - 配置项分「必配项」与「可选项」：必配项缺失时本次读取失败、配置不生效；可选项缺失时使用默认值。

### 扫描与迁移周期

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.scan.period | 内存冷热扫描周期，每隔该周期统计一次近端/远端内存页访存信息。 | 默认值：200<br>单位：毫秒<br>取值范围：[50, 60000]<br>非法值重置为默认值。<br>⚠ 必须满足 `smap.scan.period <= smap.migrate.period`，否则配置校验失败。 |
| 2 | smap.migrate.period | 内存迁移周期，每隔该周期执行一次冷热页面迁移决策与迁移动作。 | 默认值：2000<br>单位：毫秒<br>取值范围：[500, 300000]<br>非法值重置为默认值。 |
| 3 | smap.slow.threshold | 慢速阈值，用于判定访存是否处于慢速访问状态，参与冷热评分与迁移决策。 | 默认值：2<br>取值范围：[0, 6000]<br>非法值重置为默认值。 |

### 冷热识别与频率

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.remote.freq.percentile | 远端访存频率百分位，用于从远端内存筛选热页的频率门槛百分位。值越大，判热标准越严格。 | 默认值：99<br>取值范围：[1, 100]<br>非法值重置为默认值。 |
| 2 | smap.freq.wt | 频率权重，冷热评分时对访存频率项加权，影响冷热页面排序。 | 默认值：0<br>取值范围：[0, 65535]<br>非法值重置为默认值。 |
| 3 | smap.remote.hot.threshold | 远端热页阈值，访存次数达到该阈值的远端页面判为热页，纳入回迁候选。 | 默认值：65535<br>取值范围：[1, 65535]<br>非法值重置为默认值。 |

### 分组交换策略

用于多 local NUMA 共用同一远端 NUMA 时，按分组批次交错执行冷热页面交换，避免单次迁移冲击过大。

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.group.swap.ratio | 分组交换比例，控制每个分组批次参与冷热交换的页面规模系数。 | 默认值：1<br>取值范围：[0, 10]<br>非法值重置为默认值。 |
| 2 | smap.group.swap.min.remote.freq | 分组交换触发时，远端页面需达到的最小访存频率门槛。 | 默认值：0<br>取值范围：[0, 65535]<br>非法值重置为默认值。 |
| 3 | smap.group.swap.min.freq.gain | 分组交换触发时要求的最小频率增益，确保交换收益抵消迁移开销。 | 默认值：0<br>取值范围：[0, 65535]<br>非法值重置为默认值。 |
| 4 | smap.group.swap.local.watermark.ratio | 分组交换本地水位比例，近端内存使用率低于该水位时触发回迁/交换。 | 默认值：95<br>单位：%<br>取值范围：[0, 100]<br>非法值重置为默认值。 |

### 迁移方式

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.migrate.mode | 页面迁移使用的数据通路方式。 | 默认值：1<br>取值范围：<br>- 0：LD/ST（load/store 指令拷贝迁移）<br>- 1：URMA（远端内存访问迁移）<br>- 2：保留<br>非法值重置为默认值。 |
| 2 | smap.migrate.mode.enable | 迁移方式切换开关。为 `true` 时 `smap.migrate.mode` 的新取值才被采纳并触发切换；为 `false` 时修改 `smap.migrate.mode` 不生效。 | 默认值：false<br>取值范围：[true, false]<br>非法值重置为默认值。 |

### 功能使能开关

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.zero.freq.migrate.enable | 零频迁移使能。为 `true` 时允许将访存次数为 0 的极冷页迁出至远端；为 `false` 时跳过零频页。 | 默认值：true<br>取值范围：[true, false]<br>非法值重置为默认值。 |
| 2 | smap.adaptive.ratio.enable | 自适应比例使能。为 `true` 时按各进程/虚机资源使用情况动态调整近端-远端内存配比；为 `false` 时采用固定配比。 | 默认值：true<br>取值范围：[true, false]<br>非法值重置为默认值。 |
| 3 | smap.period.file.config.switch | 文件配置生效开关。为 `true` 时每个迁移周期从该文件读取的配置将覆盖内存中已生效的配置；为 `false` 时仅读取不覆盖。 | 默认值：false<br>取值范围：[true, false]<br>非法值重置为默认值。 |

### 扫描 CPU 与 UB 带宽

| 序号 | 参数 | 说明 | 取值 |
| :--- | :--- | :--- | :--- |
| 1 | smap.scan.cpu | 执行内存扫描任务的 CPU 范围，扫描线程仅在该范围内被调度。 | 默认值：系统有效 CPU 范围（从 `/sys/devices/system/cpu/possible` 读取，形如 `0-127`）<br>格式：**min-max**，要求 min <= max，否则配置校验失败。 |
| 2 | smap.ub.bw.threshold | UB（Unit Bus）带宽阈值，用于带宽敏感场景下抑制或延迟迁移决策，避免迁移流量抢占业务带宽。为可选项。 | 默认值：0<br>取值范围：[0, 65535]<br>取 0 表示不启用带宽阈值约束。<br>非法值重置为默认值。 |

>[!NOTE]说明
> - 在线修改 `smap.scan.period`、`smap.migrate.period`、`smap.scan.cpu`、`smap.migrate.mode` 等项后，需将 `smap.period.file.config.switch` 置为 `true` 才能让修改在后续周期生效；改回 `false` 可冻结当前配置。
> - 切换 `smap.migrate.mode` 时需同时将 `smap.migrate.mode.enable` 置为 `true`，迁移方式才会真正切换；切换完成后建议改回 `false` 以避免误触发。
> - 必须保证 `smap.scan.period <= smap.migrate.period`，否则配置校验失败，本次读取的配置不会生效。

## 配置示例

### 启用 RMRS 插件（虚拟化场景）

1. 编辑 `/opt/ubturbo/conf/ubturbo_plugin_admission.conf`，取消 RMRS 注释：

   ```shell
   # code 必须大于200
   rmrs=777
   #turbo_ucache=778
   ```

2. 确认 `/opt/ubturbo/conf/plugin_rmrs.conf`：

   ```shell
   turbo.plugin.name=rmrs
   turbo.plugin.pkg=librmrs_ubturbo_plugin.so
   rmrs.ucache.enable=false
   ```

3. 调整日志等级（可选），编辑 `/opt/ubturbo/conf/ubturbo.conf`：

   ```shell
   log.level=INFO
   ```

4. 重启服务使配置生效：

   ```shell
   systemctl restart ubturbo
   ```

### 同时启用 RMRS 与 UCache 插件

1. 编辑 `/opt/ubturbo/conf/ubturbo_plugin_admission.conf`：

   ```shell
   # code 必须大于200
   rmrs=777
   turbo_ucache=778
   ```

2. 确认 `/opt/ubturbo/conf/plugin_turbo_ucache.conf`：

   ```shell
   turbo.plugin.name=ucache
   turbo.plugin.pkg=libucache_os_turbo_plugin.so
   # 迁移周期，单位毫秒
   migrate.migrateInterval=1000
   ```

3. 重启服务：

   ```shell
   systemctl restart ubturbo
   ```

>[!NOTE]说明
> - UBTurbo 进程切换运行用户或切换场景（如虚拟化场景切换到大数据/容器场景）时，需先删除 `/dev/shm/smap_config` 与 `/dev/shm/ubturbo_page_type.dat`，避免读到旧用户的进程迁移配置或旧的 pageType。
> - 若环境未安装 `ubturbo-rmrs` 组件，需手动创建 `/opt/ubturbo/conf` 目录，并确保目录权限与 `ub_turbo_exec` 二进制文件权限一致。
