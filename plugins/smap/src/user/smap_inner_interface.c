/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "smap_env.h"
#include "smap_inner_interface.h"

#include "manage/manage.h"
#include "advanced-strategy/scene.h"
#include "smap_user_log.h"
#include "smap_log_core.h"

int SmapEnableAdaptMem(int flag)
{
    if (flag == ENABLE_ADAPT_MEM) {
        SetAdaptMem(true);
        SMAP_LOGGER_INFO("Enable adapt mem ratio succeed!");
        return 0;
    } else if (flag == DISABLE_ADAPT_MEM) {
        SetAdaptMem(false);
        SMAP_LOGGER_INFO("Disable adapt mem ratio succeed!");
        return 0;
    } else {
        SMAP_LOGGER_ERROR("The arg: %d is invalid for adapt mem ratio!", flag);
        return -EINVAL;
    }
}

int SmapQueryVmMemRatio(struct VmRatioMsg *vrMsg)
{
    int ret = 0;
    if (!ubturbo_smap_is_running()) {
        SMAP_LOGGER_ERROR("Smap already stopped, SmapQueryVmMemRatio failed.");
        return -EPERM;
    }
    if (!vrMsg) {
        SMAP_LOGGER_ERROR("Query failed as msg is null.");
        return -EINVAL;
    }
    vrMsg->nrVm = 0;
    struct ProcessManager *manager = GetProcessManager();
    int nrLocalNuma = GetNrLocalNuma();
    struct PidSlot *all[MAX_PID_SLOTS];
    size_t n = PidSlotCollectRefs(manager, all, MAX_PID_SLOTS);
    for (size_t k = 0; k < n; k++) {
        ProcessAttr *current = all[k]->attr;
        int l1Node = GetAttrL1(current);
        int l2Node = GetAttrL2(current);

        if (l1Node < 0) {
            SMAP_LOGGER_ERROR("SmapQueryVmMemRatio pid %d L1 is invalid.", current->pid);
            ret = -EINVAL;
            PidSlotReleaseRefs(all, n);
            break;
        }
        if (current->type != VM_TYPE) {
            continue;
        }
        vrMsg->vr[vrMsg->nrVm].pid = current->pid;
        vrMsg->vr[vrMsg->nrVm].ratio = HUNDRED;
        if (l2Node >= nrLocalNuma) {
            vrMsg->vr[vrMsg->nrVm].ratio -= current->strategyAttr.l3RemoteMemRatio[l1Node][l2Node - nrLocalNuma];
        }
        vrMsg->nrVm++;
    }

    if (ret == 0) {
        PidSlotReleaseRefs(all, n);
    }

    return ret;
}

int SmapSetLogLevel(int level)
{
    if (level < SMAP_LOG_CORE_TRACE || level >= SMAP_LOG_CORE_BUTT) {
        SMAP_LOGGER_ERROR("Invalid log level %d, valid range: %d-%d.", level, SMAP_LOG_CORE_TRACE,
                          SMAP_LOG_CORE_CRITICAL);
        return -EINVAL;
    }
    SmapLogCoreSetMinLogLevel(level);
    SMAP_LOGGER_INFO("Log level set to %s.", SmapLogCoreGetMinLogLevel() == level ? "success" : "failed");
    return 0;
}
