# UBTurbo 安装指南

## 环境要求

|部件|版本|
|:---|:---|
|操作系统|openEuler 24.03 LTS 或更高版本|
|CPU架构|aarch64|
|内存|32GB及以上（服务默认内存上限为30GB）|
|磁盘|SSD，IOPS 500MB/s|
|网络|无特殊要求（节点内部署，IPC使用UDS）|
|用户权限|安装与管理需 <code>root</code> 权限|

## 节点规划

- UBTurbo 为节点内资源管理框架，单节点独立部署即可，不依赖集群组网。

- 如需与 UBS-Engine（UBSE）配合使用，建议先安装 UBTurbo，再将 UBSE 的 ubse 用户加入 ubturbo 用户组（见“安装注意事项”）。

- 若使用 SMAP 分级内存或 RMRS 内存迁移能力，需提前规划对应前置依赖（见“（可选）安装SMAP”）。

## 安装注意事项

  - UBTurbo RPM 包安装时会通过安装脚本自动创建 <code>ubturbo</code> 系统用户和用户组（登录shell为 <code>/sbin/nologin</code>），无需手动创建。
  - UBSE 需要调用 UBTurbo 接口，UBTurbo 接口有权限校验，需要将 ubse 用户加到 ubturbo 用户组中，该用户组由 UBTurbo 服务创建。如果 UBTurbo 服务未在 UBSE 前安装，ubse 用户可能无法加入 ubturbo 用户组，导致 UBSE 服务调用 UBTurbo 接口异常。待 UBTurbo 安装完成后，需手动执行 <code>sudo usermod -aG ubturbo ubse</code> 将 ubse 用户加入 ubturbo 用户组。
  - 使用 SMAP（分级内存）能力时，需先安装 <code>ubturbo-smap</code> 包（该步骤只安装包，不插入驱动）。
  - <code>/dev/shm/smap_config</code> 保存了 NUMA 和进程配置等信息，如果 UBTurbo 进程需要切换用户，则需要先删除该文件。
  - <code>/dev/shm/ubturbo_page_type.dat</code> 保存了 SMAP 的初始化类型信息，如果 UBTurbo 进程需要切换用户或者场景（例如虚拟化场景切换到大数据场景），则需要先删除该文件。
  - UBTurbo 默认不启用任何插件，需依据业务场景在 <code>ubturbo_plugin_admission.conf</code> 中打开插件（取消对应插件的注释）。打开插件前，需保证插件的动态库和配置文件已分别放置在 <code>/opt/ubturbo/lib</code> 和 <code>/opt/ubturbo/conf</code> 下，否则 UBTurbo 及对应插件将启动异常。

## 执行安装

- 在线安装

  > [!NOTE]说明
  >
  > 在线安装过程中，所需依赖会自动进行安装。
  > 需要系统已配置包含 ubturbo 组件的 openEuler 镜像源。

  ```bash
  # 安装主程序包（含 RMRS 插件）
  sudo dnf install -y ubturbo-rmrs
  ```

- 离线安装

  > [!WARNING]说明
  >
  > 离线安装需要提前安装所需依赖。
  > UBTurbo 运行依赖信息记录在 spec 文件（ubturbo.spec）中。
  > 运行依赖所需系统库，通常由包管理器自动安装。

  ```bash
  # 通过rpm包安装运行包
  # 安装主程序包（含 RMRS 插件）
  sudo rpm -ivh ubturbo-rmrs-<version>-<release>.aarch64.rpm
  # 如需覆盖安装，可执行如下命令：
  sudo rpm -ivh ubturbo-rmrs-<version>-<release>.aarch64.rpm --force
  ```

