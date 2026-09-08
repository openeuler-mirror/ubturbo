/*
 * rmrs is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "turbo_module_security.h"

#include <iostream>

#include "turbo_error.h"
#include "turbo_security_manager.h"

namespace turbo::security {
using namespace turbo::common;

namespace {
RetCode SetMinCapabilities()
{
    // 必须先获取一次 Capabilities，否则无法成功设置 Capabilities
    if (TurboSecurityManager::GetCapabilities() != TURBO_OK) {
        std::cerr << "[Security] Get capabilities failed." << std::endl;
        return TURBO_ERROR;
    }
    if (TurboSecurityManager::SetInitialCapabilities() != TURBO_OK) {
        std::cerr << "[Security] Set initial capabilities failed." << std::endl;
        return TURBO_ERROR;
    }
    return TURBO_OK;
}
} // namespace

RetCode TurboModuleSecurity::Init()
{
    return SetMinCapabilities();
}

void TurboModuleSecurity::UnInit()
{
    // Do Nothing
}

RetCode TurboModuleSecurity::Start()
{
    return TURBO_OK;
}

void TurboModuleSecurity::Stop()
{
    // Do Nothing
}

std::string TurboModuleSecurity::Name()
{
    return "security";
}

} // namespace turbo::security
