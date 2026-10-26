# UBTurbo 配置参考

## 配置模型

UBTurbo 配置文件安装在 `/opt/ubturbo/conf/`。框架配置采用一行一个 `key=value` 的格式，`#` 开头
的行是注释。框架根据文件名派生 section 名，代码通过 `section + key` 读取配置。

| 文件                              | Section                 | 读取者          |
| --------------------------------- | ----------------------- | --------------- |
| `ubturbo.conf`                  | `ubturbo`             | 主进程          |
| `ubturbo_plugin_admission.conf` | 不适用                  | 插件管理器      |
| `plugin_rmrs.conf`              | `plugin_rmrs`         | RMRS            |
| `plugin_turbo_ucache.conf`      | `plugin_turbo_ucache` | UCache          |
| `smap/smap_strategy.conf`       | 不适用                  | SMAP 自有解析器 |

除 SMAP 策略文件的周期读取机制外，不承诺配置热加载。修改后应重启 UBTurbo。

## 主进程

`ubturbo.conf`：

```ini
log.level=INFO
```

`log.level` 支持 `DEBUG`、`INFO`、`WARN`、`ERROR`、`CRIT`。非法值由日志模块回退到默认级别
`INFO`。

## 插件准入

`ubturbo_plugin_admission.conf` 的格式是 `pluginName=moduleCode`：

```ini
# 默认均不启用
# rmrs=777
# turbo_ucache=778
```

- 模块码必须大于 200；较小的值为框架内部模块保留。
- 插件名必须与其配置中 `turbo.plugin.name` 对应。
- 启用前必须确认共享库和配置文件已安装。当前实现会记录并跳过加载失败的单个插件，守护进程仍可能
  启动，因此部署验证必须检查日志和插件服务，而不能只检查主进程状态。

## RMRS

`plugin_rmrs.conf`：

| 键                     | 类型   | 默认/示例                     | 说明                         |
| ---------------------- | ------ | ----------------------------- | ---------------------------- |
| `turbo.plugin.name`  | string | `rmrs`                      | 插件名                       |
| `turbo.plugin.pkg`   | string | `librmrs_ubturbo_plugin.so` | 共享库名称                   |
| `rmrs.ucache.enable` | bool   | `false`                     | 是否启用 RMRS 的 UCache 路径 |

配置读取失败时，`rmrs.ucache.enable` 回退为 `false`。启用它还要求 UCache 内核能力已经安装并可用：
`ucache.ko` 内核驱动由 `ubturbo-ucache-dkms`/`ubturbo-ucache-kmod` 子包安装（源码位于
`plugins/smap/src/ucache`），需已加载（`modprobe ucache`）并生成 `/dev/ucache`。

## UCache

`plugin_turbo_ucache.conf`：

| 键                          | 类型   | 默认/示例                        | 说明                               |
| --------------------------- | ------ | -------------------------------- | ---------------------------------- |
| `turbo.plugin.name`       | string | `ucache`                       | 插件名                             |
| `turbo.plugin.pkg`        | string | `libucache_os_turbo_plugin.so` | 共享库名称                         |
| `migrate.migrateInterval` | uint32 | `1000` ms                      | 迁移执行线程的等待周期，必须大于 0 |

`migrate.migrateInterval=0` 或读取失败会使 UCache 配置初始化失败。

## SMAP 策略

SMAP 从 `/opt/ubturbo/conf/smap/smap_strategy.conf` 读取扁平键值。该文件由 `ubturbo-smap` RPM 以
`%config` 方式安装（属主 `ubturbo:ubturbo`，权限 `0640`），RPM 升级时新版配置直接覆盖旧配置；
若旧配置曾被用户修改，升级会将其自动备份为 `.rpmorig`，便于比对后合并到新版文件；文件自带注释，
说明每键的取值范围。SMAP 只读取该文件，运行期不会生成或回写；读取路径固定
在 SMAP 策略配置模块内，不能通过调用方参数或环境变量覆盖。单独部署 SMAP 且未安装 RPM 时，需
手动创建目录与文件，并为实际运行主体设置合适的最小权限。`#` 开头的行是注释。

