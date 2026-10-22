# UBTurbo 构建与单元测试指南

本文用于在源码仓库中完成 UBTurbo 编译，并运行 UBTurbo、RMRS 和 SMAP 单元测试。除特别说明外，
所有命令均从仓库根目录执行。

## 环境要求

> **架构与操作系统要求**：UBTurbo 和 SMAP 仅支持 aarch64 架构，推荐使用 openEuler 24.03 LTS。

安装 UBTurbo 编译和单元测试依赖：

```bash
sudo dnf install -y make gcc gcc-c++ cmake ninja-build dos2unix chrpath \
    patchelf libboundscheck libvirt-devel findutils git
```

| 依赖 | 说明 | 来源 |
| ---- | ---- | ---- |
| CMake >= 3.22 | 构建系统 | `CMakeLists.txt` |
| gcc/g++ | C/C++ 编译器 | `ubturbo.spec` |
| make | 构建工具及 SMAP UT 构建工具 | `ubturbo.spec`、`plugins/smap/test/run_dt.sh` |
| ninja-build | UBTurbo 构建加速，可选但推荐 | `build.sh` |
| dos2unix | 转换脚本和补丁的行尾格式 | 构建及测试脚本 |
| chrpath、patchelf | ELF 和 RPATH 处理工具 | `ubturbo.spec` |
| libboundscheck | 安全字符串与内存函数库 | `ubturbo.spec`、测试 CMake 配置 |
| rapidjson | JSON 解析库；系统未安装时由 CMake 下载 | 测试 CMake 配置 |
| libvirt-devel | RMRS 编译依赖 | `ubturbo.spec` |
| findutils | 提供 `find`、`xargs`，测试脚本需要 | 测试脚本 |
| git | 初始化子模块并应用 mockcpp 补丁 | 测试脚本 |
| GoogleTest、mockcpp | 单元测试框架 | Git 子模块 |

测试脚本会继续生成覆盖率报告。尤其是 SMAP 的 `run_dt.sh` 会在缺少 `lcov` 或 `genhtml` 时返回失败；
如需让完整脚本成功结束，应在运行测试前按[代码覆盖率](#代码覆盖率)一节安装覆盖率工具。

单元测试使用 mockcpp 修改代码段进行打桩，只能在系统页大小为 4K 的环境实际运行。执行以下命令检查：

```bash
getconf PAGESIZE
```

输出必须为 `4096`。如果不是 4K，测试脚本会输出 `[SKIP]` 并以状态码 0 退出；这表示测试被跳过，
不能视为单元测试通过。建议在干净的工作区执行测试脚本，因为脚本会准备测试源码、构建目录并应用
mockcpp 补丁。

## 初始化源码

拉取测试所需的 GoogleTest、mockcpp 等子模块，并转换入口脚本的行尾格式：

```bash
git submodule update --init --recursive
dos2unix build.sh
```

确认命令成功后再继续。子模块不完整会导致后续 CMake 配置或 mockcpp 补丁应用失败。

## 编译 UBTurbo

执行 Release 构建：

```bash
./build.sh
```

构建成功后应存在以下关键产物：

- `dist/release/bin/ub_turbo_exec`
- `dist/release/lib/libubturbo_client.so`
- `dist/release/conf/ubturbo.conf`
- `dist/release/conf/ubturbo_plugin_admission.conf`

可以执行以下命令确认：

```bash
test -x dist/release/bin/ub_turbo_exec
test -f dist/release/lib/libubturbo_client.so
test -f dist/release/conf/ubturbo.conf
test -f dist/release/conf/ubturbo_plugin_admission.conf
```

## 运行 UBTurbo 和 RMRS 单元测试

在仓库根目录执行：

```bash
./build.sh -t test
```

该命令调用 `test/run_ut.sh`，依次构建并运行：

| 测试可执行文件 | 测试范围 |
| -------------- | -------- |
| `test/build/ubturbo_ut` | UBTurbo 核心模块，包括 config、log、IPC、plugin、SMAP 编解码等 |
| `test/build/rmrs_ut` | RMRS 插件，包括 migrate、smap_helper、ucache 等 |

也可以直接执行同一个测试脚本：

```bash
sh test/run_ut.sh
```

日志中必须同时出现 `ubturbo_ut` 和 `rmrs_ut` 的 GoogleTest 通过结果。仅出现“编译完成”不代表测试已经
运行。脚本会自动设置 `TZ=Asia/Shanghai`，避免 UTC 时区导致 `TestGenerateCompressedFilename` 失败。

运行单个测试套或测试用例时，先完成上述构建，再执行：

```bash
./test/build/ubturbo_ut --gtest_filter='SuiteName.CaseName'
./test/build/rmrs_ut --gtest_filter='SuiteName.CaseName'
```

将 `SuiteName.CaseName` 替换为实际的测试套和用例名称。使用 `--gtest_list_tests` 可以列出用例：

```bash
./test/build/ubturbo_ut --gtest_list_tests
./test/build/rmrs_ut --gtest_list_tests
```

## 运行 SMAP 单元测试

SMAP 使用独立测试脚本。在仓库根目录执行：

```bash
cd plugins/smap/test
sh run_dt.sh
```

脚本会构建并运行 `plugins/smap/test/build/smap_dt`，覆盖 drivers、tiering、user、ucache 等模块。日志中
必须出现 `smap_dt` 的 GoogleTest 通过结果，并且不能出现 `[SKIP]`，才表示 SMAP 单元测试实际通过。

执行完成后返回仓库根目录：

```bash
cd ../../..
```

## 代码覆盖率

`test/run_ut.sh` 和 `plugins/smap/test/run_dt.sh` 会在测试后调用 `lcov` 和 `genhtml` 生成覆盖率报告。
lcov 不在 openEuler 24.03 官方仓库中，需要单独安装；可从
[Linux Test Project lcov 仓库](https://github.com/linux-test-project/lcov)获取源码，进入源码目录后安装：

```bash
make install PREFIX=/usr/local
```

lcov 的运行还需要 Perl 及相关模块：

```bash
sudo dnf install -y perl perl-Capture-Tiny perl-DateTime
```

已知 `lcov 2.3` 与 `gcc 12.3.1` 可能出现 `mismatched exception tag` 兼容问题。遇到覆盖率错误时，必须
先查看三个 GoogleTest 可执行文件的结果：GoogleTest 全部通过而 lcov 失败，表示单元测试通过、覆盖率
报告生成失败；应改用与当前 GCC/gcov 兼容的 lcov 版本。

覆盖率报告的默认位置为：

- UBTurbo/RMRS：`test/build/gcovr_report/`
- SMAP：`plugins/smap/test/build/gcovr_report/`

## 完成检查

只有同时满足以下条件，才能认为仓库构建和全部单元测试完成：

1. `./build.sh` 成功退出，且 Release 关键产物存在。
2. 系统页大小为 4096，没有测试输出 `[SKIP]`。
3. `ubturbo_ut`、`rmrs_ut` 和 `smap_dt` 均完成执行并输出 GoogleTest 通过结果。
4. 如果覆盖率阶段失败，已经确认失败发生在所有 GoogleTest 通过之后，并单独记录覆盖率工具问题。
