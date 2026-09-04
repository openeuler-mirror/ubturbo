# UBTurbo 接口总览

UBTurbo 对外接口分为四类：IPC 通信（客户端调用 / 服务端注册）、配置读取、日志输出，以及插件加载/卸载入口与公共数据类型。各接口所属头文件与库如下。

| 头文件 | 安装路径 | 所属库 | 用途 |
| :--- | :--- | :--- | :--- |
| turbo_def.h | /usr/include | libubturbo_client.so / 框架内部 | 公共数据类型与 IPC 错误码 |
| turbo_ipc_client.h | /usr/include | /usr/lib64/libubturbo_client.so | 外部进程调用 IPC |
| turbo_ipc_server.h | /usr/include | 守护进程内部 | 插件注册/注销 IPC 回调 |
| turbo_conf.h | /usr/include | 守护进程内部 | 插件读取配置 |
| turbo_logger.h | /usr/include | 守护进程内部 | 模块日志输出 |

外部进程仅需链接 libubturbo_client.so 并包含 turbo_ipc_client.h；插件开发者随插件源码引入其余头文件。

# UBTurboGetUInt32: 获取指定的uint32类型配置项

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_conf.h"

uint32_t UBTurboGetUInt32(const std::string &section, const std::string &configKey, uint32_t &configValue);
```

## 描述 DESCRIPTION

获取指定的uint32类型配置项。

## 参数 Parameters

| name        | IN/OUT | description  |
| ----------- | ------ | ------------ |
| section     | IN     | 指定具体插件。要求格式为"plugin_<plugin_name>"，其中<plugin_name>必须是在ubturbo_plugin_admission.conf中指定的，例如"plugin_rmrs"。不符合要求的输入会导致获取配置项失败，返回错误码1。|
| configKey      | IN     | 配置项名称。必须是在plugin_<plugin_name>.conf中存在的配置项名称。否则返回错误1。配置文件中，1<= configKey长度 <= 256。 |
| configValue | OUT    | 配置项的值。执行成功时，configValue即为获取到的配置项的值。执行失败时，configValue不具有任何意义。配置文件中，1<= configValue长度 <= 256。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 配置项名称的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项的值的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项名称只能包含字母、数字、.、_、-
- 配置项的值只能包含字母、数字、.、_、-、:、,、/、;
- 执行成功时，configValue即为获取到的配置项的值
- 同一个配置文件中存在重复配置项则报错，且服务启动失败

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成获取指定的uint32类型配置项。

```cpp
#include "turbo_conf.h"

int main(void)
{
    const std::string section = "section";
    const std::string configKey = "configKey";
    uint32_t configVal;
    uint32_t ret = UBTurboGetUInt32(section, configKey, configVal);
    return 0;
}
```

# UBTurboGetFloat: 获取指定的float类型配置项

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_conf.h"

uint32_t UBTurboGetFloat(const std::string &section, const std::string &configKey, float &configValue);
```

## 描述 DESCRIPTION

获取指定的float类型配置项

## 参数 Parameters

| name        | IN/OUT | description  |
| ----------- | ------ | ------------ |
| section     | IN     | 指定具体插件。要求格式为"plugin_<plugin_name>"，其中<plugin_name>必须是在ubturbo_plugin_admission.conf中指定的，例如"plugin_rmrs"。不符合要求的输入会导致获取配置项失败，返回错误码1。|
| configKey      | IN     | 配置项名称。必须是在plugin_<plugin_name>.conf中存在的配置项名称。否则返回错误1。配置文件中，1<= configKey长度 <= 256。 |
| configValue | OUT    | 配置项的值。执行成功时，configValue即为获取到的配置项的值。执行失败时，configValue不具有任何意义。配置文件中，1<= configValue长度 <= 256。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 支持指数形式，例如-1.2e3、1E-4。
- 配置项名称的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项的值的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项名称只能包含字母、数字、.、_、-
- 配置项的值只能包含字母、数字、.、_、-、:、,、/、;
- 执行成功时，configValue即为获取到的配置项的值
- 同一个配置文件中存在重复配置项则报错，且服务启动失败

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成获取指定的float类型配置项。

