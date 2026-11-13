# SMAP

[简体中文](README.md) | [English](readme_en.md)

SMAP 是 UBTurbo 集成的多级内存扫描与页面迁移组件。它由 `libsmap.so` 用户态库以及统一的
`smap.ko` 内核模块组成，支持 4K 普通进程和 2M 静态大页虚机场景。

## 核心能力

- 使用 Linux 页表 Access Flag 进行软件访问扫描。
- 在配套硬件上选择性使用 HIST 进行远端页面访问统计。
- 根据本地冷页和远端热页选择迁移对象。
- 配置进程迁出比例/容量、迁回、紧急迁出和远端到远端迁移。
- 支持多个本地、远端 NUMA 目标和组级迁移策略。

远端扫描模式由 `smap_strategy.conf` 的 `smap.remote.scan.mode` 控制：`0` 为纯软扫描，`1` 为软硬结合
（默认），`2` 为纯硬扫描。HIST 硬件判热仅鲲鹏950机型支持，硬件不可用时自动降级为纯软扫描。
模块初始化时创建 `/dev/smap_scan_dev` 并初始化软件扫描；硬件判热子系统由用户态配置按需初始化。
管理层通过扫描设备下发 PID 管理、页表扫描和频次查询命令，扫描子系统执行扫描并返回结果。

## 🏗️ 架构

![SMAP 分层架构](images/smap-architecture.svg)

| 层次             | 职责                                   |
| ---------------- | -------------------------------------- |
| 调用方           | 设置页面类型、PID、NUMA 容量和迁移目标 |
| 用户态管理与策略 | 管理生命周期、配置、冷热数据和迁移决策 |
| 扫描驱动         | 进程跟踪、页表 AF 扫描和可选 HIST 统计 |
| 迁移驱动         | 页面迁出、迁回和远端到远端迁移         |

![SMAP 扫描与迁移周期](images/smap-cycle.svg)

一个迁移周期包含一个或多个扫描周期。配置被接受不代表页面已经立即收敛到目标比例；PID 退出、
页面被固定、容量变化和 NUMA 下线都可能产生部分成功。

## 🚀 构建与使用

用户态库：

```bash
cd plugins/smap
./build.sh -t release
```

内核模块：

```bash
cd plugins/smap
make -C src/kernel KERNEL_VERSION=openeuler -j"$(nproc)"
```

内核模块的安装、加载、扫描模式配置和卸载方法统一参见
[UBTurbo 安装指南](../ubturbo_installation.md#安装-smap)。

`ubturbo_smap_start` 的 `pageType` 为 `0` 时使用 4K 普通页面，为 `1` 时使用 2M 静态大页；同一实例
不得混用。典型流程为启动 SMAP、设置远端容量、添加 PID 或配置迁出目标、查询结果、移除 PID 并停止
SMAP。完整接口见 [API 参考](api_reference.md)。

内核开发包必须与运行内核匹配。

## 📌 常见问题

| 现象         | 检查项                              |
| ------------ | ----------------------------------- |
| 模块格式错误 | 运行内核与`kernel-devel` 是否一致 |
| 设备打开失败 | 模块加载、udev 规则和运行用户权限   |
| 页面类型冲突 | 已有实例和共享状态中的`pageType`  |
| 扫描无数据   | PID、页型、扫描方式和平台能力       |
| 迁移量不足   | 可迁页面、远端容量、NUMA 状态和阈值 |

## 📑 文档

- [API 参考](api_reference.md)
- [算法设计](performance_algorithm.md)
- [发布说明](release_notes.md)

## ⚠️ 约束

- 仅支持 aarch64 Linux/openEuler 目标环境。
- 内核模块、内核开发包、用户态库和运行内核必须配套。
- `pageType` 在 `ubturbo_smap_start` 时确定；同一实例中的后续调用必须匹配。
- PID 和 NUMA 信息必须由可信调用方提供，SMAP 不承担租户授权。

## 📜 许可证

用户态与内核态文件的许可条款可能不同，分发时应保留源码头和
[Third Party Notice](<../../plugins/smap/License/Third Party Open Source Software Notice.md>)。
