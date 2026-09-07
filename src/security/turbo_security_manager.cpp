/*
 * rmrs is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "turbo_security_manager.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <new>
#include <vector>

#include <linux/capability.h>

#include <securec.h>

#include "turbo_error.h"

namespace turbo::security {
using namespace turbo::common;

constexpr unsigned long long CAP_SEGMENT_COUNT = 2; // cap_data 结构的数量（V3 标准，覆盖 64 位能力）

RetCode TurboSecurityManager::GetCapabilities()
{
    // 初始化 cap_header，version 3，pid = 0 表示当前进程
    __user_cap_header_struct capHeader{};
    capHeader.version = _LINUX_CAPABILITY_VERSION_3;
    capHeader.pid = 0;

    const auto data = new (std::nothrow) __user_cap_data_struct[CAP_SEGMENT_COUNT];
    if (data == nullptr) {
        std::cerr << "[Security] Allocate capability data failed." << std::endl;
        return TURBO_ERROR;
    }

    if (memset_s(data, sizeof(__user_cap_data_struct) * CAP_SEGMENT_COUNT, 0,
                 sizeof(__user_cap_data_struct) * CAP_SEGMENT_COUNT) != EOK) {
        delete[] data;
        return TURBO_ERROR;
    }

    errno = 0;
    if (syscall(SYS_capget, &capHeader, data) < 0) {
        std::cerr << "[Security] Failed to get capabilities, errno=" << errno << "." << std::endl;
        delete[] data;
        return TURBO_ERROR;
    }

    delete[] data;
    return TURBO_OK;
}

namespace {
// 读取其他进程 /proc/<pid>/{numa_maps,comm,cmdline} 所需的最小能力集
void SetCapabilitiesData(__user_cap_data_struct *capData)
{
    const std::vector<__u32> capabilities = {
        CAP_DAC_READ_SEARCH, // 绕过 DAC 读取其他 uid 拥有的 /proc/<pid>/* 文件
        CAP_SYS_PTRACE,      // 绕过 ptrace 访问校验，读取 /proc/<pid>/numa_maps 等受保护信息
    };

    for (const auto cap : capabilities) {
        capData[CAP_TO_INDEX(cap)].permitted |= CAP_TO_MASK(cap);
        capData[CAP_TO_INDEX(cap)].effective |= CAP_TO_MASK(cap);
    }
}
} // namespace

RetCode TurboSecurityManager::SetInitialCapabilities()
{
    __user_cap_header_struct capHeader{};
    capHeader.version = _LINUX_CAPABILITY_VERSION_3;
    capHeader.pid = 0;

    const auto capData = new (std::nothrow) __user_cap_data_struct[CAP_SEGMENT_COUNT];
    if (capData == nullptr) {
        std::cerr << "[Security] Allocate capability data failed." << std::endl;
        return TURBO_ERROR;
    }

    if (memset_s(capData, sizeof(__user_cap_data_struct) * CAP_SEGMENT_COUNT, 0,
                 sizeof(__user_cap_data_struct) * CAP_SEGMENT_COUNT) != EOK) {
        delete[] capData;
        return TURBO_ERROR;
    }

    SetCapabilitiesData(capData);

    errno = 0;
    if (syscall(SYS_capset, &capHeader, capData) < 0) {
        std::cerr << "[Security] Failed to set capabilities, errno=" << errno << "." << std::endl;
        delete[] capData;
        return TURBO_ERROR;
    }

    delete[] capData;
    return TURBO_OK;
}

} // namespace turbo::security