```cpp
#include "turbo_conf.h"

int main(void)
{
    const std::string section = "section";
    const std::string configKey = "configKey";
    float configValue;
    uint32_t ret = UBTurboGetFloat(section, configKey, configValue);
    return 0;
}
```

# UBTurboGetStr: 获取指定的string类型配置项

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_conf.h"

uint32_t UBTurboGetStr(const std::string &section, const std::string &configKey, std::string &configValue);
```

## 描述 DESCRIPTION

获取指定的string类型配置项

## 参数 Parameters

| name        | IN/OUT | description  |
| ----------- | ------ | ------------ |
| section     | IN     | 指定具体插件。要求格式为"plugin_<plugin_name>"，其中<plugin_name>必须是在ubturbo_plugin_admission.conf中指定的，例如"plugin_rmrs"。不符合要求的输入会导致获取配置项失败，返回错误码1。|
| configKey      | IN     | 配置项名称。必须是在plugin_<plugin_name>.conf中存在的配置项名称。否则返回错误1。配置文件中，1<= configKey长度 <= 256。 |
| configValue | OUT    | 配置项的值。执行成功时，configValue即为获取到的配置项的值。执行失败时，configValue不具有任何意义。配置文件中，1<= configValue长度 <= 256。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 配置项名称的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项的值的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项名称只能包含字母、数字、.、_、-
- 配置项的值只能包含字母、数字、.、_、-、:、,、/、;
- 执行成功时，configValue即为获取到的配置项的值
- 同一个配置文件中存在重复配置项则报错，且服务启动失败
- 配置项的值前导、后导空白字符均会被删除

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成获取指定的string类型配置项。

```cpp
#include "turbo_conf.h"

int main(void)
{
    const std::string section = "section";
    const std::string configKey = "configKey";
    std::string configValue;
    uint32_t ret = UBTurboGetStr(section, configKey, configValue);
    return 0;
}
```

# UBTurboGetBool: 获取指定的bool类型配置项

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_conf.h"

uint32_t UBTurboGetBool(const std::string &section, const std::string &configKey, bool &configValue);
```

## 描述 DESCRIPTION

获取指定的bool类型配置项

## 参数 Parameters

| name        | IN/OUT | description  |
| ----------- | ------ | ------------ |
| section     | IN     | 指定具体插件。要求格式为"plugin_<plugin_name>"，其中<plugin_name>必须是在ubturbo_plugin_admission.conf中指定的，例如"plugin_rmrs"。不符合要求的输入会导致获取配置项失败，返回错误码1。|
| configKey      | IN     | 配置项名称。必须是在plugin_<plugin_name>.conf中存在的配置项名称。否则返回错误1。配置文件中，1<= configKey长度 <= 256。 |
| configValue | OUT    | 配置项的值。执行成功时，configValue即为获取到的配置项的值。执行失败时，configValue不具有任何意义。配置文件中，1<= configValue长度 <= 256。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 配置项的值支持true/false、0/1、yse/no，对大小写不敏感
- 配置项名称的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项的值的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项名称只能包含字母、数字、.、_、-
- 配置项的值只能包含字母、数字、.、_、-、:、,、/、;
- 执行成功时，configValue即为获取到的配置项的值
- 同一个配置文件中存在重复配置项则报错，且服务启动失败

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成获取指定的bool类型配置项。

```cpp
#include "turbo_conf.h"

int main(void)
{
    const std::string section = "section";
    const std::string configKey = "configKey";
    bool configValue;
    uint32_t ret = UBTurboGetBool(section, configKey, configValue);
    return 0;
}
```

# UBTurboGetUInt64: 获取指定的uint64类型配置项

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_conf.h"

