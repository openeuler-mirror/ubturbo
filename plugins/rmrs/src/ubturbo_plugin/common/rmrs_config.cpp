/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "rmrs_config.h"

#include <cstdint>
#include <fstream>
#include "turbo_conf.h"
#include "turbo_logger.h"

namespace rmrs {
using namespace turbo::config;
using namespace turbo::log;

// smap模块持久化pageType的文件, 路径需与src/smap/server/turbo_module_smap.cpp中FILE_NAME保持一致
// pageType语义: 0-容器场景(4KB页), 1-虚机场景(2MB页)
constexpr const char *PAGE_TYPE_FILE = "/dev/shm/ubturbo_page_type.dat";

void RmrsConfig::RmrsLoadConfig()
{
    uint32_t ret = UBTurboGetBool("plugin_rmrs", "rmrs.ucache.enable", rmrsUCacheEnable);
    if (ret != RMRS_OK) {
        UBTURBO_LOG_WARN(RMRS_MODULE_NAME, RMRS_MODULE_CODE)
            << "Get config value failed, key=rmrs.ucache.enable, ret=" << ret << ", using default value false.";
        rmrsUCacheEnable = false;
    }

    LoadRmrsScene();
}

void RmrsConfig::LoadRmrsScene()
{
    std::ifstream inFile(PAGE_TYPE_FILE, std::ios::binary);
    if (!inFile) {
        // 首次启动或系统重启后pageType文件尚未生成, 保持未知场景, 由首个虚机业务惰性初始化libvirt
        UBTURBO_LOG_INFO(RMRS_MODULE_NAME, RMRS_MODULE_CODE)
            << "PageType file not exist, scene is unknown, wait for lazy init.";
        rmrsScene = RmrsScene::UNKNOWN;
        return;
    }
    uint32_t pageType = 0;
    inFile.read(reinterpret_cast<char *>(&pageType), sizeof(pageType));
    if (inFile.gcount() != sizeof(pageType)) {
        UBTURBO_LOG_WARN(RMRS_MODULE_NAME, RMRS_MODULE_CODE)
            << "PageType file invalid, scene is unknown, wait for lazy init.";
        rmrsScene = RmrsScene::UNKNOWN;
        return;
    }
    rmrsScene = static_cast<RmrsScene>(pageType);
    UBTURBO_LOG_INFO(RMRS_MODULE_NAME, RMRS_MODULE_CODE) << "Load scene from pageType file, pageType=" << pageType;
}

bool RmrsConfig::GetRmrsUcacheEnable()
{
    return rmrsUCacheEnable;
}

RmrsScene RmrsConfig::GetRmrsScene()
{
    return rmrsScene;
}

void RmrsConfig::SetRmrsScene(RmrsScene scene)
{
    rmrsScene = scene;
}

} // namespace rmrs