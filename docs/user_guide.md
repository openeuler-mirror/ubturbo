# UBTurbo 用户指南

## 使用前检查

1. 确认运行平台为 aarch64 openEuler，SMAP 用户态库与内核模块版本匹配。
2. 确认 `ubturbo` 系统用户和用户组已由安装包创建。
3. 确认所需插件共享库和配置已安装，再启用插件。
4. 确认 PID、源 NUMA、目的 NUMA 和容量信息来自可信资源管理中心。

首次安装步骤见[安装指南](ubturbo_installation.md)。

## 配置

运行配置通常位于 `/opt/ubturbo/conf/`：

- `ubturbo.conf`：主进程配置。
- `ubturbo_plugin_admission.conf`：插件准入配置。
- `plugin_rmrs.conf`：RMRS 插件配置。
- `plugin_turbo_ucache.conf`：UCache 插件配置。
- `smap/smap_strategy.conf`：SMAP 策略参数，由 ubturbo-smap 包安装；SMAP 只读取该文件，运行期不回写。

默认准入文件中的插件项被注释，因此 UBTurbo 默认不加载 RMRS 或 UCache。修改前备份原配置，并
使用[配置参考](ubturbo_configuration.md)确认插件名、模块码和共享库均匹配。

## 服务管理

```bash
sudo systemctl start ubturbo
sudo systemctl stop ubturbo
sudo systemctl restart ubturbo
systemctl status ubturbo --no-pager
```

修改主配置、插件准入或插件配置后，应重启服务。当前文档不承诺配置热加载。

### 通过 IPC 使用 SMAP 能力

服务启动后先确认默认 socket 已就绪，再从仓库根目录运行 Python 客户端：

```bash
test -S /opt/ubturbo/ubturbo_ipc
python3 tools/ubturbo_ipc/ubturbo_ipc.py start 0
python3 tools/ubturbo_ipc/ubturbo_ipc.py is_running
```

`start 0` 启动 4K（容器）模式，`start 1` 启动 2M（虚机）模式。其他迁移与查询命令见
[IPC 客户端说明](../tools/ubturbo_ipc/README.md)。调用前必须确认 PID 和 NUMA 参数来自可信资源管理中心。

## 检查运行状态

### 进程与模块

```bash
pgrep -a ub_turbo_exec
lsmod | grep '^smap'
```

### 安装文件

```bash
find /opt/ubturbo -maxdepth 2 -type f -ls
```

检查命令只用于确认安装布局，不应通过放宽整个目录权限来处理单个文件的访问错误。

### 日志

服务日志位置由打包脚本和日志配置决定。优先使用 systemd 日志和实际配置定位：

```bash
journalctl -u ubturbo --since today
```

日志中不得输出凭证、完整内存内容、敏感地址或个人信息。

## 常见故障

| 现象                    | 优先检查                               | 处理原则                       |
| ----------------------- | -------------------------------------- | ------------------------------ |
| 配置模块启动失败        | 配置路径、格式、权限                   | 恢复有效配置，不跳过校验       |
| SMAP 启动失败           | `libsmap.so`、内核模块、设备权限     | 安装匹配版本并检查模块日志     |
| 插件加载失败            | 准入名称、模块码、共享库路径           | 禁用错误项或安装正确插件       |
| IPC 调用失败            | 守护进程、UDS 文件、服务名、超时       | 检查服务注册和本机权限         |
| 切换用户/场景后状态异常 | `/dev/shm/smap_config`、页面类型状态 | 停止所有使用者后按发布说明清理 |

不要在进程运行时直接删除共享状态文件。清理 `/dev/shm/smap_config` 或
`/dev/shm/ubturbo_page_type.dat` 前必须停止所有 SMAP 使用者，并确认没有其他实例共享这些文件。

## 插件使用

- [RMRS](rmrs/README.md)
- [SMAP](smap/README.md)
- [UBDMA](ubdma/README.md)
- [UCache 说明](ucache/README.md)

## 安全注意事项

UBTurbo 只提供节点内执行能力，不校验 PID 是否属于发起方。部署系统必须完成认证、授权、审计和
参数完整性保护。详细要求见[安全说明](ubturbo_security_description.md)。