uint32_t UBTurboGetUInt64(const std::string &section, const std::string &configKey, uint64_t &configValue);
```

## 描述 DESCRIPTION

获取指定的uint64类型配置项

## 参数 Parameters

| name        | IN/OUT | description  |
| ----------- | ------ | ------------ |
| section     | IN     | 指定具体插件。要求格式为"plugin_<plugin_name>"，其中<plugin_name>必须是在ubturbo_plugin_admission.conf中指定的，例如"plugin_rmrs"。不符合要求的输入会导致获取配置项失败，返回错误码1。|
| configKey      | IN     | 配置项名称。必须是在plugin_<plugin_name>.conf中存在的配置项名称。否则返回错误1。配置文件中，1<= configKey长度 <= 256。 |
| configValue | OUT    | 配置项的值。执行成功时，configValue即为获取到的配置项的值。执行失败时，configValue不具有任何意义。配置文件中，1<= configValue长度 <= 256。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 配置项名称的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项的值的长度区间为[1, 256]，否则报错，进程启动失败
- 配置项名称只能包含字母、数字、.、_、-
- 配置项的值只能包含字母、数字、.、_、-、:、,、/、;
- 执行成功时，configValue即为获取到的配置项的值
- 同一个配置文件中存在重复配置项则报错，且服务启动失败

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成获取指定的uint64_t类型配置项。

```cpp
#include "turbo_conf.h"

int main(void)
{
    const std::string section = "section";
    const std::string configKey = "configKey";
    uint64_t configValue;
    uint32_t ret = UBTurboGetUInt64(section, configKey, configValue);
    return 0;
}
```

# UBTurboFunctionCaller: 客户端通过进程间通信调用服务端函数

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_ipc_client.h"

uint32_t UBTurboFunctionCaller(const std::string &function, const TurboByteBuffer &params, TurboByteBuffer &result);
```

## 描述 DESCRIPTION

客户端通过进程间通信调用服务端函数。

## 参数 Parameters

| name     | IN/OUT | description                                                  |
| -------- | ------ | ------------------------------------------------------------ |
| function | IN     | 函数名。                                                     |
| params   | IN     | 客户端发送给服务端的数据，由调用方进行序列化。               |
| result   | OUT    | 服务端返回给客户端的数据，由调用方进行反序列化。result.data的内存通过new进行分配，应该由调用方通过delete进行释放。 |

## 返回值 RETURN VALUE

返回值0：表示成功。
返回值1：通用错误码。
返回值2：socket失效。
返回值3：连接断开。
返回值4：服务端无对应函数。
返回值5：服务端对应函数执行错误。
返回值6：服务端对应函数出参范围异常。

## 约束 CONSTRAINTS

暂无

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成客户端通过进程间通信调用服务端函数。

```cpp
#include "turbo_ipc_client.h"

int main(void)
{
    TurboByteBuffer params;
    TurboByteBuffer result;
    params.data = new uint8_t[5];
    params.len = 5;
    params.data[0] = 'h';
    params.data[1] = 'e';
    params.data[2] = 'l';
    params.data[3] = 'l';
    params.data[4] = 'o';
    uint32_t ret = UBTurboFunctionCaller("function", params, result);
    return 0;
}
```

# SetIpcTimeLimit: 配置IPC客户端超时时间

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_ipc_client.h"

uint32_t SetIpcTimeLimit(uint32_t timeLimit);
```

## 描述 DESCRIPTION

客户端通过进程间通信调用服务端函数。

## 参数 Parameters

| name     | IN/OUT | description                                                  |
| -------- | ------ | ------------------------------------------------------------ |
| timeLimit | IN     | 超时时间，单位秒，默认值60s                               |

## 返回值 RETURN VALUE

返回值0：表示成功。
返回值1：失败。

## 约束 CONSTRAINTS

暂无

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成客户端超时时间配置。

```cpp
#include "turbo_ipc_client.h"

int main(void)
{
    uint32_t timeLimit = 180;
    uint32_t ret = SetIpcTimeLimit(timeLimit);
    return 0;
}
```

# UBTurboRegIpcService: 服务端注册回调函数

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_ipc_server.h"

uint32_t UBTurboRegIpcService(const std::string &name, IpcHandlerFunc function);
```

