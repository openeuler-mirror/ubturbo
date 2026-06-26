/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 */
#include "turbo_conf.h"
namespace turbo::config {
constexpr uint32_t CONFIGVALUE = 5;

/**
 * @brief 获取无符号短整型类型配置
 * @param section [in] 配置节
 * @param configKey [in] 配置参数key
 * @param configValue [out] 配置参数的值
 * @return #TURBO_OK 0 成功
 * @return #非TURBO_OK 失败
 */
uint32_t UBTurboGetUInt32(const std::string &section, const std::string &configKey, uint32_t &configValue)
{
    if (configKey == "cpuboostllc.monitor.interval") {
        configValue = CONFIGVALUE;
    }
    return 0;
}

/**
 * @brief 获取字符串类型配置
 * @param section [in] 配置节
 * @param configKey [in] 配置参数key
 * @param configValue [out] 配置参数的值
 * @return #TURBO_OK 0 成功
 * @return #非TURBO_OK 失败
 */
uint32_t UBTurboGetStr(const std::string &section, const std::string &configKey, std::string &configValue)
{
    if (configKey == "cpuboostllc.monitor.blackList") {
        configValue = "ps,htop";
    } else if (configKey == "cpuboostllc.collect.pmu") {
        configValue = "0x0,0x2";
    }
    return 0;
}

/**
 * @brief 获取布尔类型配置
 * @param section [in] 配置节
 * @param configKey [in] 配置参数key
 * @param configValue [out] 配置参数的值
 * @return #TURBO_OK 0 成功
 * @return #非TURBO_OK 失败
 */
uint32_t UBTurboGetBool(const std::string &section, const std::string &configKey, bool &configValue)
{
    return 0;
}

} // namespace turbo::config
