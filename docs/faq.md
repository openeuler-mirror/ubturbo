# UBTurbo FAQ

## 为什么不能在 x86 或非 openEuler 环境直接构建？

项目当前仅支持 aarch64。CMake 配置阶段还会读取 `/etc/openEuler-release`，因此请在受支持的
openEuler 环境中构建。环境与依赖要求见[安装指南](installation.md)。

## 为什么启用插件后守护进程启动失败？

确认插件动态库已经安装，并检查 `ubturbo_plugin_admission.conf` 中的准入项、动态库路径和模块编号。
插件初始化失败会导致对应模块不可用，具体排查步骤见[用户指南](user_guide.md#常见故障)。

## 为什么 SMAP 内核模块无法加载？

内核模块必须与运行内核及 `kernel-devel` 匹配，并按依赖顺序加载。还应检查内核配置、NUMA 环境和
设备依赖。完整顺序见[安装指南](installation.md#安装-smap)。

## 为什么 IPC 客户端无法连接？

先确认 UBTurbo 进程运行正常，随后检查 `/opt/ubturbo/ubturbo_ipc` 是否存在且为 Unix socket，并确认
调用用户具备目录和 socket 访问权限。不要通过授予全局读写权限绕过权限问题。

## 如何选择 4K 和 2M 页面模式？

调用 `start` 时，`pageType=0` 选择 4K（典型容器）模式，`pageType=1` 选择 2M（典型虚机）模式。
同一实例后续调用必须与初始化模式一致，详情见 [SMAP API 参考](smap/api_reference.md)。

## 切换用户或页面模式后为什么启动异常？

SMAP 会在共享内存中保存运行配置和页面模式。请先停止相关服务，并依据部署流程处理旧状态文件；不要
在服务运行时直接删除共享状态。操作前参阅[用户指南](user_guide.md)并确认没有正在执行的迁移任务。

## 单元测试通过但覆盖率生成失败怎么办？

覆盖率阶段依赖 `lcov` 和 `genhtml`，工具版本还需与 GCC/gcov 兼容。应分别确认测试二进制的退出状态
和覆盖率工具输出，相关依赖见[开发者指南](developer_guide.md#测试)。

## 如何报告问题？

请提供 UBTurbo 版本、openEuler 与内核版本、硬件和 NUMA 拓扑、启用的插件、配置脱敏副本、复现步骤
以及相关错误日志。不得提交密码、Token、私钥、个人信息或敏感内存地址。