## 描述 DESCRIPTION

服务端注册回调函数。

## 参数 Parameters

| name     | IN/OUT | description                          |
| -------- | ------ | ------------------------------------ |
| name     | IN     | 服务端回调函数的名称。非空字符串，长度区间[1, 128]且无空格。|
| function | IN     | 服务端用于处理客户端请求的回调函数。 |

## 返回值 RETURN VALUE

返回值0：表示成功。
返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 同名函数不能重复注册
- 回调函数中禁止嵌套调用UBTurboRegIpcService

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成服务端注册回调函数。

```cpp
#include "turbo_ipc_server.h"

uint32_t MemBorrowRollbackRecvHandler(const TurboByteBuffer &req, TurboByteBuffer &resp)
{
    /*
    对应实现
    */
    return 0;
}

int main(void)
{
    uint32_t ret = UBTurboRegIpcService("MemBorrowRollback", MemBorrowRollbackRecvHandler);
    return 0;
}
```

# UBTurboUnRegIpcService: 服务端解注册回调函数

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_ipc_server.h"

uint32_t UBTurboUnRegIpcService(const std::string &name);
```

## 描述 DESCRIPTION

服务端解注册回调函数。

## 参数 Parameters

| name | IN/OUT | description              |
| ---- | ------ | ------------------------ |
| name | IN     | 已注册的函数名，字符串。 |

## 返回值 RETURN VALUE

返回值0：表示成功。
返回非0错误码：表示失败。

## 约束 CONSTRAINTS

- 已注册的服务解注册成功后，再次解注册，返回失败
- 回调函数中禁止嵌套调用UBTurboUnRegIpcService

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成服务端解注册。

```cpp
#include "turbo_ipc_server.h"


int main(void)
{
    uint32_t ret = UBTurboUnRegIpcService("MemBorrowRollback");
    return 0;
}
```

# UBTURBO_LOG_CRIT: 创建单条CRIT级别日志，写入终端或文件

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

UBTURBO_LOG_CRIT(moduleName, moduleId) << args;
```

## 描述 DESCRIPTION

创建单条CRIT级别日志，写入终端或文件。

## 参数 Parameters

| name       | IN/OUT | description        |
| ---------- | ------ | ------------------ |
| moduleName | IN     | 打印日志的模块名。；非空，以'\0'结尾。传入nullptr时，使用默认值'\0'，此时日志打印在.log文件中。 |
| moduleId   | IN     | 打印日志的模块id。moduleid作为保留字段，当前无意义。 |
| args       | IN     | 需要打印的信息。 可打印的类型，支持通过operator <<打印。  |

## 返回值 RETURN VALUE

返回值true：表示成功。

返回值false：表示失败。

## 约束 CONSTRAINTS

- 打印内容输出格式为：[时间][CRIT][进程号][线程号][代码所在文件及行数]日志信息。
- 插件/模块可以通过modulename指定日志的输出文件，modulename作为日志文件的文件名。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成创建单条CRIT级别日志，写入终端或文件。

```cpp
#include "turbo_logger.h"


int main(void)
{
    UBTURBO_LOG_CRIT(moduleName, moduleId) << "This is CRIT log";
    return 0;
}
```

# UBTURBO_LOG_ERROR: 创建单条ERROR级别日志，写入终端或文件

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

UBTURBO_LOG_ERROR(moduleName, moduleId) << args;
```

## 描述 DESCRIPTION

创建单条ERROR级别日志，写入终端或文件。

## 参数 Parameters

| name       | IN/OUT | description        |
| ---------- | ------ | ------------------ |
| moduleName | IN     | 打印日志的模块名。；非空，以'\0'结尾。传入nullptr时，使用默认值'\0'，此时日志打印在.log文件中。 |
| moduleId   | IN     | 打印日志的模块id。moduleid作为保留字段，当前无意义。 |
| args       | IN     | 需要打印的信息。 可打印的类型，支持通过operator <<打印。  |

## 返回值 RETURN VALUE

返回值true：表示成功。

返回值false：表示失败。

## 约束 CONSTRAINTS

- 打印内容输出格式为：[时间][CRIT][进程号][线程号][代码所在文件及行数]日志信息。
- 插件/模块可以通过modulename指定日志的输出文件，modulename作为日志文件的文件名。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成创建单条ERROR级别日志，写入终端或文件。

```cpp
#include "turbo_logger.h"


