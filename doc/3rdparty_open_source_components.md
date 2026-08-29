# ubturbo 开源三方件清单

> 本文档梳理本仓库依赖的全部开源三方件、来源、许可证及引用方式（静态/动态）。
> 发布件 `ub_turbo_exec`、`libubturbo_client.so`、`librmrs_ubturbo_plugin.so` 本体不静态链接任何三方件；静态引用的三方件仅存在于 UT 可执行文件 `ubturbo_ut`、`rmrs_ut` 中，不随产品分发。

## 1. 依赖总览

| 三方件 | 版本 | 许可证 | 来源 | 引用方式 | 使用范围 |
|--------|------|--------|------|----------|----------|
| libboundscheck | 系统安装版本 | MulanPSL-2.0 | openEuler 发行版软件包 `libboundscheck`（`yum install libboundscheck`） | 动态链接（`libboundscheck.so`） | 主库与 UT |
| rapidjson | 1.1.0 | MIT (Tencent) | openEuler 发行版软件包 `rapidjson-devel`（`yum install rapidjson-devel`） | 纯头文件引用（`/usr/include/rapidjson/`） | 主库（rmrs 插件） |
| libvirt | 系统安装版本 | LGPL-2.1 | openEuler 发行版软件包 `libvirt-devel`（`yum install libvirt-devel`） | 动态链接（`libvirt.so`） | 主库（rmrs 插件） |
| googletest | 1.10.9004 | BSD-3-Clause | openEuler 社区开源件 [src-openeuler/googletest](https://gitcode.com/src-openeuler/googletest)（humble 分支，`ros-humble-gtest-vendor_1.10.9004.orig.tar.gz`） | **静态链接**（`libgtest.a`、`libgtest_main.a`） | 仅 UT（`test/`） |
| mockcpp | v2.7 | Apache-2.0 | 开源项目 mockcpp，维持原有获取方式（git submodule / 手动放置于 `test/3rdparty/mockcpp`），src-openeuler 中暂无对应件 | **静态链接**（`libmockcpp.a`） | 仅 UT（`test/`） |
| pthread / rt / dl | glibc | LGPL（glibc） | 操作系统自带 | 动态链接 | 主库与 UT |

## 2. 静态引用的开源三方件明细

### 2.1 googletest（静态）

- **引入位置**：`test/3rdparty/CMakeLists.txt` 中 `EXTERNALPROJECT_ADD(gtest ...)`
- **获取方式**：构建 UT 时自动从 openEuler 社区开源仓库 `https://gitcode.com/src-openeuler/googletest`（humble 分支）下载 `ros-humble-gtest-vendor_1.10.9004.orig.tar.gz` 源码包并解压构建；也可将 googletest 源码手动放置于 `test/3rdparty/googletest/` 优先使用本地源码。下载后自动校验 SHA256（`79b5f1bd841f84ac1b289ab362031cb18ef396fb6234ddf003af09bca3e050fa`），确保供应链完整性与可复现性
- **构建开关**：`-DBUILD_SHARED_LIBS=OFF -DINSTALL_GTEST=ON -DGOOGLETEST_VERSION=1.10.0`（vendor 源码包顶层为 ROS 包装，实际构建入口为包内 `CMakeLists.txt.upstream`）
- **链接产物**：`libgtest.a`、`libgtest_main.a` 静态链接进 `ubturbo_ut`、`rmrs_ut`
- **许可证**：BSD-3-Clause（源码包内 `LICENSE`）
- **合规说明**：BSD-3-Clause 允许静态链接闭源使用，需保留版权声明；该静态库仅存在于测试可执行文件，不进入发布件

### 2.2 mockcpp（静态）

- **引入位置**：`test/3rdparty/CMakeLists.txt` 中 `EXTERNALPROJECT_ADD(mockcpp ...)`
- **获取方式**：维持原有静态依赖方式，源码通过 git submodule / 手动放置于 `test/3rdparty/mockcpp/`（src-openeuler 中暂无 mockcpp 对应开源件）。构建时拷贝至 build 目录并应用 `mockcpp_support_arm64.patch`（ARM64 支持补丁）
- **链接产物**：`libmockcpp.a` 静态链接进 `ubturbo_ut`、`rmrs_ut`
- **兼容性限制（64K 内存页）**：mockcpp 通过 `mprotect` 修改代码段页属性实现 API 打桩，其 ARM64 支持补丁（`mockcpp_support_arm64.patch`）中地址对齐宏 `ADDR_ALIGN_UP`/`ADDR_ALIGN_DOWN` 按 4K 页硬编码；在 64K 内存页环境（如鲲鹏 920 默认配置）下，传入 `mprotect` 的地址未按实际页大小对齐导致调用失败，进程向只读代码段写入指令时崩溃（SIGSEGV）。
- **64K 页环境 UT 跳过策略（不修改 mockcpp 框架）**：UT 执行脚本（`test/run_ut.sh`、`plugins/smap/test/run_dt.sh`、`plugins/ubdma/test/run_dt.sh`、`plugins/ucache/test/run_dt.sh`）在执行测试二进制前通过 `getconf PAGESIZE` 检测系统内存页大小，非 4K 页时打印说明信息并跳过 UT 执行（含覆盖率统计），脚本正常退出（返回 0），不影响 `./build.sh -t test` 等整体流程的执行。编译构建本身与内存页大小无关，仍正常执行。
- **跳过逻辑验证**：如需在 4K 页环境验证跳过行为，可设置环境变量 `MOCKCPP_PAGE_SIZE_OVERRIDE`（如 `MOCKCPP_PAGE_SIZE_OVERRIDE=65536 sh test/run_ut.sh`）强制脚本按指定页大小判断；该变量仅用于验证跳过逻辑，请勿在正式环境使用。
- **直接执行限制**：不兼容环境（非 4K 页）不支持直接运行 `ubturbo_ut`、`rmrs_ut`、`smap_dt`、`ubdma_dt`、`ucache_dt` 等测试二进制，请通过上述脚本执行 UT。
- **许可证**：Apache-2.0（源码内 `COPYING`）
- **合规说明**：Apache-2.0 允许静态链接使用，需随源码保留许可证与 NOTICE；该静态库仅存在于测试可执行文件，不进入发布件

## 3. 动态链接三方件

### 3.1 libboundscheck

- 来源：openEuler 发行版开源软件包（`yum install libboundscheck`），提供 `/usr/lib64/libboundscheck.so`（securec 同源边界检查库）及 `/usr/include/securec.h`
- 主库 `src/CMakeLists.txt` 与 UT `test/CMakeLists.txt` 均动态链接
- openEuler 社区另有源码仓 `openeuler/libboundscheck`，发行版本身即为开源发布件

### 3.2 rapidjson

- 来源：openEuler 发行版开源软件包 `rapidjson-devel`（`yum install rapidjson-devel`），提供 `/usr/include/rapidjson/` 头文件
- 纯头文件库，无链接产物
- rmrs 插件 `rmrs_json_util.h` 中 `#include <rapidjson/document.h>` 等引用

### 3.3 libvirt

- 来源：openEuler 发行版开源软件包 `libvirt-devel`（`yum install libvirt-devel`），提供 `/usr/lib64/libvirt.so` 及 `/usr/include/libvirt/`
- rmrs 插件 `rmrs_libvirt_module.h` 中 `#include <libvirt/libvirt.h>` 引用，动态链接

### 3.4 系统库

- `pthread`、`rt`、`dl`：glibc 提供，动态链接，无需管理

## 4. 发布件依赖结论

`ub_turbo_exec`、`libubturbo_client.so`、`librmrs_ubturbo_plugin.so`（RPM 发布件）链接关系：

```
ub_turbo_exec / libubturbo_client.so / librmrs_ubturbo_plugin.so
├── libboundscheck.so   (openEuler 开源件，动态)
├── libvirt.so          (openEuler 开源件，动态)
├── rapidjson           (openEuler 开源件，header-only)
├── libpthread / librt  (glibc，动态)
└── 无静态链接的三方件
```

即：**发布件中不存在静态引用的开源三方件**，静态引用仅发生在 UT 构建中（googletest、mockcpp），不产生对外分发的合规义务传递。

## 5. 变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-08 | libboundscheck 移除 submodule，改用 openEuler 发行版软件包（动态链接） |
| 2026-08 | rapidjson 移除 submodule，改用 openEuler 发行版软件包 rapidjson-devel（header-only） |
| 2026-08 | googletest 移除 submodule，切换为 openEuler 社区开源件 src-openeuler/googletest（humble 分支，1.10.9004），构建时自动拉取 |
| 2026-08 | mockcpp 维持原有静态依赖方式（源码由 git submodule / 手动放置于 `test/3rdparty/mockcpp`），不随仓库内置 |
| 2026-08 | 补充 mockcpp 64K 内存页兼容性限制说明；UT 执行脚本增加内存页大小检测，非 4K 页环境自动跳过 UT 执行，不影响整体流程 |
