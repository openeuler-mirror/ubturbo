/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "smap_log_core.h"

static SmapLogFile g_smapLogFile;
static int g_minLogLevel = SMAP_LOG_CORE_DEBUG;
static int g_initialized = 0;

static const char *LogLevelToString(int level)
{
    static const char *levelNames[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL" };
    if (level >= 0 && level < SMAP_LOG_CORE_BUTT) {
        return levelNames[level];
    }
    return "UNKNOWN";
}

static int GetTimestamp(char *buffer, size_t bufSize)
{
    int ret;
    struct timespec ts;
    struct tm tmInfo;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmInfo);

    ret = snprintf_s(buffer, bufSize, bufSize - 1, "%04d-%02d-%02d %02d:%02d:%02d.%06ld", tmInfo.tm_year + 1900,
                     tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec,
                     ts.tv_nsec / NS_PER_USEC);
    if (ret == -1) {
        return -EINVAL;
    }
    return 0;
}

static FILE *OpenLogFile(const char *basePath)
{
    char filePath[SMAP_LOG_MAX_PATH_LEN];
    FILE *fp = NULL;
    int ret;

    ret = snprintf_s(filePath, sizeof(filePath), sizeof(filePath) - 1, "%s", basePath);
    if (ret == -1) {
        return NULL;
    }

    fp = fopen(filePath, "a");
    if (fp == NULL) {
        return NULL;
    }

    return fp;
}

static size_t GetFileSize(const char *filePath)
{
    struct stat st;
    if (stat(filePath, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

static void WriteLogHeader(FILE *fp)
{
#ifndef RELEASE
    (void)fprintf(fp, "Log started at [DEBUG] level\n");
#else
    (void)fprintf(fp, "Log started at [INFO] level\n");
#endif
    (void)fprintf(fp, "Log default format: yyyy-mm-dd hh:mm:ss.uuuuuu threadid loglevel msg\n");
}

static int RotateLogFile(void)
{
    char oldPath[SMAP_LOG_MAX_PATH_LEN];
    char newPath[SMAP_LOG_MAX_PATH_LEN];
    int ret;
    int i;

    if (g_smapLogFile.fileHandle != NULL) {
        fclose(g_smapLogFile.fileHandle);
        g_smapLogFile.fileHandle = NULL;
    }

    for (i = g_smapLogFile.maxFileCount - 1; i > 0; i--) {
        ret = snprintf_s(oldPath, sizeof(oldPath), sizeof(oldPath) - 1, "%s.%d", g_smapLogFile.basePath, i - 1);
        if (ret == -1) {
            return -1;
        }
        ret = snprintf_s(newPath, sizeof(newPath), sizeof(newPath) - 1, "%s.%d", g_smapLogFile.basePath, i);
        if (ret == -1) {
            return -1;
        }

        if (access(oldPath, F_OK) == 0) {
            if (i == g_smapLogFile.maxFileCount - 1) {
                remove(newPath);
            }
            rename(oldPath, newPath);
        }
    }

    ret = snprintf_s(newPath, sizeof(newPath), sizeof(newPath) - 1, "%s.0", g_smapLogFile.basePath);
    if (ret == -1) {
        return -1;
    }
    if (access(g_smapLogFile.basePath, F_OK) == 0) {
        rename(g_smapLogFile.basePath, newPath);
    }

    g_smapLogFile.currentFileIndex = 0;
    g_smapLogFile.currentFileSize = 0;
    g_smapLogFile.fileHandle = OpenLogFile(g_smapLogFile.basePath);

    if (g_smapLogFile.fileHandle != NULL) {
        WriteLogHeader(g_smapLogFile.fileHandle);
    }

    return g_smapLogFile.fileHandle != NULL ? 0 : -1;
}

int SmapLogCoreInit(const SmapLogConfig *config)
{
    int ret;
    if (config == NULL || config->filePath[0] == '\0') {
        return -EINVAL;
    }

    if (g_initialized) {
        return 0;
    }

    pthread_mutex_init(&g_smapLogFile.lock, NULL);

    ret = snprintf_s(g_smapLogFile.basePath, sizeof(g_smapLogFile.basePath), sizeof(g_smapLogFile.basePath) - 1, "%s",
                     config->filePath);
    if (ret == -1) {
        return -EINVAL;
    }

    g_smapLogFile.maxFileSize = config->maxFileSize > 0 ? config->maxFileSize : SMAP_LOG_MAX_FILE_SIZE_DEFAULT;
    g_smapLogFile.maxFileCount = config->maxFileCount > 0 ? config->maxFileCount : SMAP_LOG_MAX_FILE_COUNT_DEFAULT;
    g_minLogLevel = config->minLogLevel;

    g_smapLogFile.currentFileIndex = 0;
    g_smapLogFile.currentFileSize = GetFileSize(config->filePath);
    g_smapLogFile.fileHandle = OpenLogFile(config->filePath);

    if (g_smapLogFile.fileHandle == NULL) {
        pthread_mutex_destroy(&g_smapLogFile.lock);
        return -EIO;
    }

    WriteLogHeader(g_smapLogFile.fileHandle);
    g_initialized = 1;

    return 0;
}

void SmapLogCoreExit(void)
{
    if (!g_initialized) {
        return;
    }

    pthread_mutex_lock(&g_smapLogFile.lock);

    if (g_smapLogFile.fileHandle != NULL) {
        fclose(g_smapLogFile.fileHandle);
        g_smapLogFile.fileHandle = NULL;
    }

    pthread_mutex_unlock(&g_smapLogFile.lock);
    pthread_mutex_destroy(&g_smapLogFile.lock);
    g_initialized = 0;
}

int SmapLogCoreWrite(int level, const char *prefix, const char *message)
{
    char timestamp[64];
    char logLine[SMAP_LOG_MAX_MSG_LEN * 2] = { 0 };
    size_t lineLen;
    int ret;

    if (!g_initialized) {
        return -EINVAL;
    }

    if (level < g_minLogLevel || level >= SMAP_LOG_CORE_BUTT) {
        return 0;
    }

    if (prefix == NULL || message == NULL) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_smapLogFile.lock);

    if (g_smapLogFile.fileHandle == NULL) {
        pthread_mutex_unlock(&g_smapLogFile.lock);
        return -EIO;
    }

    ret = GetTimestamp(timestamp, sizeof(timestamp));
    if (ret < 0) {
        pthread_mutex_unlock(&g_smapLogFile.lock);
        return -EINVAL;
    }

    ret = snprintf_s(logLine, sizeof(logLine), sizeof(logLine) - 1, "%s %d %s [%s] %s\n", timestamp, getpid(),
                     LogLevelToString(level), prefix, message);
    if (ret == -1) {
        pthread_mutex_unlock(&g_smapLogFile.lock);
        return -EINVAL;
    }

    lineLen = strlen(logLine);
    if (g_smapLogFile.currentFileSize + lineLen > g_smapLogFile.maxFileSize) {
        if (RotateLogFile() != 0) {
            pthread_mutex_unlock(&g_smapLogFile.lock);
            return -EIO;
        }
    }

    if (fprintf(g_smapLogFile.fileHandle, logLine) != lineLen) {
        pthread_mutex_unlock(&g_smapLogFile.lock);
        return -EIO;
    }

    if (level >= SMAP_LOG_CORE_ERROR) {
        fflush(g_smapLogFile.fileHandle);
    }

    g_smapLogFile.currentFileSize += lineLen;

    pthread_mutex_unlock(&g_smapLogFile.lock);

    return 0;
}

int SmapLogCoreGetMinLogLevel(void)
{
    return g_minLogLevel;
}