int main(void)
{
    UBTURBO_LOG_ERROR(moduleName, moduleId) << "This is ERROR log";
    return 0;
}
```

# UBTURBO_LOG_WARN: 创建单条WARN级别日志，写入终端或文件

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

UBTURBO_LOG_WARN(moduleName, moduleId) << args;
```

## 描述 DESCRIPTION

创建单条WARN级别日志，写入终端或文件。

## 参数 Parameters

| name       | IN/OUT | description        |
| ---------- | ------ | ------------------ |
| moduleName | IN     | 打印日志的模块名。；非空，以'\0'结尾。传入nullptr时，使用默认值'\0'，此时日志打印在.log文件中。 |
| moduleId   | IN     | 打印日志的模块id。moduleid作为保留字段，当前无意义。 |
| args       | IN     | 需要打印的信息。 可打印的类型，支持通过operator <<打印。  |

## 返回值 RETURN VALUE

返回值true：表示成功。

返回值false：表示失败。

## 约束 CONSTRAINTS

- 打印内容输出格式为：[时间][CRIT][进程号][线程号][代码所在文件及行数]日志信息。
- 插件/模块可以通过modulename指定日志的输出文件，modulename作为日志文件的文件名。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成创建单条WARN级别日志，写入终端或文件。

```cpp
#include "turbo_logger.h"


int main(void)
{
    UBTURBO_LOG_WARN(moduleName, moduleId) << "This is WARN log";
    return 0;
}
```

# UBTURBO_LOG_INFO: 创建单条INFO级别日志，写入终端或文件

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

UBTURBO_LOG_INFO(moduleName, moduleId) << args;
```

## 描述 DESCRIPTION

创建单条INFO级别日志，写入终端或文件。

## 参数 Parameters

| name       | IN/OUT | description        |
| ---------- | ------ | ------------------ |
| moduleName | IN     | 打印日志的模块名。；非空，以'\0'结尾。传入nullptr时，使用默认值'\0'，此时日志打印在.log文件中。 |
| moduleId   | IN     | 打印日志的模块id。moduleid作为保留字段，当前无意义。 |
| args       | IN     | 需要打印的信息。 可打印的类型，支持通过operator <<打印。  |

## 返回值 RETURN VALUE

返回值true：表示成功。

返回值false：表示失败。

## 约束 CONSTRAINTS

- 打印内容输出格式为：[时间][CRIT][进程号][线程号][代码所在文件及行数]日志信息。
- 插件/模块可以通过modulename指定日志的输出文件，modulename作为日志文件的文件名。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成创建单条INFO级别日志，写入终端或文件。

```cpp
#include "turbo_logger.h"


int main(void)
{
    UBTURBO_LOG_INFO(moduleName, moduleId) << "This is INFO log";
    return 0;
}
```

# UBTURBO_LOG_DEBUG: 创建单条DEBUG级别日志，写入终端或文件

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

UBTURBO_LOG_DEBUG(moduleName, moduleId) << args;
```

## 描述 DESCRIPTION

创建单条DEBUG级别日志，写入终端或文件。

## 参数 Parameters

| name       | IN/OUT | description        |
| ---------- | ------ | ------------------ |
| moduleName | IN     | 打印日志的模块名。；非空，以'\0'结尾。传入nullptr时，使用默认值'\0'，此时日志打印在.log文件中。 |
| moduleId   | IN     | 打印日志的模块id。moduleid作为保留字段，当前无意义。 |
| args       | IN     | 需要打印的信息。 可打印的类型，支持通过operator <<打印。  |

## 返回值 RETURN VALUE

