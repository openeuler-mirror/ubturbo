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
| `smap/period.config`            | 不适用                  | SMAP 自有解析器 |

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

SMAP 从 `/opt/ubturbo/conf/smap/period.config` 读取扁平键值。当前解析表包含 18 个键，其中
`smap.ub.bw.threshold` 是可选项，其余为必填项：

该路径由 `ubturbo` 主框架包创建；单独部署 SMAP 且未安装主框架时，需要手动创建
`/opt/ubturbo/conf`，并为实际运行主体设置合适的最小权限。

| 键                                        |        默认值 | 有效范围/格式                          |
| ----------------------------------------- | ------------: | -------------------------------------- |
| `smap.scan.period`                      |           200 | 50–60000，单位 ms                     |
| `smap.migrate.period`                   |          2000 | 500–300000，单位 ms，且不小于扫描周期 |
| `smap.remote.freq.percentile`           |            99 | 1–100                                 |
| `smap.slow.threshold`                   |             2 | 0–6000                                |
| `smap.freq.wt`                          |             0 | 0–65535                               |
| `smap.remote.hot.threshold`             |         65535 | 1–65535                               |
| `smap.group.swap.ratio`                 |             1 | 0–10                                  |
| `smap.group.swap.min.remote.freq`       |             0 | 0–65535                               |
| `smap.group.swap.min.freq.gain`         |             0 | 0–65535                               |
| `smap.group.swap.local.watermark.ratio` |            95 | 0–100                                 |
| `smap.migrate.mode`                     |             1 | 0：LD/ST；1：URMA                      |
| `smap.migrate.mode.enable`              |         false | `true` / `false`                   |
| `smap.zero.freq.migrate.enable`         |          true | `true` / `false`                   |
| `smap.adaptive.ratio.enable`            |          true | `true` / `false`                   |
| `smap.period.file.config.switch`        |         false | `true` / `false`                   |
| `smap.scan.cpu`                         | 系统 CPU 范围 | `min-max`，且 `min <= max`         |
| `smap.ub.bw.threshold`                  |             0 | 0–65535                               |
| `smap.remote.scan.mode`                 |             1 | 0：纯软；1：软硬结合；2：纯硬         |

版本升级后应以同版本源码或随包示例为准，不能将其他版本的 `period.config` 直接覆盖到当前环境。

`smap.period.file.config.switch=true` 才允许周期读取的文件值覆盖内存中的策略配置。生产环境修改周期、
CPU 范围或迁移方式前，应先评估扫描开销、迁移流量和业务抖动。

`smap.remote.scan.mode` 控制远端节点扫描模式，硬件判热仅鲲鹏950机型支持，硬件不可用时自动降级为
纯软扫描。修改后下个迁移周期生效，无需重插内核模块。配置项缺失或配置非法时沿用上一次有效配置
（尚未配置时使用默认值），并在下个迁移周期检测到非法配置后将文件恢复为上一次有效配置。

## 配置变更流程

1. 备份单个待修改文件，而不是整个安装目录。
2. 校验键名、类型、范围、插件库和模块码。
3. 在维护窗口重启 UBTurbo，或按 SMAP 文件开关规则等待周期生效。
4. 检查日志中是否存在解析回退或模块启动失败。
5. 验证 IPC 和迁移行为；异常时恢复备份并重启。

配置文件不得包含密码、令牌、私钥或不必要的 PID、内存地址等敏感运行数据。
