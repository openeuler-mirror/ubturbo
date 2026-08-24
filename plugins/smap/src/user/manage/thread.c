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
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "smap_user_log.h"
#include "securec.h"
#include "manage.h"
#include "thread.h"
#include "strategy/migration.h"

static void *ThreadMain(void *args)
{
    struct ProcessManager *manager = args;

    SMAP_LOGGER_INFO("ScanMigrate thread %lu created.", gettid());
    while (!EnvAtomicRead(&manager->scanMigrateStop)) {
        EnvMsleep(manager->migPeriod);
        ScanMigrateWork(manager);
    }
    return NULL;
}

int InitScanMigrateThread(struct ProcessManager *manager, uint32_t period)
{
    int ret;
    manager->migPeriod = period;
    EnvAtomicSet(&manager->scanMigrateStop, 0);
    /* 先清零 pthread_t，确保 pthread_create 失败时不残留未定义值，
     * DestroyScanMigrateThread 通过判断 pthread_t 是否为零来跳过无效 join */
    (void)memset_s(&manager->scanMigrateThread, sizeof(manager->scanMigrateThread), 0,
                   sizeof(manager->scanMigrateThread));
    ret = pthread_create(&manager->scanMigrateThread, NULL, ThreadMain, manager);
    if (ret) {
        SMAP_LOGGER_ERROR("Create scan migrate thread failed: %d.", ret);
        (void)memset_s(&manager->scanMigrateThread, sizeof(manager->scanMigrateThread), 0,
                       sizeof(manager->scanMigrateThread));
        return -ret;
    }
    return 0;
}

int DestroyScanMigrateThread(struct ProcessManager *manager)
{
    pthread_t zeroThread;
    (void)memset_s(&zeroThread, sizeof(zeroThread), 0, sizeof(zeroThread));

    EnvAtomicSet(&manager->scanMigrateStop, 1);
    /* 仅在 pthread_t 非零（即线程已成功创建）时才 join，避免对未初始化的 pthread_t 执行未定义行为 */
    if (memcmp(&manager->scanMigrateThread, &zeroThread, sizeof(pthread_t)) != 0) {
        pthread_join(manager->scanMigrateThread, NULL);
    }
    SMAP_LOGGER_INFO("ScanMigrate thread destroyed.");
    return 0;
}