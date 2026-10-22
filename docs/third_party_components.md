# 第三方开源组件

## 范围

本清单说明 UBTurbo 源码构建和测试直接使用的第三方组件。最终发布件中的精确依赖应以 RPM 查询、
ELF 动态依赖和对应发布包的许可证清单为准。

## 依赖清单

| 组件 | 用途 | 引入方式 | 是否进入运行时 |
| --- | --- | --- | --- |
| libboundscheck | 安全字符串与内存函数 | 系统包或源码子模块 | 动态依赖 |
| RapidJSON 1.1.0 | JSON 编解码 | 头文件/源码子模块 | 模板代码进入产物，无独立 `.so` |
| libvirt | RMRS 虚机信息与管理 | 系统开发包 | RMRS 动态依赖 |
| GoogleTest | 单元测试 | 测试子模块 | 否 |
| mockcpp | C/C++ mock | 测试子模块 | 否 |
| pthread、dl、rt 等系统库 | 线程、动态加载、时间等 | openEuler 系统库 | 是 |

SMAP 和 UBDMA 还依赖 Linux 内核接口及目标平台提供的内存/互联能力，这些内容不能仅按普通用户态
动态库理解。

## 构建与发布判断

- GoogleTest 和 mockcpp 只链接测试目标，不应出现在生产 RPM 的运行时依赖中。
- RapidJSON 是 header-only 依赖，应保留其许可证和版权声明。
- `libboundscheck` 与 `libvirt` 是否被动态链接，应对最终 ELF 执行 `readelf -d` 或 `ldd` 验证。
- 子模块内容由其上游许可证约束，不应批量改写其 README、版权头或许可证原文。

示例检查：

```bash
readelf -d dist/release/bin/ub_turbo_exec | grep NEEDED
readelf -d dist/release/lib/libubturbo_client.so | grep NEEDED
rpm -qpR <RPM_FILE>
```

`ldd` 会加载目标平台的动态链接器，只应对可信构建产物执行。

## 许可证说明

根项目条款见 [LICENSE](../LICENSE)。SMAP、UBDMA 内核代码和第三方组件可能采用不同许可证；分发时
应同时保留相应源码头、LICENSE 和 Third Party Open Source Software Notice。

仓库根 LICENSE 与部分 SPEC 文件中的许可证字段并不完全一致。本文件不对该差异作法律判断，也不
擅自修改任何许可证标识；正式发布前应由维护者和合规人员确认包元数据。

## 更新流程

增加、升级或移除依赖时，应同步检查：

1. `.gitmodules`、CMake 和 SPEC 的版本与来源。
2. 许可证、NOTICE、源码版权头和再分发义务。
3. 发布件是否新增动态依赖或静态代码。
4. 已知漏洞、维护状态和目标 openEuler 版本兼容性。
