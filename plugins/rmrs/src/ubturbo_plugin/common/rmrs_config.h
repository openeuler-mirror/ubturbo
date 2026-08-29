/*
 * rmrs is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef RMRS_CONFIGURATION_H
#define RMRS_CONFIGURATION_H

#include <string>
#include "rmrs_error.h"

namespace rmrs {

#define RMRS_MODULE_NAME RmrsConfig::Instance().GetModuleName()
#define RMRS_MODULE_CODE RmrsConfig::Instance().GetModuleCode()
#define RMRS_CONFIG_NAME RmrsConfig::Instance().GetConfigName()

// rmrs部署场景: 虚机场景强依赖libvirt, 容器场景不依赖libvirt, 场景由smap的pageType驱动判定
// 枚举值与pageType取值对齐: 1-虚机(2MB页), 0-容器(4KB页); UNKNOWN表示pageType未知, 待惰性判定
enum class RmrsScene
{
    UNKNOWN = -1,
    CONTAINER = 0,
    VM = 1,
};

class RmrsConfig {
public:
    static RmrsConfig &Instance()
    {
        static RmrsConfig instance;
        return instance;
    }

    RmrsConfig(const RmrsConfig &) = delete;
    RmrsConfig &operator=(const RmrsConfig &) = delete;
    RmrsConfig(RmrsConfig &&) = delete;
    RmrsConfig &operator=(RmrsConfig &&) = delete;

    inline const char *GetModuleName()
    {
        return moduleName.c_str();
    }

    inline const char *GetConfigName()
    {
        return configName.c_str();
    }

    inline uint16_t GetModuleCode()
    {
        return moduleCode;
    }

    bool GetRmrsUcacheEnable();

    RmrsScene GetRmrsScene();

    void SetRmrsScene(RmrsScene scene);

    inline RmrsResult Init(const uint16_t modCode)
    {
        moduleCode = modCode;
        RmrsLoadConfig();
        return RMRS_OK;
    }

    inline long GetBasePageSize()
    {
        return basePageSize;
    }

    inline void SetBasePageSize(long pageSize)
    {
        basePageSize = pageSize;
    }

private:
    RmrsConfig() = default;
    void RmrsLoadConfig();
    void LoadRmrsScene();

    std::string moduleName = "rmrs";
    uint16_t moduleCode = 0;
    std::string configName = "plugin_rmrs";
    bool rmrsUCacheEnable = false;
    RmrsScene rmrsScene = RmrsScene::UNKNOWN; // 默认未知场景, 待pageType文件读取或业务惰性判定
    long basePageSize{};
};
} // namespace rmrs

#endif
