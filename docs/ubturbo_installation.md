# UBTurbo 安装指南

## 适用范围

本文说明从源码构建 UBTurbo、生成 RPM、安装运行文件以及验证部署。运行完整的内存迁移能力还必须
安装 SMAP；仅编译 UBTurbo 单元测试不要求加载 SMAP 内核模块。

## 环境要求

| 项目       | 要求                           |
| ---------- | ------------------------------ |
| CPU 架构   | aarch64                        |
| 操作系统   | openEuler；推荐 24.03 LTS 系列 |
| 构建系统   | CMake 3.22 或更高版本          |
| 语言标准   | C++17 / C11                    |
| 内核开发包 | 构建 SMAP 时与运行内核严格匹配 |

安装基础依赖：

```bash
sudo dnf install -y make gcc gcc-c++ cmake ninja-build dos2unix chrpath \
    patchelf libboundscheck libvirt-devel kernel-devel findutils git
```

生成 RPM 还需要：

```bash
sudo dnf install -y rpm-build
```

## 获取源码

```bash
git clone https://gitcode.com/openeuler/ubturbo.git
cd ubturbo
git submodule update --init --recursive
dos2unix build.sh
```

## 源码构建

Release 构建：

```bash
./build.sh
```

默认 Release 产物位于：

- `dist/release/bin/ub_turbo_exec`
- `dist/release/lib/libubturbo_client.so`
- `dist/release/conf/ubturbo.conf`
- `dist/release/conf/ubturbo_plugin_admission.conf`

## 构建 RPM

```bash
./build.sh ubturbo
./build.sh ubturbo-rmrs
```

RPM 打包以仓库根目录的 `ubturbo.spec` 为准，通过 `rpmbuild` 构建，不再使用 CPack。产物位于根目录
`output/`：`ubturbo-*.aarch64.rpm` 是主框架包，`ubturbo-rmrs-*.aarch64.rpm` 是依赖主框架的 RMRS
插件包。不要在自动化脚本中硬编码完整 RPM 文件名，应使用实际构建产物。

安装前检查包信息：

```bash
rpm -qpi <RPM_FILE>
sudo rpm -Uvh output/ubturbo-*.aarch64.rpm output/ubturbo-rmrs-*.aarch64.rpm
```

在线安装时同时指定主框架和 RMRS 插件，包管理器会处理二者依赖：

```bash
sudo dnf install -y ubturbo ubturbo-rmrs
```

## 安装 SMAP

SMAP 是 UBTurbo 的必装组件，包含 `libsmap.so`、udev 规则与统一的内核模块 `smap.ko`（由
`src/kernel` 构建，内含扫描与迁移两部分）。未安装 SMAP 包或未加载 SMAP 驱动时，UBTurbo 服务将
启动失败（日志提示 `Start module failed, name:Smap`）。推荐通过配套 RPM 安装；需要从源码构建时，
参见 [SMAP 文档](smap/README.md)。

内核模块包按 `-dkms` / `-kmod` 成对交付（二选一安装），安装时自动完成模块编译（dkms 方式）、
部署与 depmod，无需手工 insmod / 写入 `/etc/depmod.d/` 配置：

| 子包 | 内容 | 安装路径与行为 |
| ---- | ---- | -------------- |
| `ubturbo-smap` | SMAP 用户态库 `libsmap.so` 与 udev 规则 `99-smap.rules` | `/usr/lib64/`、`/etc/udev/rules.d/`；不含内核模块 |
| `ubturbo-smap-dkms`（推荐） | SMAP 内核模块源码树（`src/kernel`，另含其依赖的上层 `src/common.h`）与 `dkms.conf` | 安装至 `/usr/src/ubturbo-smap-<版本>/`；安装时 dkms 对当前内核自动编译并部署至 `/lib/modules/<内核版本>/updates/`，内核升级后 dkms autoinstall 自动重新编译 |
| `ubturbo-smap-kmod` | 构建机 `kernel-devel` 预编译的 `smap.ko` | 安装至 `/lib/modules/<内核版本>/extra/`，需与运行内核版本一致 |

`-dkms` 包内不含任何 `.ko`，编译失败会导致整体安装失败（由 dnf 回滚）；两种方式路径隔离
（depmod 优先 `updates/`），可共存，同一家族建议二选一。

```bash
# 查询是否已安装（应包含 ubturbo-smap 及 ubturbo-smap-dkms 或 ubturbo-smap-kmod）
rpm -qa | grep '^ubturbo-smap'

# 在线安装（方式一：DKMS）
sudo dnf install -y ubturbo-smap ubturbo-smap-dkms

# 在线安装（方式二：预编译）
sudo dnf install -y ubturbo-smap ubturbo-smap-kmod

# 或安装本地 RPM
sudo rpm -Uvh output/ubturbo-smap-*.aarch64.rpm
```

加载 SMAP 内核模块（包安装后经 depmod 注册，按名称解析，无需进入 ko 所在目录）：

```bash
sudo modprobe smap
```

