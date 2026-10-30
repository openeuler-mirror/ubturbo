# SMAP 发布说明

## 1.1.1

1.1.1 于 2026-10-30 发布，对应 Git 标签 `1.1.1`（commit `4fd3bcd`）。

### 改进与优化

**配置变化**：配置文件由 `period.config` 更名为 `smap_strategy.conf`，以 RPM `%config` 方式打包。键定义在近期版本有删除/更名、默认值有调整，升级时新版配置直接覆盖旧配置，被修改过的旧文件不保留，自动备份为 `.rpmorig`；旧 `period.config` 不再被删除，升级后需手动清理。SMAP 只读取配置文件、不回写；键缺席时沿用默认值，单键非法只告警不阻断，其余键照常生效。删除 `smap.migrate.mode.enable` 键，`smap.migrate.mode` 默认值改为 `0`（LD/ST）。

### 公共接口

当前公开接口以 `src/user/smap_interface.h` 为准，包含生命周期、迁出/迁回、进程跟踪、查询和远端
NUMA 迁移能力。详细清单见 [API 参考](api_reference.md)。

### 兼容性

- aarch64 Linux/openEuler。
- 用户态库、内核模块、运行内核和内核开发包必须配套。
- 4K 普通进程与 2M 静态大页虚机在启动时选择，不能在同一实例混用。
