# 版本说明

## 版本标识

仓库中存在不同用途的版本字段：

- 顶层 CMake `project(UBTurbo VERSION 1.0)` 是构建工程版本。
- 根目录 `ubturbo.spec` 通过 `rpmbuild` 分别生成 `ubturbo` 主框架包和 `ubturbo-rmrs` 插件包。
- SMAP 使用独立版本和发布节奏，见 [SMAP 发布说明](smap/release_notes.md)。

使用者应以实际 RPM 元数据和目标分支的发布标签为准，不应仅从 README 推断安装包版本。

## 当前能力

- UBTurbo 配置、日志、插件和节点内 IPC 框架。
- RMRS 虚机/容器内存迁移策略与执行服务。
- SMAP 4K/2M 页面扫描和迁移、软件 AF 扫描及可选 HIST 硬件扫描。
- UCache 页面缓存迁移能力和 UBDMA 远端内存复制组件。

## 已知限制

- 仅支持 aarch64 openEuler，构建配置依赖 `/etc/openEuler-release`。
- SMAP 内核模块必须与运行内核及平台能力配套。
- 默认不启用插件；单个插件加载或初始化失败时，当前实现记录错误并跳过，主服务仍可能启动。
- UBTurbo 不校验业务 PID 的所有权和有效性。
- `lcov` 版本不兼容可能影响覆盖率报告生成，但不代表测试用例失败。

## 文档配套

| 文档 | 说明 |
| --- | --- |
| [安装指南](ubturbo_installation.md) | 构建、RPM、SMAP 和部署验证 |
| [用户指南](user_guide.md) | 配置、服务管理和排障 |
| [架构设计](architecture.md) | 组件边界、数据流和生命周期 |
| [开发者指南](developer_guide.md) | SDK 与插件开发 |
| [API 参考](ubturbo_api_reference.md) | UBTurbo 公共接口 |

## 维护要求

每次发布应补充发布日期、发布标签、包版本、兼容平台、API/配置变化、修复项和遗留问题。没有问题单、
测试记录或发布标签依据时，不应填写推测性的“已解决问题”或性能结论。
