/*
 * rmrs is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef TURBO_MODULE_SECURITY_H
#define TURBO_MODULE_SECURITY_H

#include "turbo_common.h"
#include "turbo_module.h"

namespace turbo::security {
using namespace turbo::common;
using namespace turbo::module;

// 安全模块：在其它模块运行前，将进程能力收敛到最小集（对齐 ubs-engine 的 security 方案）
class TurboModuleSecurity : public TurboModule {
public:
    RetCode Init() override;

    void UnInit() override;

    RetCode Start() override;

    void Stop() override;

    std::string Name() override;
};

} // namespace turbo::security

#endif // TURBO_MODULE_SECURITY_H
