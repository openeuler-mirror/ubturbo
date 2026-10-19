# UBTurbo 开发者指南

## 开发方式

UBTurbo 支持三种集成方式：

1. 外部进程链接客户端 SDK，通过 IPC 调用已注册服务。
2. 在守护进程内启用已有插件。
3. 开发新的动态插件，并通过 UBTurbo 暴露节点内服务。

公共接口的完整签名见 [API 参考](api_reference.md)。

## 构建开发环境

```bash
git submodule update --init --recursive
./build.sh -D
```

Debug 编译数据库位于 `dist/debug/compile_commands.json`。Release 构建使用 `dist/release/`。项目只在
aarch64 openEuler 上受支持，因为顶层 CMake 会读取 `/etc/openEuler-release`。

## 外部进程调用

外部程序包含 `turbo_ipc_client.h` 和 `turbo_def.h`，链接 `libubturbo_client.so`。调用流程：

1. 使用 `SetIpcTimeLimit` 设置本进程的 IPC 超时时间（如需覆盖默认值）。
2. 将请求数据放入 `TurboByteBuffer`。
3. 使用与服务端注册完全一致的服务名调用 `UBTurboFunctionCaller`。
4. 检查返回码，再读取输出缓冲区。

`TurboByteBuffer` 可以引用外部分配的数据。输入在 IPC 调用返回前必须有效。当前客户端实现使用
`new[]` 分配成功响应，且不会填写 `response.freeFunc`，调用方读取后必须使用 `delete[]` 释放；这一行为
属于当前 C++ SDK 契约，跨版本升级时应重新核对头文件和实现。

```cpp
#include <string>
#include "turbo_ipc_client.h"

std::string payload = "request payload";
TurboByteBuffer request{
    reinterpret_cast<uint8_t *>(payload.data()), payload.size(), {}};
TurboByteBuffer response;

const uint32_t ret = turbo::ipc::client::UBTurboFunctionCaller(
    "ExampleService", request, response);
if (ret != IPC_OK) {
    // 按错误码处理，不能继续解析 response。
} else {
    // 读取 response.data/response.len。
    delete[] response.data;
}
```

## 开发插件

### 生命周期入口

插件共享库必须导出：

```cpp
extern "C" uint32_t TurboPluginInit(uint16_t moduleCode);
extern "C" void TurboPluginDeInit();
```

- `TurboPluginInit` 注册资源与 IPC 服务，成功返回 `0`。
- 初始化失败必须释放本次调用已经取得的资源，并返回非零值。
- `TurboPluginDeInit` 应注销 IPC 服务、停止工作线程并释放资源；实现应允许在部分初始化后安全调用。
- 不要在卸载后仍保留指向插件代码或静态对象的回调。

### 注册 IPC 服务

```cpp
uint32_t HandleRequest(const TurboByteBuffer &input, TurboByteBuffer &output);

uint32_t ret = UBTurboRegIpcService("ExampleService", HandleRequest);
```

服务名在进程内必须唯一。反初始化时使用同一个名称调用：

```cpp
UBTurboUnRegIpcService("ExampleService");
```

handler 必须验证消息长度、字段范围和枚举值。UBTurbo 的 IPC 框架不会替插件判断 PID 或 NUMA 是否
属于调用者。

### 配置

插件配置文件至少包含：

```ini
turbo.plugin.name=example
turbo.plugin.pkg=libexample_ubturbo_plugin.so
```

插件使用 `UBTurboGetUInt32`、`UBTurboGetUInt64`、`UBTurboGetFloat`、`UBTurboGetBool` 或
`UBTurboGetStr` 读取本 section 的配置。读取失败时应采用明确的安全默认值或终止初始化，不能继续使用
未初始化数据。

### 日志

先声明模块，再使用对应级别宏：

```cpp
RACK_DEFINE_THIS_MODULE("example", 779);
UBTURBO_LOG_INFO("example", 779) << "plugin initialized";
```

日志不得包含凭证、私钥、完整请求内容、敏感内存地址或个人信息。高频路径避免逐页打印日志。

## 测试

UBTurbo、RMRS 和 SMAP 单元测试的环境准备、完整执行命令、结果判断及覆盖率问题处理，见
[构建与单元测试指南](unit_testing.md)。修改代码后应运行与变更范围相关的测试；提交前应完成指南中的
全部测试。

## 提交检查

- Release 构建和相关测试通过。
- 修改过的 C/C++ 文件通过项目 clang-format 和 clang-tidy 规则。
- 新行为包含正常、非法输入、边界值和清理路径测试。
- 公共接口、配置或用户可见行为发生变化时同步更新文档。