远端扫描模式由 `/opt/ubturbo/conf/smap/smap_strategy.conf` 中的 `smap.remote.scan.mode` 控制：
`0` 为纯软扫描，`1` 为软硬结合（默认），`2` 为纯硬扫描。硬件判热仅鲲鹏950机型支持，
硬件不可用时自动降级为纯软扫描；运行期可通过配置切换，无需重插模块。
页面类型由 `ubturbo_smap_start(pageType)` 指定，`0` 为 4K，`1` 为 2M，与模块加载解耦。
加载后提供 `/dev/smap_scan_dev` 和 `/dev/smap_migrate_dev` 两个 SMAP 主设备节点，
由 udev 配置为 `ubturbo:ubturbo`、0600 权限。使用 dkms 方式时，可通过 `dkms status`
查看模块源码树的编译/安装状态。

卸载前停止所有 SMAP 使用者；包卸载脚本会自动卸载模块（`modprobe -r smap`），dkms 方式
还会执行 `dkms remove` 清理源码树与已编译模块：

```bash
sudo modprobe -r smap
```

## 关闭冲突的内存管理机制

启动 SMAP 前必须关闭 Linux 的 NUMA Balance：

```bash
sudo sysctl -w kernel.numa_balancing=0
sysctl kernel.numa_balancing
```

NUMA Balance 会根据页面访问情况自动调整页面所在的 NUMA 节点。若保持开启，它可能将 SMAP 已经
迁移到远端 NUMA 的页面再次迁回本地，导致迁移比例无法稳定，并干扰冷热识别和性能测试结果。生产
环境应通过系统配置持久化该设置，确保服务重启后仍为 `kernel.numa_balancing = 0`。

同时必须确保 Linux DAMON（Data Access MONitor）没有运行。DAMON 与 SMAP 都会采集内存访问特征，
同时扫描同一进程或地址空间时会产生额外开销，并可能相互干扰扫描周期及冷热判断。先检查 DAMON
管理接口和 DAMON reclaim 状态：

```bash
grep -H . /sys/kernel/mm/damon/admin/kdamonds/*/state 2>/dev/null || true
cat /sys/module/damon_reclaim/parameters/enabled 2>/dev/null || true
```

若存在运行中的 kdamond，停止所有 DAMON 使用者后将其关闭：

```bash
for state_file in /sys/kernel/mm/damon/admin/kdamonds/*/state; do
    test -e "$state_file" || continue
    echo off | sudo tee "$state_file"
done

if test -e /sys/module/damon_reclaim/parameters/enabled; then
    echo N | sudo tee /sys/module/damon_reclaim/parameters/enabled
fi
```

不同内核版本的 DAMON sysfs 路径可能不同；若系统通过其他服务或工具启动 DAMON，还应停止对应服务，
并确认测试期间没有新的 DAMON 监控上下文被创建。内核未启用 DAMON 时相关路径不存在，检查命令会
直接跳过。

## 配置并启动

默认不启用任何插件。先按[配置参考](ubturbo_configuration.md)检查插件共享库和配置，再修改
`/opt/ubturbo/conf/ubturbo_plugin_admission.conf`。

RPM 安装场景使用 systemd：

启动前必须完成 SMAP 包安装和驱动加载。

```bash
sudo systemctl start ubturbo
systemctl status ubturbo
```

源码构建场景可在保持 `bin/`、`lib/`、`conf/` 相对布局的前提下运行：

```bash
cd dist/release/bin
./ub_turbo_exec
```

## 验证

```bash
rpm -qa | grep '^ubturbo'
lsmod | grep '^smap'
systemctl status ubturbo
```

同时检查日志中是否出现配置解析、SMAP 加载、插件加载或 UDS 监听失败。服务启动成功只表明进程
完成初始化，不代表业务 PID、远端 NUMA 容量或外部资源管理链路已经可用。

## 容器说明

`docker/ubturbo.Dockerfile` 和 `docker/run_verify.sh` 用于构建验证环境。SMAP 涉及宿主机内核模块、设备
节点和特权操作，容器部署不能隔离这些宿主机依赖。生产环境不得直接照搬验证脚本中的宽权限设置。

可以直接拉取预构建的验证镜像，或使用仓库中的 Dockerfile 构建：

```bash
docker pull swr.cn-north-4.myhuaweicloud.com/ubscore/ubturbo:24.03-lts
docker build -f docker/ubturbo.Dockerfile -t ubturbo-build:24.03-lts .
```

GoogleTest 和 mock 测试桩框架来自源码子模块，无需作为系统包预装。覆盖率报告依赖 openEuler 官方
仓库未提供的 lcov 工具链，需要使用时应另行安装。

## 卸载

先停止服务和所有 SMAP 调用者，再卸载软件包和内核模块：

```bash
sudo systemctl stop ubturbo
sudo rpm -e ubturbo-smap ubturbo-rmrs ubturbo
```

是否删除 `/opt/ubturbo/conf`、日志和 `/dev/shm` 状态文件应由数据保留策略决定，不应在卸载脚本外
直接递归删除整个目录。
