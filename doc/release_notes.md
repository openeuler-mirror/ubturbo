# 版本说明

## 修改记录

|文档版本|发布日期|修改说明|
|---|---|---|
|01|2026-06-30|第一次正式发布|

## 版本配套说明

### 产品版本信息

|项目|内容|
|--|--|
|产品名称|UBTurbo|
|版本|master|

### 软件版本配套说明

|软件名称|软件版本|
|--|--|
|OS|openEuler 24.03 LTS 或更高版本|
|ubs engine|1.0.0 版本（[ubs-engine 官方指南](https://atomgit.com/openeuler/ubs-engine/blob/master/README.md)）|

### 硬件版本配套说明

|部件名称|硬件要求|
|--|--|
|CPU架构|aarch64|
|内存|64GB及以上|
|磁盘|SSD，IOPS 500MB/s|
|芯片互联|UB|
|网卡|可选依赖（可选使用TCP辅助UB建链，默认采用UB自举建链）|

## UBTurbo

### 更新说明

* 支持主机场景下Redis进程借用远端内存借用性能优化。
* SMAP支持大虚机场景多NUMA分级内存的冷热数据流动。
* RMRS提供接口支持大虚机场景使能虚机多NUMA内存冷热流动。
* RMRS支持大虚机场景大页资源采集及借用策略。
* RMRS提供虚机内存碎片故障场景内存借用和迁移并行处理能力。
  
### 已解决问题

无

### 遗留问题

无

## 版本配套文档

|文档名称|内容简介|
|----|---|
|《[API 文档](./docapi_docs_reference.md)》|本文档详细描述了UBTurbo对外提供的 API，包括API接口参数解释等。|
|《[开发者指南](./Developer_Guide.md)》|本文档介绍了基于UBTurbo框架的插件开发与调用机制，涵盖进程内外使用方式及核心接口说明。|
|《[设计文档](./Design_docs_Reference.md)》|本文档描述了UBTurbo项目结构化概览，包含摘要、设计、约束、策略及维护团队等核心内容。|
|《[SMAP 性能测试指南](./smap_performance_testing_guide.md)》|本文档描述了SMAP性能测试指南，包括内存分级、虚拟化约束及Redis/MySQL参数配置与测试步骤。|
|《[用户指南](./User_Guide.md)》|本文档描述了UBTurbo简介、安装部署及常用命令，涵盖用户组创建、配置修改及服务启停操作。|
|《[参考实践](./Tutorial.md)》|本文档描述了UBTurbo资源管理框架及RMRS内存调度工具的实现机制与应用实践。|
