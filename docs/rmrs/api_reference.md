# RMRS API 参考

本文在新版文档结构中完整保留原 API 参考内容。各接口的参数、返回值、错误、约束、附注和示例均按原文呈现，文末保留新版补充说明。

## 阅读导航

- [内存迁移](#ubturbormrsagentmigratestrategy内存迁出策略)
- [信息采集](#pidnumainfocollectrecvhandler容器进程内存采集)
- [UCache](#ubturbormrsagentucachemigratestrategypagecache迁移策略执行)

## UBTurboRMRSAgentMigrateStrategy:内存迁出策略

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
#include "turbo_rmrs_interface.h"

uint32_t UBTurboRMRSAgentMigrateStrategy(const MigrateStrategyParamRMRS &migrateStrategyParam, MigrateStrategyResult &migrateStrategyResult);
```

### 描述 DESCRIPTION

根据输入需要迁出本地内存大小和每个虚拟机对应最大迁出比例，确定虚拟机迁出的具体比例和对应远端NUMA。

### 参数 Parameters

| name                  | IN/OUT | description                                                  |
| --------------------- | ------ | ------------------------------------------------------------ |
| migrateStrategyParam  | IN     | struct MigrateStrategyParamRMRS { <br/> std::vector&lt;VMPresetParam&gt; vmInfoList;                    // 虚拟机列表及最大迁出比例<br/> std::uint64_t borrowSize;                                 // 需要匀出本地内存大小 <br/> std::map<pid_t, std::vector<uint16_t>> pidRemoteNumaMap;  // pid对应的远端numa信息Map <br/> std::vector<uint16_t> timeOutNumas;                       // 归还超时的远端numa<br/>}; <br/> struct VMPresetParam {<br/>pid_t pid;      // vm对应pid<br/>uint16_t ratio; // 迁出最大比例<br/>};|
| migrateStrategyResult | OUT    | struct VMMigrateOutParam {<br/>    pid_t pid;<br/>    uint64_t memSize;   // 迁出预设大小<br/>    uint16_t desNumaId; // 迁移远端numa<br/>};<br/>struct MigrateStrategyResult {<br/>    std::vector&lt;VMMigrateOutParam&gt; vmInfoList;<br/>    uint64_t waitingTime; // 单位ms<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成内存迁出策略调用。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentMigrateStrategy UBTurboRMRSAgentMigrateStrategy = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentMigrateStrategy = reinterpret_cast<UBTurboRMRSAgentMigrateStrategy>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentMigrateStrategy"));

    MigrateStrategyParam migrateStrategyParam;
    /*
    填充对应参数
    */
    MigrateStrategyResult migrateStrategyResult;
    auto ret = UBTurboRMRSAgentMigrateStrategy(migrateStrategyParam, migrateStrategyResult);
    return 0;
}
```

## UBTurboRMRSAgentMigrateExecute:内存迁出执行

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
uint32_t UBTurboRMRSAgentMigrateExecute(const MigrateStrategyResult &migrateStrategyResult);
```

### 描述 DESCRIPTION

内存迁出执行。

### 参数 Parameters

| name                  | IN/OUT | description                                                  |
| --------------------- | ------ | ------------------------------------------------------------ |
| migrateStrategyResult | IN     | struct VMMigrateOutParam {<br/>    pid_t pid;<br/>    uint64_t memSize;   // 迁出预设比例<br/>    uint16_t desNumaId; // 迁移远端numa<br/>};<br/>struct MigrateStrategyResult {<br/>    std::vector&lt;VMMigrateOutParam&gt; vmInfoList;<br/>    uint64_t waitingTime; // 单位ms, 范围10s-3min<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码
返回值2：socket创建失败
返回值3：与server通信失败
返回值4：未找到对应函数
返回值5：函数执行错误
返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成虚拟机内存迁出功能。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentMigrateExecute UBTurboRMRSAgentMigrateExecute = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentMigrateExecute = reinterpret_cast<UBTurboRMRSAgentMigrateExecute>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentMigrateExecute"));

    MigrateStrategyResult migrateStrategyResult;
    /*
    填充对应参数
    */
    ret = UBTurboRMRSAgentMigrateExecute(migrateStrategyResult);
    return 0;
}
```

## UBTurboRMRSAgentMigrateBack:内存归还

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
#include "turbo_rmrs_interface.h"

uint32_t UBTurboRMRSAgentMigrateBack(MigrateBackResult &migrateBackResult);
```

### 描述 DESCRIPTION

检查并迁回当前节点上可迁回的远端虚拟机，计算可归还的远端NUMA列表。

### 参数 Parameters

| name              | IN/OUT | description                                                  |
| ----------------- | ------ | ------------------------------------------------------------ |
| MigrateBackResult | IN     | class MigrateBackResult {<br/>public:<br/>    uint32_t result{};                   <br/>    std::vector<uint16_t> numaIds{};     // 决策结果，表示哪些远端numaID可以归还<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成内存归还部分功能（将虚拟机迁回本地numa，计算可归还的远端NUMA列表）。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentMigrateBack UBTurboRMRSAgentMigrateBack = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentMigrateBack = reinterpret_cast<UBTurboRMRSAgentMigrateBack>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentMigrateBack"));

    MigrateBackResult migrateBackResult;
    /*
    填充对应参数
    */
    auto ret = UBTurboRMRSAgentMigrateBack(migrateBackResult);
    return 0;
}
```

## UBTurboRMRSAgentBorrowRollBack:内存借用回滚

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
uint32_t UBTurboRMRSAgentBorrowRollBack(std::map<std::string, std::set<BorrowIdInfo>> &borrowIdsPidsMap);
```

### 描述 DESCRIPTION

内存借用回滚的迁回部分的回调函数。

### 参数 Parameters

| name             | IN/OUT | description                                                  |
| ---------------- | ------ | ------------------------------------------------------------ |
| borrowIdsPidsMap | IN     | std::map<std::string, std::set&lt;BorrowIdInfo&gt;><br/>（key为borrowId）<br/>struct BorrowIdInfo {<br/>    pid_t pid;<br/>    uint64_t oriSize;    // 对应pid之前占用远端内存大小<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成内存借用回滚的迁回部分。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentBorrowRollBack UBTurboRMRSAgentBorrowRollBack = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentBorrowRollBack = reinterpret_cast<UBTurboRMRSAgentBorrowRollBack>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentBorrowRollBack"));

    std::map<std::string, std::set<BorrowIdInfo>> curBorrowIdsPidsMap;
    /*
    填充对应参数
    */
    auto ret = UBTurboRMRSAgentBorrowRollBack(curBorrowIdsPidsMap);
    return 0;
}
```

## PidNumaInfoCollectRecvHandler:容器进程内存采集

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
uint32_t UBTurboRMRSAgentPidNumaInfoCollect(const PidNumaInfoCollectParam &pidNumaInfoCollectParam, PidNumaInfoCollectResult &pidNumaInfoCollectResult);
```

### 描述 DESCRIPTION

RMRS提供容器进程内存采集。

### 参数 Parameters

| name                     | IN/OUT | description                                                  |
| ------------------------ | ------ | ------------------------------------------------------------ |
| PidNumaInfoCollectParam  | IN     | class PidNumaInfoCollectParam : public Serializer { <br/>public: <br/>PidNumaInfoCollectParam() {} <br/>explicit PidNumaInfoCollectParam(std::vector<pid_t> pidList) : pidList(pidList) {} <br/>std::vector<pid_t> pidList{}; <br/> }; |
| PidNumaInfoCollectResult | OUT    | class PidNumaInfoCollectResult : public Serializer { <br/>public: <br/>PidNumaInfoCollectResult() {} <br/>explicit PidNumaInfoCollectResult(std::vector< mempooling::PidInfo> pidInfoList) <br/>: pidInfoList(pidInfoList) {} <br/>std::vector< mempooling::PidInfo> pidInfoList{};  <br/> |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成RMRS容器进程内存采集。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentPidNumaInfoCollect UBTurboRMRSAgentPidNumaInfoCollect = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentPidNumaInfoCollect =
        reinterpret_cast<UBTurboRMRSAgentPidNumaInfoCollect>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentPidNumaInfoCollect"));

    turbo::rmrs::PidNumaInfoCollectParam pidNumaInfoCollectParam;
    turbo::rmrs::PidNumaInfoCollectResult pidNumaInfoCollectResult;
    /*
    填充对应参数
    */
    auto ret = MempoolingMessage::UBTurboRMRSAgentPidNumaInfoCollect(pidNumaInfoCollectParam, pidNumaInfoCollectResult);
    return 0;
}
```

## UBTurboRMRSAgentNumaMemInfoCollect:容器进程内存采集

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
uint32_t UBTurboRMRSAgentNumaMemInfoCollect(const NumaMemInfoCollectParam &numaMemInfoCollectParam, ResponseInfoSimpo &responseInfoSimpo);
```

### 描述 DESCRIPTION

RMRS提供容器进程内存采集。

### 参数 Parameters

| name                    | IN/OUT | description                                                  |
| ----------------------- | ------ | ------------------------------------------------------------ |
| NumaMemInfoCollectParam | IN     | class NumaMemInfoCollectParam : public Serializer { <br/>public: <br/>NumaMemInfoCollectParam() {} <br/>explicit NumaMemInfoCollectParam(int numaId) : <br/>numaId(numaId) {} <br/>int numaId{}; <br/> |
| ResponseInfoSimpo       | OUT    | class ResponseInfoSimpo { <br/>public: ResponseInfoSimpo() = default;<br> explicit ResponseInfoSimpo(ResponseInfo responseInfoInput) : responseInfo_(std::move(responseInfoInput)) {}<br> inline ResponseInfo GetResponseInfo() { <br> return responseInfo_; <br> }<br> inline void SetResponseInfo(const int code, const std::string &message) { <br/>responseInfo_.code = code; <br/>responseInfo_.message = message;<br>  }<br> std::string ToString() const { <br/>return "code=" + std::to_string(responseInfo_.code) + ", message=" + responseInfo_.message; <br>}<br> ResponseInfo responseInfo_{}; <br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成RMRS容器进程内存采集。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentNumaMemInfoCollect UBTurboRMRSAgentNumaMemInfoCollect = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentNumaMemInfoCollect =
        reinterpret_cast<UBTurboRMRSAgentNumaMemInfoCollect>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentNumaMemInfoCollect"));

   turbo::rmrs::NumaMemInfoCollectParam numaMemInfoCollectParam;
   turbo::rmrs::ResponseInfoSimpo responseInfoSimpo;
    /*
    填充对应参数
    */
    auto ret = MempoolingMessage::UBTurboRMRSAgentNumaMemInfoCollect(numaMemInfoCollectParam, responseInfoSimpo);
    return 0;
}
```

## UBTurboRMRSAgentUCacheMigrateStrategy:pagecache迁移策略执行

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
#include "turbo_rmrs_interface.h"

uint32_t UBTurboRMRSAgentUCacheMigrateStrategy(const UCacheMigrationStrategyParam &uCacheMigrationStrategy, ResCode &rescode);
```

### 描述 DESCRIPTION

RMRS提供pagecache迁移策略执行。

### 参数 Parameters

| name                    | IN/OUT | description                                                  |
| ----------------------- | ------ | ------------------------------------------------------------ |
| uCacheMigrationStrategy | IN     | struct UCacheMigrationStrategyParam {<br/>    int16_t localNumaId;                 // 执行迁出的本地numa节点。若小于0，代表所有本地numa节点<br/>    std::vector<uint16_t> remoteNumaIds; // 执行迁入的远端内存呈现numa节点列表<br/>    std::vector<pid_t> pids;             // 需要迁移的进程列表<br/>    float ucacheUsageRatio;              // 给Pagecache分配使用的内存比例<br/>}; |
| resCode                 | OUT    | struct ResCode {<br/>    uint32_t resCode;<br/>};            |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成内存迁出策略调用。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentUCacheMigrateStrategy UBTurboRMRSAgentUCacheMigrateStrategy = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentUCacheMigrateStrategy = reinterpret_cast<UBTurboRMRSAgentUCacheMigrateStrategy>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentUCacheMigrateStrategy"));

    UCacheMigrationStrategyParam param;
    ResCode result;
    /*
    填充对应参数
    */
    auto ret = UBTurboRMRSAgentUCacheMigrateStrategy(param, result);
    return 0;
}
```

## UBTurboRMRSAgentUCacheMigrateStop:停止pagecache迁移

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
#include "turbo_rmrs_interface.h"

uint32_t UBTurboRMRSAgentUCacheMigrateStop(ResCode &rescode);
```

### 描述 DESCRIPTION

RMRS提供停止pagecache迁移。

### 参数 Parameters

| name    | IN/OUT | description                                       |
| ------- | ------ | ------------------------------------------------- |
| ResCode | OUT    | struct ResCode {<br/>    uint32_t resCode;<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成停止pagecache迁移。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentUCacheMigrateStop UBTurboRMRSAgentUCacheMigrateStop = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentUCacheMigrateStop = reinterpret_cast<UBTurboRMRSAgentUCacheMigrateStop>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentUCacheMigrateStop"));

    ResCode result;
    auto ret = UBTurboRMRSAgentUCacheMigrateStop(result);
    return 0;
}
```

## UBTurboRMRSAgentUpdateUCacheRatio:计算Pagecache远端内存使用比例

### 库 LIBRARY

UBTurbo客户端库 (libubturbo_client.so)

### 摘要 SYNOPSIS

```cpp
#include "turbo_rmrs_interface.h"

uint32_t UBTurboRMRSAgentUpdateUCacheRatio(const MigrationInfoParam &migrationInfoParam, UCacheRatioRes &uCacheRatioRes);
```

### 描述 DESCRIPTION

RMRS提供计算Pagecache远端内存使用比例。

### 参数 Parameters

| name               | IN/OUT | description                                                  |
| ------------------ | ------ | ------------------------------------------------------------ |
| MigrationInfoParam | IN     | struct MigrationInfoParam {<br/>    uint64_t borrowMemKB;    // 借用内存总大小，单位KB<br/>    std::vector<pid_t> pids; // 需要迁移的进程列表<br/>}; |
| UCacheRatioRes     | OUT    | struct UCacheRatioRes {<br/>    float ucacheUsageRatio; // ucache借用比例<br/>    uint32_t resCode;       // 是否执行成功<br/>}; |

### 返回值 RETURN VALUE

返回 `0` 表示成功，返回其他值表示失败，请见`错误 ERRORS`

### 错误 ERRORS

返回值1：通用错误码

返回值2：socket创建失败

返回值3：与server通信失败

返回值4：未找到对应函数

返回值5：函数执行错误

返回值6：函数返回值不合法

### 约束 CONSTRAINTS

暂无

### 附注 NOTES

暂无

### 样例 EXAMPLES

以下程序完成计算Pagecache远端内存使用比例。

```c
#include <iostream>
#include "turbo_rmrs_interface.h"

const char *const LIBUBTURBO_CLIENT_PATH = "/usr/lib64/libubturbo_client.so";
void *ubturboClientHandle = nullptr;
UBTurboRMRSAgentUpdateUCacheRatio UBTurboRMRSAgentUpdateUCacheRatio = nullptr;

int main()
{
    ubturboClientHandle = dlopen(LIBUBTURBO_CLIENT_PATH, RTLD_LAZY);
    UBTurboRMRSAgentUpdateUCacheRatio = reinterpret_cast<UBTurboRMRSAgentUpdateUCacheRatio>(dlsym(ubturboClientHandle, "UBTurboRMRSAgentUpdateUCacheRatio"));

    MigrationInfoParam param;
    UCacheRatioRes result;
    /*
    填充对应参数
    */
    auto ret = UBTurboRMRSAgentUpdateUCacheRatio(param, result);
    return 0;
}
```

## 新版补充说明

### 接口边界

RMRS 客户端接口声明在 `src/sdk/turbo_rmrs_interface.h`，位于 `turbo::rmrs` 命名空间。接口参数包含
STL 容器和 C++ 类，因此只面向与发布件 ABI 兼容的 C++ 调用方。

接口返回值首先表示 IPC 调用、handler 返回和响应反序列化是否成功。常见 IPC 错误码为 1–6，分别
对应通用错误、socket 创建失败、连接失败、服务不存在、handler 失败和响应无效。返回 `0` 不一定
代表业务目标已经完成：带 `resCode`、`result` 等业务字段的响应必须继续检查这些字段；迁出策略接口
即使未生成可执行方案也可能成功返回序列化结果，调用方还必须验证结果内容。

### 迁出策略

```cpp
uint32_t UBTurboRMRSAgentMigrateStrategy(
    const MigrateStrategyParamRMRS &param,
    MigrateStrategyResult &result);
```

输入包含候选 PID 及最大迁出比例、需要释放的本地内存量、PID 到远端 NUMA 的映射和超时 NUMA。
输出为每个 PID 的迁移大小、目的 NUMA 及建议等待时间。`borrowSize` 和
`VMMigrateOutParam.memSize` 的单位均为 KB；公共头文件的现有注释未完整标明这一点，调用方不得混用
字节、KB 和页面数。

### 迁出执行

```cpp
uint32_t UBTurboRMRSAgentMigrateExecute(
    const MigrateStrategyResult &result);
```

执行先前策略结果。调用方应保证结果未被其他事务修改，并在失败或部分成功时进入回滚流程。

### 迁回

```cpp
uint32_t UBTurboRMRSAgentMigrateBack(MigrateBackResult &result);
```

尝试将节点内相关远端内存迁回本地。除函数返回值外，还应读取 `result.result` 和 `result.numaIds`，判断
哪些 NUMA 可以或已经完成归还。

### 借用回滚

```cpp
uint32_t UBTurboRMRSAgentBorrowRollBack(
    std::map<std::string, std::set<BorrowIdInfo>> &borrowIdsPidsMap);
```

按借用标识、PID 和原始大小回滚先前事务。映射必须来自同一受信任业务事务，不能使用日志中复制的
标识重放请求。

### 信息采集

```cpp
uint32_t UBTurboRMRSAgentPidNumaInfoCollect(
    const PidNumaInfoCollectParam &param,
    PidNumaInfoCollectResult &result);

uint32_t UBTurboRMRSAgentNumaMemInfoCollect(
    const NumaMemInfoCollectParam &param,
    ResponseInfoSimpo &result);
```

进程结果包含本地 NUMA、近端/远端使用量和各 NUMA 元信息。采集结果具有时效性，不能作为后续
迁移时仍然有效的锁定快照。

### UCache 路径

```cpp
uint32_t UBTurboRMRSAgentUCacheMigrateStrategy(
    const UCacheMigrationStrategyParam &param,
    ResCode &result);
uint32_t UBTurboRMRSAgentUCacheMigrateStop(ResCode &result);
uint32_t UBTurboRMRSAgentUpdateUCacheRatio(
    const MigrationInfoParam &param,
    UCacheRatioRes &result);
```

只有 `rmrs.ucache.enable=true` 且 UCache 内核能力、设备和权限就绪时使用。接口返回成功后仍需检查
响应中的 `resCode`；比例必须在业务允许范围内，PID 列表必须经过授权。

### 主要数据结构

| 类型 | 关键字段 |
| --- | --- |
| `VMPresetParam` | `pid`、最大迁出 `ratio` |
| `MigrateStrategyParamRMRS` | PID 列表、`borrowSize`、远端映射、超时 NUMA |
| `VMMigrateOutParam` | `pid`、`memSize`、`desNumaId` |
| `MigrateStrategyResult` | 迁出列表、`waitingTime`（ms） |
| `PidInfo` | 本地 NUMA、近端/远端用量、NUMA 元信息 |
| `UCacheMigrationStrategyParam` | 本地/远端 NUMA、PID、UCache 比例 |

精确字段、类型和默认初始化值以同版本 `turbo_rmrs_interface.h` 为准。升级客户端或服务端时必须成对
验证 C++ ABI 和 JSON 序列化兼容性。
