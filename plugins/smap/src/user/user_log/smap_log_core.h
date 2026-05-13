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

#ifndef SMAP_LOG_CORE_H
#define SMAP_LOG_CORE_H

#include <stddef.h>
#include <pthread.h>
#include <stdio.h>

#define SMAP_LOG_MAX_PATH_LEN 512
#define SMAP_LOG_MAX_MSG_LEN 512
#define SMAP_LOG_MAX_FILE_SIZE_DEFAULT (200 * 1024 * 1024)
#define SMAP_LOG_MAX_FILE_COUNT_DEFAULT 50
#define NS_PER_USEC 1000

typedef enum SmapLogCoreLevel {
    SMAP_LOG_CORE_TRACE = 0,
    SMAP_LOG_CORE_DEBUG = 1,
    SMAP_LOG_CORE_INFO = 2,
    SMAP_LOG_CORE_WARN = 3,
    SMAP_LOG_CORE_ERROR = 4,
    SMAP_LOG_CORE_CRITICAL = 5,
    SMAP_LOG_CORE_BUTT
} SmapLogCoreLevel;

typedef struct SmapLogConfig {
    char filePath[SMAP_LOG_MAX_PATH_LEN];
    size_t maxFileSize;
    int maxFileCount;
    int minLogLevel;
} SmapLogConfig;

typedef struct SmapLogFile {
    char basePath[SMAP_LOG_MAX_PATH_LEN];
    size_t maxFileSize;
    int maxFileCount;
    int currentFileIndex;
    size_t currentFileSize;
    FILE *fileHandle;
    pthread_mutex_t lock;
} SmapLogFile;

int SmapLogCoreInit(const SmapLogConfig *config);
void SmapLogCoreExit(void);
int SmapLogCoreWrite(int level, const char *prefix, const char *message);
int SmapLogCoreGetMinLogLevel(void);

#endif /* SMAP_LOG_CORE_H */
