<div align="center">

<h1><img src="docs/images/ubturbo-logo.png" alt="UBTurbo" width="50%" /></h1>

面向 openEuler 的节点内资源管理与多级内存调度框架

[![License](https://img.shields.io/badge/license-MulanPSL--2.0-orange.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-openEuler%20aarch64-blue.svg)](#软件硬件配套说明)
[![Language](https://img.shields.io/badge/language-C%20%7C%20C%2B%2B-lightgrey.svg)](CMakeLists.txt)

[简体中文](README.md) | [English](README_EN.md)

</div>

## 🔄 Latest News

- `[2026/08]` 支持通过监控远端内存带宽自适应调整冷热页面交换。
- `[2026/06]` 支持多本地 NUMA、容器远端迁远端与 4K 硬件扫描。
- `[2026/05]` 支持大规格TB级虚机的管理。
- `[2026/04]` 支持 UBDMA 远端迁移。
- `[2026/02]` 支持虚机多远端 NUMA 迁移。
- `[2026/01]` 适配硬件判热模块。

## 🎉 概述

UBTurbo 是一款开源的节点内资源管理框架，面向 openEuler aarch64 服务器。其设计目的与核心思想是：

- **通用能力统一承载**：由守护进程统一提供配置、日志、生命周期、插件管理和节点内通信能力。
- **资源能力插件化**：RMRS、UCache 等组件以动态库形式按需加载，业务能力与框架独立演进。
- **多级内存协同调度**：组合 RMRS 的迁移决策和 SMAP 的页面扫描、迁移能力，管理本地与远端 NUMA 内存。

![UBTurbo architecture](docs/images/ubturbo-architecture.svg)

如上图所示，UBTurbo 主要由以下模块组成：

- **Client SDK**：以 `libubturbo_client.so` 向外部进程提供调用接口，完成请求序列化和结果解析。
- **IPC**：基于 Unix Domain Socket 接收节点内请求，将服务名分发到对应处理函数。
- **Plugin Manager**：读取插件准入配置，通过 `dlopen`、`dlclose` 管理插件生命周期。
- **Config & Log**：为主进程和插件提供统一配置读取与分级日志能力。
- **RMRS**：面向虚机和容器场景生成并执行内存迁移决策。
- **SMAP**：通过用户态策略与内核模块识别冷热页面并执行页面迁移。
- **UCache & UBDMA**：分别提供进程Page Cache比例控制和 DMA页面迁移相关扩展能力。

典型场景中，集群资源管理中心通过 Client SDK 向 RMRS 下发本地内存迁移请求；RMRS 根据资源状态形成决策出业务pid的内存迁出量，
调用 SMAP 能力页面扫描和迁移。整个过程由 UBTurbo 统一管理配置、日志、服务注册与进程生命周期。

## 🧩 核心特性

- **插件化资源管理**

  UBTurbo 根据 `ubturbo_plugin_admission.conf` 发现和加载动态库。插件通过统一入口完成初始化、IPC 服务
  注册和资源释放，可在不修改主进程职责的情况下独立开发、部署和演进。
- **节点内可靠通信**

  Client SDK 通过 UDS 调用主进程和插件注册的服务。服务端完成请求解码、服务查找、处理函数调用及结果
  返回。该通道不监听网络端口，其访问边界由本机账号、用户组和 socket 文件权限控制。
- **多级内存扫描与迁移**

  SMAP 识别页面冷热，并在本地或远端 NUMA 节点之间迁移页面。`pageType=0` 表示 4K 页面模式，
  `pageType=1` 表示 2M 页面模式。RMRS 可结合容量、冷热信息和业务约束执行迁出、迁回与回滚。
- **统一基础设施**

  框架按依赖顺序启动配置、日志、SMAP、插件和 IPC，并在退出时释放资源。异步日志、配置读取和客户端
  SDK 可被主进程及各插件复用，减少重复实现。

## 🔍 目录结构

```text
├── 3rdparty/              # 第三方源码依赖
├── conf/                  # 主进程与插件配置
├── docs/                  # 安装、使用、开发、API 与组件文档
│   ├── images/            # 项目级架构图
│   ├── rmrs/              # RMRS 文档
│   ├── smap/              # SMAP 文档
│   ├── ubdma/             # UBDMA 文档
│   └── ucache/            # UCache 文档
├── include/               # 公共头文件
├── plugins/               # RMRS、SMAP、UBDMA、UCache
├── src/                   # 框架、IPC、SDK 与公共模块源码
├── test/                  # UBTurbo 与 RMRS 单元测试
├── tools/ubturbo_ipc/     # Python IPC 客户端
├── CMakeLists.txt
├── build.sh
└── README.md
```

## 🚀 快速入门

请访问以下文档获取简易教程。

- [编译安装](docs/installation.md)：介绍依赖准备、源码构建、RPM 安装、SMAP 加载和服务验证。
- [构建与单元测试](docs/unit_testing.md)：按步骤编译仓库并运行 UBTurbo、RMRS 和 SMAP 单元测试。
- [服务使用](docs/user_guide.md)：介绍配置插件、启动服务、检查状态和常见故障处理。
- [样例执行](docs/tutorial.md)：介绍如何调用 Client SDK、RMRS 与 SMAP 能力。

## 📑 学习教程

- [功能与架构](docs/architecture.md)：介绍系统边界、组件职责、生命周期和 IPC 数据流。
- [C/C++ API](docs/api_reference.md)：介绍公共数据类型、客户端、服务端、配置和日志接口。
- [配置参考](docs/configuration.md)：介绍 UBTurbo、RMRS、UCache 和 SMAP 配置项。
- [插件开发](docs/developer_guide.md)：介绍开发环境、插件入口、IPC 服务注册、测试和提交检查。
- [IPC Python 客户端](tools/ubturbo_ipc/README.md)：介绍命令、页面模式和调用安全约束。

## 📦 软件硬件配套说明

- 硬件平台
  - CPU 架构：aarch64
  - 内存拓扑：本地/远端 NUMA；实际支持范围取决于 SMAP 驱动和目标硬件
- 操作系统：openEuler 24.03 LTS 系列
- CMake >= 3.22
- GCC 工具链，支持 C++17/C11
- RMRS：`libvirt-devel`

> **SMAP 交付形态**：使用 SMAP 能力需提前安装 SMAP，包含用户态包 `ubturbo-smap` 与内核模块包
> `ubturbo-smap-dkms`（源码包，dkms 自动编译部署至 `/lib/modules/<内核版本>/updates/`）或
> `ubturbo-smap-kmod`（预编译包，安装至 `extra/`）二选一。内核模块已统一为单个 `smap.ko`，经
> `modprobe smap` 加载；详见 [安装指南](docs/installation.md) 与 [SMAP 文档](docs/smap/README.md)。

## 📌 FAQ

常见问题请参考：[FAQ](docs/faq.md)

## 📝 相关信息

- [安全声明](docs/ubturbo_security_description.md)
- [权限机制迁移说明](docs/ubturbo_capability_migration.md)
- [许可证](LICENSE)
- [第三方开源组件](docs/third_party_components.md)
- [版本说明](docs/release_notes.md)

## 说明

此开源项目非商业产品，仅提供社区支持。
