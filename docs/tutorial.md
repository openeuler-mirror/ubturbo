# UBTurbo 实践教程

## 目标

本教程展示如何验证框架 IPC，以及如何准备 RMRS/SMAP 迁移调用。内存迁移会改变目标进程的页面
位置，必须在隔离测试环境中使用由可信资源管理器提供的 PID 和 NUMA 信息。

## 示例一：验证自定义 IPC 服务

### 服务端插件

实现 handler，并在插件初始化时注册：

```cpp
namespace {
uint32_t EchoHandler(const TurboByteBuffer &input, TurboByteBuffer &output)
{
    // 同步 handler 返回前 input 始终有效；server 会立即复制 output。
    output.data = input.data;
    output.len = input.len;
    return IPC_OK;
}
}

extern "C" uint32_t TurboPluginInit(uint16_t moduleCode)
{
    return UBTurboRegIpcService("Echo", EchoHandler);
}

extern "C" void TurboPluginDeInit()
{
    (void)UBTurboUnRegIpcService("Echo");
}
```

将插件共享库安装到 UBTurbo 库目录，添加插件配置和准入项后重启服务。不要复用已有插件名或小于等于
200 的模块码。

### 客户端

```cpp
#include <iostream>
#include <string>
#include "turbo_ipc_client.h"

int main()
{
    std::string message = "hello";
    TurboByteBuffer input{
        reinterpret_cast<uint8_t *>(message.data()), message.size(), {}};
    TurboByteBuffer output;

    const uint32_t ret = turbo::ipc::client::UBTurboFunctionCaller(
        "Echo", input, output);
    if (ret != IPC_OK) {
        std::cerr << "IPC failed: " << ret << '\n';
        return 1;
    }
    std::cout.write(reinterpret_cast<const char *>(output.data), output.len);
    std::cout << '\n';
    delete[] output.data;
    return 0;
}
```

编译时包含 UBTurbo 头文件并链接 `ubturbo_client`。安装路径可能随打包方式变化，应通过构建产物或
RPM 文件列表确定，避免把本机路径硬编码进源码。

## 示例二：准备 RMRS 迁移调用

1. 安装并加载 SMAP，确认页面类型与目标进程一致：普通 4K 进程使用 `pageType=0`，2M 静态大页
   虚机使用 `pageType=1`。
2. 安装 RMRS 插件，并在 `ubturbo_plugin_admission.conf` 中启用 `rmrs=777`。
3. 启动 UBTurbo，确认 RMRS 注册的服务没有报错。
4. 从可信资源管理器取得 PID、远端 NUMA 和容量信息。
5. 按 [RMRS API 参考](rmrs/api_reference.md) 构造请求；先执行策略计算，再根据业务事务
   状态执行迁移、迁回或回滚。

> [!CAUTION]
> 不要使用随意选择的系统 PID 验证迁移。UBTurbo 不校验 PID 的业务归属和有效性，调用链路必须在
> 请求到达 UBTurbo 前完成认证、授权和资源归属校验。

## 示例三：直接集成 SMAP

需要在 UBTurbo 进程之外直接使用 SMAP 时：

1. 链接 `libsmap.so`。
2. 调用 `ubturbo_smap_start(pageType, logCallback)`，同一运行实例只使用一种页面类型。
3. 设置远端 NUMA 可用容量。
4. 添加进程跟踪或配置迁出目标。
5. 结束前移除进程配置并调用 `ubturbo_smap_stop()`。

接口顺序、结构体字段和错误处理见 [SMAP API 参考](smap/api_reference.md)。

## 验证结果

- 客户端返回 `IPC_OK` 只表示 IPC handler 成功，不等同于业务迁移已经完成。
- 同步迁移接口需要同时检查返回码、超时和实际页面位置。
- 异步迁移应通过后续查询、日志和业务侧事务状态判断结果。
- 测试完成后撤销插件准入项、停止服务，并按环境策略清理测试配置。