返回值true：表示成功。
返回值false：表示失败。

## 约束 CONSTRAINTS

- 打印内容输出格式为：[时间][CRIT][进程号][线程号][代码所在文件及行数]日志信息。
- 插件/模块可以通过modulename指定日志的输出文件，modulename作为日志文件的文件名。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序完成创建单条DEBUG级别日志，写入终端或文件。

```cpp
#include "turbo_logger.h"


int main(void)
{
    UBTURBO_LOG_DEBUG(moduleName, moduleId) << "This is DEBUG log";
    return 0;
}
```

# RACK_DEFINE_THIS_MODULE: 声明日志模块名与模块码

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

RACK_DEFINE_THIS_MODULE(mn, mid);
```

## 描述 DESCRIPTION

在模块/插件实现文件中声明模块名 mn 与模块码 mid，生成内部静态变量 gModuleName 与 gModuleId，供后续 UBTURBO_LOG_* 宏定位日志归属文件。每个使用日志的编译单元须先调用本宏。

## 参数 Parameters

| name       | IN/OUT | description                                                        |
| ---------- | ------ | ------------------------------------------------------------------ |
| mn         | IN     | 模块名（C字符串）。非空，以'\0'结尾。用作日志输出文件名。          |
| mid        | IN     | 模块码（uint32_t）。保留字段，当前无意义。                         |

## 返回值 RETURN VALUE

无（宏展开为静态变量声明）。

## 约束 CONSTRAINTS

- 每个使用 UBTURBO_LOG_* 宏的编译单元须先调用本宏，否则编译报错。
- mn 决定日志输出文件名，建议与插件名一致。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序声明模块名与模块码后输出日志。

```cpp
#include "turbo_logger.h"

RACK_DEFINE_THIS_MODULE("rmrs", 777)

int main(void)
{
    UBTURBO_LOG_INFO(gModuleName, gModuleId) << "start";
    return 0;
}
```

# TurboLogOutput: 三方库日志输出

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_logger.h"

void TurboLogOutput(const char *moduleName, TurboLogLevel level, const char *msg);
```

## 描述 DESCRIPTION

面向三方库/非模块化代码的日志输出接口。将 msg 以指定 level 写入 moduleName 对应的日志文件。不依赖 RACK_DEFINE_THIS_MODULE 声明，便于外部代码接入 UBTurbo 日志体系。

## 参数 Parameters

| name       | IN/OUT | description                                                        |
| ---------- | ------ | ------------------------------------------------------------------ |
| moduleName | IN     | 模块名（C字符串）。非空，以'\0'结尾。决定日志落盘文件名。         |
| level      | IN     | 日志级别。取值 DEBUG/INFO/WARN/ERROR/CRIT（TurboLogLevel 枚举）。  |
| msg        | IN     | 日志文本（C字符串）。                                             |

## 返回值 RETURN VALUE

无。

## 约束 CONSTRAINTS

- level 取值需在 TurboLogLevel 枚举范围内。
- 低于 ubturbo.conf 中 log.level 设定等级的日志不会被输出。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序输出一条 INFO 级别日志。

```cpp
#include "turbo_logger.h"

int main(void)
{
    TurboLogOutput("smap", TurboLogLevel::INFO, "scan period started");
    return 0;
}
```

# TurboByteBuffer: IPC通用数据载体

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_def.h"

using TurboByteBufferFreeFunc = std::function<void(uint8_t *data)>;

