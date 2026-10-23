# UCache

UCache 是运行在 UBTurbo 守护进程中的页面缓存迁移插件。插件初始化时加载配置并启动任务执行器，
根据上层请求收集资源信息、计算迁移策略，并通过配套驱动执行页面缓存迁移。

> [!IMPORTANT]
> UCache 涉及内核设备和页面迁移操作，只能由可信资源管理系统调用。调用方必须校验目标资源归属。

## 🧩 核心能力

- 收集 UCache 资源状态。
- 计算页面缓存迁移策略。
- 调度和执行迁移任务。
- 通过 UBTurbo 配置、日志和插件生命周期统一管理。

## 🚀 构建与启用

UCache 随顶层工程构建。内核驱动 `ucache.ko`（源码位于 `plugins/smap/src/ucache`）按模块家族以
`ubturbo-ucache-dkms`/`ubturbo-ucache-kmod` 成对交付（二选一安装：dkms 方式部署至
`/lib/modules/<内核版本>/updates/`，预编译方式安装至 `extra/`），使用前加载：

```bash
sudo modprobe ucache
```

启用前应确保插件动态库、配置和配套驱动均已正确安装，然后在
`ubturbo_plugin_admission.conf` 中取消以下配置的注释：

```ini
turbo_ucache=778
```

重启 UBTurbo 后，通过日志确认插件配置和任务执行器初始化成功。

## ⚠️ 约束

- 不得向插件下发未经授权的进程、地址或 NUMA 资源信息。
- 配套设备、用户态库、内核模块和运行内核必须保持版本兼容。
- 行为和配置变更应通过 `plugins/ucache/test` 中的单元测试验证。

## 🔗 相关文档

- [UBTurbo 配置参考](../ubturbo_configuration.md)
- [UBTurbo 用户指南](../user_guide.md)
- [UBTurbo 安全说明](../ubturbo_security_description.md)

## 📜 许可证

许可条款见仓库根 [LICENSE](../../LICENSE) 及相关源码文件头。