## 安装结果

  UBTurbo 主程序安装结果：

  | 路径                                  | 用途          |
  |-------------------------------------| -------------|
  | /opt/ubturbo/bin/ub_turbo_exec      | UBTurbo 守护进程主程序 |
  | /opt/ubturbo/lib/                   | 插件动态库目录 |
  | /opt/ubturbo/lib/librmrs_ubturbo_plugin.so | RMRS 插件动态库 |
  | /opt/ubturbo/conf/ubturbo.conf      | 主配置文件    |
  | /opt/ubturbo/conf/ubturbo_plugin_admission.conf | 插件准入配置文件 |
  | /opt/ubturbo/conf/plugin_rmrs.conf  | RMRS 插件配置文件 |
  | /etc/systemd/system/ubturbo.service | systemd 服务  |
  | /var/log/ubturbo/                   | 日志目录      |
  | /var/run/ubturbo/                   | 运行时目录    |
  | ubturbo 用户/用户组                  | 系统用户与用户组（安装脚本自动创建） |

- UBTurbo 客户端运行库安装结果：

  | 文件                                 | 其它说明                                          |
  | ------------------------------------ | ------------------------------------------------- |
  | `/usr/lib64/libubturbo_client.so`    | 客户端 SDK 动态库，供外部进程通过 IPC 访问 UBTurbo，属主 `ubturbo:ubturbo`，权限 `550` |

- 辅助脚本安装结果：

  | 文件                                 | 其它说明                                          |
  | ------------------------------------ | ------------------------------------------------- |
  | `/usr/local/bin/cat.sh`              | root 属主辅助脚本，权限 `500` |

  > [!NOTE]说明
  > 安装完成后，安装脚本已自动执行 <code>systemctl enable ubturbo.service</code>，服务已设置为开机自启，无需重复配置。

## （可选）修改配置

1. 编辑主配置文件：

    ```bash
    sudo vi /opt/ubturbo/conf/ubturbo.conf
    ```

2. UBTurbo 主配置说明：

    | 参数 | 说明 | 取值 |
    | ---- | ---- | ---- |
    | `log.level` | 日志等级 | 默认值：`INFO`，取值范围：`DEBUG`、`INFO`、`WARN`、`ERROR`、`CRIT` |

3. 按业务场景启用插件：

    ```bash
    sudo vi /opt/ubturbo/conf/ubturbo_plugin_admission.conf
    ```

    ```ini
    # code 必须大于200
    rmrs=777
    #turbo_ucache=778
    ```

    > [!NOTE]说明
    > 只有在该准入配置中配置的插件才会被加载并初始化，未配置的插件不予加载。
    > 配置中 key 对应插件的插件名称，value 为插件初始化时使用的 moduleCode（唯一值）。
    > RMRS 插件自身的详细配置见 <code>plugin_rmrs.conf</code>，参数说明参见 RMRS 插件用户指南。

4. 启动 ubturbo 服务

    ```bash
    sudo systemctl start ubturbo
    ```

## （可选）安装SMAP

使用 SMAP 分级内存能力时，需确保部署环境中已安装 <code>ubturbo-smap</code> 包，并按 SMAP 自身安装流程插入驱动。UBTurbo 安装步骤中只安装 SMAP 的包，不插入 SMAP 驱动。

```bash
# 查询 SMAP 包是否已安装
rpm -qa | grep ubturbo-smap
```

若返回如下信息，表示安装成功。

```bash
[root@controller ~]# rpm -qa | grep ubturbo-smap
ubturbo-smap-*.aarch64
```

如未安装，需先安装 SMAP。安装后重启 ubturbo 服务：

    ```bash
    sudo systemctl restart ubturbo
    ```

## 验证部署

### 检查安装结果

```bash
rpm -qa | grep ubturbo-rmrs  # 应输出 ubturbo-rmrs-*.aarch64
```

### 检查服务状态

```bash
systemctl is-active ubturbo  # 应输出 "active"
ps -ef | grep ub_turbo_exec  # 观察进程是否存在
```

### 查看日志

- 方式一

  ```bash
  journalctl -u ubturbo -f
  ```

- 方式二

  ```bash
  /var/log/ubturbo/ubturbo.log
  ```

  启动成功的标志：日志中出现 `TurboMain::Run end.`

### 检查插件加载

```bash
cat /var/log/ubturbo/ubturbo.log | grep "loaded successfully"
```