struct TurboByteBuffer {
    uint8_t *data = nullptr;           // 数据指针
    size_t len = 0;                    // 数据长度
    TurboByteBufferFreeFunc freeFunc;  // 非空代表接收方需要释放内存；空代表接收方不需要释放内存
};
```

## 描述 DESCRIPTION

UBTurbo IPC 的通用数据载体，承载序列化后的入参与出参。data 指向字节缓冲，len 为有效字节长度，freeFunc 表达缓冲的释放归属。

## 参数 Parameters

| name     | IN/OUT | description                                                                  |
| -------- | ------ | ----------------------------------------------------------------------------- |
| data     | IN/OUT | 数据指针，指向字节缓冲。                                                      |
| len      | IN/OUT | 数据长度，单位字节。                                                          |
| freeFunc | IN/OUT | 释放函数。非空代表接收方需通过该函数释放 data；空代表接收方不需要释放内存。   |

## 返回值 RETURN VALUE

无（数据结构）。

## 约束 CONSTRAINTS

- 作为入参（UBTurboFunctionCaller 的 params）：由调用方拥有，freeFunc 通常为空。
- 作为出参（result）：result.data 通过 new 分配，应由调用方通过 delete 释放。
- 作为回调出参（IpcHandlerFunc 的 output）：由回调分配，框架在合适时机释放。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序构造入参缓冲并调用 IPC。

```cpp
#include "turbo_ipc_client.h"

int main(void)
{
    TurboByteBuffer params;
    params.data = new uint8_t[5]{'h','e','l','l','o'};
    params.len = 5;
    TurboByteBuffer result;
    uint32_t ret = UBTurboFunctionCaller("function", params, result);
    delete[] params.data;
    if (result.data) { delete[] result.data; }
    return 0;
}
```

# IpcHandlerFunc: IPC回调函数签名

## 框架 FRAMEWORK

UBTurbo框架

## 摘要 SYNOPSIS

```cpp
#include "turbo_def.h"

using IpcHandlerFunc = std::function<uint32_t(const TurboByteBuffer &inputBuffer, TurboByteBuffer &outputBuffer)>;
```

## 描述 DESCRIPTION

IPC 处理回调的统一签名。inputBuffer 为外部进程经 UBTurboFunctionCaller 传入的请求数据（只读），outputBuffer 由回调填充返回数据。返回 0 表示处理成功，非 0 表示处理失败（对应客户端 UBTurboFunctionCaller 收到错误码 5）。

## 参数 Parameters

| name        | IN/OUT | description                                                       |
| ----------- | ------ | ----------------------------------------------------------------- |
| inputBuffer | IN     | 客户端传入的请求数据，由调用方序列化。                            |
| outputBuffer | OUT   | 回调填充的返回数据，由回调序列化。outputBuffer.data 通过 new 分配。 |

## 返回值 RETURN VALUE

返回值0：表示成功。

返回非0错误码：表示失败，客户端 UBTurboFunctionCaller 收到错误码5。

## 约束 CONSTRAINTS

- 回调中禁止嵌套调用 UBTurboRegIpcService / UBTurboUnRegIpcService。
- outputBuffer 的内存须与 TurboByteBuffer 的释放约定一致。

## 附注 NOTES

暂无

## 样例 EXAMPLES

以下程序定义并注册一个 IPC 回调。

```cpp
#include "turbo_ipc_server.h"

uint32_t QueryStatusHandler(const TurboByteBuffer &input, TurboByteBuffer &output)
{
    /* 解析 input、执行业务、填充 output */
    return 0;
}

int main(void)
{
    uint32_t ret = UBTurboRegIpcService("QueryStatus", QueryStatusHandler);
    return 0;
}
```

# 错误码

UBTurbo IPC 错误码定义于 turbo_def.h，由 UBTurboFunctionCaller 返回。

| 错误码宏 | 值 | 含义 |
| -------- | -- | ---- |
| IPC_OK | 0 | 成功。 |
| IPC_ERROR | 1 | 通用错误码。 |
| IPC_BAD_SOCKET | 2 | socket失效。 |
| IPC_BAD_CONNECT | 3 | 连接断开。 |
| IPC_NO_FUNC | 4 | 服务端无对应函数。 |
| IPC_FUNC_ERROR | 5 | 服务端对应函数执行错误。 |
| IPC_INVALID_RESULT | 6 | 服务端对应函数出参范围异常。 |

守护进程内部接口（UBTurboRegIpcService、UBTurboUnRegIpcService、UBTurboGet*、TurboPluginInit 等）返回 0 表示成功，非 0 表示失败。