解析表包含 17 个键，全部可选，键缺席时沿用下表内置默认值，不存在必填项：

| 键                                        |          默认值 | 有效范围/格式                          |
| ----------------------------------------- | ---------------: | -------------------------------------- |
| `smap.scan.period`                      |              200 | 50–60000，单位 ms                     |
| `smap.migrate.period`                   |             2000 | 500–300000，单位 ms，且不小于扫描周期 |
| `smap.remote.freq.percentile`           |               99 | 1–100                                 |
| `smap.slow.threshold`                   |                2 | 0–6000（迁移周期上限/扫描周期下限）   |
| `smap.freq.wt`                          |                0 | 0–65535                               |
| `smap.remote.hot.threshold`             |            65535 | 0–65535                               |
| `smap.group.swap.ratio`                 |                1 | 0–10                                  |
| `smap.group.swap.min.remote.freq`       |                0 | 0–65535                               |
| `smap.group.swap.min.freq.gain`         |                0 | 0–65535                               |
| `smap.group.swap.local.watermark.ratio` |               95 | 0–100                                 |
| `smap.migrate.mode`                     |                0 | 0：LD/ST；1：URMA                      |
| `smap.zero.freq.migrate.enable`         |             true | `true` / `false`                   |
| `smap.adaptive.ratio.enable`            |             true | `true` / `false`                   |
| `smap.period.file.config.switch`        |            false | `true` / `false`                   |
| `smap.scan.cpu`                         |     空值=未配置 | `min-max`，如 `0-63`，允许 `35 - 63`   |
| `smap.ub.bw.threshold`                  |                0 | 0–65535                               |
| `smap.remote.scan.mode`                 |                1 | 0：纯软；1：软硬结合；2：纯硬         |

`smap.scan.cpu` 空值（如 `smap.scan.cpu =`）表示未配置：不下发内核、迁移线程不绑核，内核默认
全 CPU 生效。显式配置时绑定扫描 workqueue 与迁移线程到 `[min, max]`，`min <= max` 且两者均不得
超出系统 possible CPU 范围（参见 `/sys/devices/system/cpu/possible`）。

单键非法（越界、非法字符、解析失败）仅告警并废弃该键（该键保持当前/默认值），错误日志带行号，
其余键照常生效；未知键、重复键仅告警不阻断（重复键保留首个值）；value 仅允许字母、数字、`.`、
`_`、`-` 和空格，长度不超过 64。跨字段校验失败（如扫描周期大于迁移周期）时本轮配置整体不应用，
内存保持上一次有效配置；文件缺失或读取失败同样保持当前值。配置文件归用户/RPM 所有；RPM 升级
直接以新版配置覆盖旧版（旧版曾修改的内容备份为 `.rpmorig`），升级后以新文件内容为准。

`smap.period.file.config.switch=true` 才允许周期读取的文件值覆盖内存中的策略配置。生产环境修改周期、
CPU 范围或迁移方式前，应先评估扫描开销、迁移流量和业务抖动。

`smap.remote.scan.mode` 控制远端节点扫描模式，硬件判热仅鲲鹏950机型支持，硬件不可用时自动降级为
纯软扫描。修改后下个迁移周期生效，无需重插内核模块；配置项缺失或本轮校验失败时沿用上一次有效配置
（尚未配置时使用默认值）。

## 配置变更流程

1. 备份单个待修改文件，而不是整个安装目录。
2. 校验键名、类型、范围、插件库和模块码。
3. 在维护窗口重启 UBTurbo，或按 SMAP 文件开关规则等待周期生效。
4. 检查日志中是否存在解析回退或模块启动失败。
5. 验证 IPC 和迁移行为；异常时恢复备份并重启。

配置文件不得包含密码、令牌、私钥或不必要的 PID、内存地址等敏感运行数据。
