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
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "smap_user_log.h"
#include "securec.h"
#include "manage/thread_pool.h"
#include "strategy_config.h"

#define STRATEGY_CONFIG_ENTRY 19
#define STRATEGY_CONFIG_BUFFSIZE 500

#define RETURN_OK 0
#define RETURN_ERROR (-1)
#define STRATEGY_CONFIG_READ_OVER 100

#define MAX_SLOW_THRESHOLD (MAX_MIGRATE_PERIOD / MIN_SCAN_PERIOD)
#define DEFAULT_SLOW_THRESHOLD 2
#define MIN_SLOW_THRESHOLD 0

#define MAX_REMOTE_FREQ_PERCENTILE 100
#define DEFAULT_REMOTE_FREQ_PERCENTILE 99
#define MIN_REMOTE_FREQ_PERCENTILE 1

#define MAX_FREQ_WT 65535
#define DEFAULT_FREQ_WT 0
#define MIN_FREQ_WT 0

#define MAX_REMOTE_HOT_THRESHOLD 65535
#define DEFAULT_REMOTE_HOT_THRESHOLD 65535
#define MIN_REMOTE_HOT_THRESHOLD 0

#define MAX_GROUP_SWAP_RATIO 10
#define DEFAULT_GROUP_SWAP_RATIO 1
#define MIN_GROUP_SWAP_RATIO 0

#define MAX_GROUP_SWAP_MIN_REMOTE_FREQ 65535
#define DEFAULT_GROUP_SWAP_MIN_REMOTE_FREQ 0
#define MIN_GROUP_SWAP_MIN_REMOTE_FREQ 0

#define MAX_GROUP_SWAP_MIN_FREQ_GAIN 65535
#define DEFAULT_GROUP_SWAP_MIN_FREQ_GAIN 0
#define MIN_GROUP_SWAP_MIN_FREQ_GAIN 0

#define MAX_GROUP_SWAP_LOCAL_WATERMARK_RATIO 100
#define DEFAULT_GROUP_SWAP_LOCAL_WATERMARK_RATIO 95
#define MIN_GROUP_SWAP_LOCAL_WATERMARK_RATIO 0

#define MAX_MIGRATE_MODE 2
#define DEFAULT_MIGRATE_MODE 1
#define MIN_MIGRATE_MODE 0

#define DEFAULT_MIGRATE_MODE_ENABLE 0

#define MAX_UB_BW_THRESHOLD 65535
#define DEFAULT_UB_BW_THRESHOLD 0

#define MAX_REMOTE_SCAN_MODE 2
#define DEFAULT_REMOTE_SCAN_MODE 1 /* 软硬结合 */
#define MIN_REMOTE_SCAN_MODE 0

#define RADIX_10 10UL

#define SYS_CPU_POSSIBLE "/sys/devices/system/cpu/possible"

/* 从 /sys/devices/system/cpu/possible 读取有效的 CPU 范围 */
static void GetSystemCpuRange(uint32_t *cpuMin, uint32_t *cpuMax)
{
    *cpuMin = 0;
    *cpuMax = 0;

    FILE *fp = fopen(SYS_CPU_POSSIBLE, "r");
    if (fp == NULL) {
        SMAP_LOGGER_ERROR("Open %s failed, use default cpu range 0-0.", SYS_CPU_POSSIBLE);
        return;
    }

    char buf[64] = { 0 };
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        SMAP_LOGGER_ERROR("Read %s failed, use default cpu range 0-0.", SYS_CPU_POSSIBLE);
        (void)fclose(fp);
        return;
    }
    (void)fclose(fp);

    /* 解析 "min-max" 格式，例如 "0-127" */
    uint32_t min = 0, max = 0;
    if (sscanf_s(buf, "%u-%u", &min, &max) != 2) {
        SMAP_LOGGER_ERROR("Parse %s failed, content:%s, use default cpu range 0-0.", SYS_CPU_POSSIBLE, buf);
        return;
    }

    *cpuMin = min;
    *cpuMax = max;
    SMAP_LOGGER_DEBUG("System cpu range: %u-%u.", *cpuMin, *cpuMax);
}

/* struct StrategyConfig 定义见 strategy_config.h（单一来源，UT 共用） */

static StrategyConfig g_tmpStrategyConfig;

static StrategyConfig g_strategyConfig;

/* 最近一次解析并校验通过的配置，用于非法配置时恢复文件文本 */
static StrategyConfig g_lastValidStrategyConfig;

typedef int32_t (*StrategyConfigReadFunc)(char *substr, char *value);

typedef struct {
    char *substr;
    StrategyConfigReadFunc func;
    uint32_t needCfg;
    uint32_t realCfg;
} StrategyConfigReadElem;

uint32_t GetScanPeriodConfig(void)
{
    return g_strategyConfig.scanPeriod;
}

uint32_t GetMigratePeriodConfig(void)
{
    return g_strategyConfig.migratePeriod;
}

uint32_t GetRemoteFreqPercentileConfig(void)
{
    return g_strategyConfig.remoteFreqPercentile;
}

uint32_t GetSlowThresholdConfig(void)
{
    return g_strategyConfig.slowThreshold;
}

uint64_t GetFreqWtConfig(void)
{
    return g_strategyConfig.freqWt;
}

uint32_t GetRemoteHotThreshold(void)
{
    return g_strategyConfig.remoteHotThreshold;
}

uint32_t GetGroupSwapRatioConfig(void)
{
    return g_strategyConfig.groupSwapRatio;
}

uint32_t GetGroupSwapMinRemoteFreqConfig(void)
{
    return g_strategyConfig.groupSwapMinRemoteFreq;
}

uint32_t GetGroupSwapMinFreqGainConfig(void)
{
    return g_strategyConfig.groupSwapMinFreqGain;
}

uint32_t GetGroupSwapLocalWatermarkRatioConfig(void)
{
    return g_strategyConfig.groupSwapLocalWatermarkRatio;
}

uint32_t GetMigrateModeConfig(void)
{
    return g_strategyConfig.migrateMode;
}

bool GetMigrateModeChanged(void)
{
    return g_strategyConfig.migrateModeChanged;
}

void SetMigrateModeChanged(bool val)
{
    g_strategyConfig.migrateModeChanged = val;
}

bool GetMigrateModeEnableConfig(void)
{
    return g_strategyConfig.migrateModeEnable;
}

uint32_t GetScanCpuMinConfig(void)
{
    return g_strategyConfig.scanCpuMin;
}

uint32_t GetScanCpuMaxConfig(void)
{
    return g_strategyConfig.scanCpuMax;
}

bool GetScanCpuChanged(void)
{
    return g_strategyConfig.scanCpuChanged;
}

void SetScanCpuChanged(bool val)
{
    g_strategyConfig.scanCpuChanged = val;
}

bool GetZeroFreqMigrateEnableConfig(void)
{
    return g_strategyConfig.zeroFreqMigrateEnable;
}

bool GetAdaptiveRatioEnableConfig(void)
{
    return g_strategyConfig.adaptiveRatioEnable;
}

bool GetFileConfSwitchConfig(void)
{
    return g_strategyConfig.fileConfSwitch;
}

bool GetScanPeriodChanged(void)
{
    return g_strategyConfig.scanPeriodChanged;
}

bool GetMigratePeriodChanged(void)
{
    return g_strategyConfig.migratePeriodChanged;
}

void SetScanPeriodChanged(bool val)
{
    g_strategyConfig.scanPeriodChanged = val;
}

void SetMigratePeriodChanged(bool val)
{
    g_strategyConfig.migratePeriodChanged = val;
}

uint32_t GetUbBwThresholdConfig(void)
{
    return g_strategyConfig.ubBwThreshold;
}

uint32_t GetRemoteScanModeConfig(void)
{
    return g_strategyConfig.remoteScanMode;
}

bool GetRemoteScanModeChanged(void)
{
    return g_strategyConfig.remoteScanModeChanged;
}

void SetRemoteScanModeChanged(bool val)
{
    g_strategyConfig.remoteScanModeChanged = val;
}

static int32_t ConfigReadValueToInt(char *pvalue, uint32_t *resultvalue)
{
    uint64_t result = 0;
    while (*pvalue != '\0') {
        if (*pvalue >= '0' && *pvalue <= '9') {
            int digit = *pvalue - '0';
            result = RADIX_10 * result + digit;
            if (result > UINT32_MAX) {
                return RETURN_ERROR;
            }
            pvalue++;
        } else {
            return RETURN_ERROR;
        }
    }
    *resultvalue = result;
    return RETURN_OK;
}

static int32_t ConfigScanPeriod(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.scanPeriod);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config scan period read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.scanPeriod < MIN_SCAN_PERIOD || g_tmpStrategyConfig.scanPeriod > MAX_SCAN_PERIOD) {
        SMAP_LOGGER_ERROR("Config scan period(%d) invalid, range(%d-%d), key:%s.", g_tmpStrategyConfig.scanPeriod,
                          MIN_SCAN_PERIOD, MAX_SCAN_PERIOD, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigMigratePeriod(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.migratePeriod);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config migrate period read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.migratePeriod < MIN_MIGRATE_PERIOD ||
        g_tmpStrategyConfig.migratePeriod > MAX_MIGRATE_PERIOD) {
        SMAP_LOGGER_ERROR("Config migrate period(%d) invalid, range(%d-%d), key:%s.", g_tmpStrategyConfig.migratePeriod,
                          MIN_MIGRATE_PERIOD, MAX_MIGRATE_PERIOD, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigRemoteFreqPercentile(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.remoteFreqPercentile);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config remote freq percentile read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.remoteFreqPercentile < MIN_REMOTE_FREQ_PERCENTILE ||
        g_tmpStrategyConfig.remoteFreqPercentile > MAX_REMOTE_FREQ_PERCENTILE) {
        SMAP_LOGGER_ERROR("Config remote freq percentile(%d) invalid, range(%d-%d), key:%s.",
                          g_tmpStrategyConfig.remoteFreqPercentile, MIN_REMOTE_FREQ_PERCENTILE,
                          MAX_REMOTE_FREQ_PERCENTILE, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigSlowThreshold(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.slowThreshold);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config slow threshold read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.slowThreshold < MIN_SLOW_THRESHOLD ||
        g_tmpStrategyConfig.slowThreshold > MAX_SLOW_THRESHOLD) {
        SMAP_LOGGER_ERROR("Config slow threshold(%d) invalid, range(%d-%d), key:%s.", g_tmpStrategyConfig.slowThreshold,
                          MIN_SLOW_THRESHOLD, MAX_SLOW_THRESHOLD, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigFreqWt(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t tempFreqWt = 0;
    int32_t ret = ConfigReadValueToInt(value, &tempFreqWt);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config freq wt read failed, key:%s.", substr);
        return ret;
    }
    if (tempFreqWt < MIN_FREQ_WT || tempFreqWt > MAX_FREQ_WT) {
        SMAP_LOGGER_ERROR("Config freq wt(%d) invalid, range(%d-%d), key:%s.", tempFreqWt, MIN_FREQ_WT, MAX_FREQ_WT,
                          substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.freqWt = tempFreqWt;
    return RETURN_OK;
}

static int32_t ConfigRemoteHotThreshold(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t remoteHotThreshold;
    int32_t ret = ConfigReadValueToInt(value, &remoteHotThreshold);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config remote hot threshold read failed, key:%s.", substr);
        return ret;
    }
    if (remoteHotThreshold > MAX_REMOTE_HOT_THRESHOLD || remoteHotThreshold < MIN_REMOTE_FREQ_PERCENTILE) {
        SMAP_LOGGER_ERROR("Config remote hot threshold(%d) invalid, range(%d-%d), key:%s.", remoteHotThreshold,
                          MIN_REMOTE_FREQ_PERCENTILE, MAX_REMOTE_HOT_THRESHOLD, substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.remoteHotThreshold = remoteHotThreshold;
    return RETURN_OK;
}

static int32_t ConfigGroupSwapRatio(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.groupSwapRatio);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap ratio read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.groupSwapRatio < MIN_GROUP_SWAP_RATIO ||
        g_tmpStrategyConfig.groupSwapRatio > MAX_GROUP_SWAP_RATIO) {
        SMAP_LOGGER_ERROR("Config group swap ratio(%u) invalid, range(%d-%d), key:%s.",
                          g_tmpStrategyConfig.groupSwapRatio, MIN_GROUP_SWAP_RATIO, MAX_GROUP_SWAP_RATIO, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigGroupSwapMinRemoteFreq(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.groupSwapMinRemoteFreq);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap min remote freq read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.groupSwapMinRemoteFreq < MIN_GROUP_SWAP_MIN_REMOTE_FREQ ||
        g_tmpStrategyConfig.groupSwapMinRemoteFreq > MAX_GROUP_SWAP_MIN_REMOTE_FREQ) {
        SMAP_LOGGER_ERROR("Config group swap min remote freq(%u) invalid, range(%d-%d), key:%s.",
                          g_tmpStrategyConfig.groupSwapMinRemoteFreq, MIN_GROUP_SWAP_MIN_REMOTE_FREQ,
                          MAX_GROUP_SWAP_MIN_REMOTE_FREQ, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigGroupSwapMinFreqGain(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.groupSwapMinFreqGain);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap min freq gain read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.groupSwapMinFreqGain < MIN_GROUP_SWAP_MIN_FREQ_GAIN ||
        g_tmpStrategyConfig.groupSwapMinFreqGain > MAX_GROUP_SWAP_MIN_FREQ_GAIN) {
        SMAP_LOGGER_ERROR("Config group swap min freq gain(%u) invalid, range(%d-%d), key:%s.",
                          g_tmpStrategyConfig.groupSwapMinFreqGain, MIN_GROUP_SWAP_MIN_FREQ_GAIN,
                          MAX_GROUP_SWAP_MIN_FREQ_GAIN, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigGroupSwapLocalWatermarkRatio(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.groupSwapLocalWatermarkRatio);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap local watermark ratio read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.groupSwapLocalWatermarkRatio < MIN_GROUP_SWAP_LOCAL_WATERMARK_RATIO ||
        g_tmpStrategyConfig.groupSwapLocalWatermarkRatio > MAX_GROUP_SWAP_LOCAL_WATERMARK_RATIO) {
        SMAP_LOGGER_ERROR("Config group swap local watermark ratio(%u) invalid, range(%d-%d), key:%s.",
                          g_tmpStrategyConfig.groupSwapLocalWatermarkRatio, MIN_GROUP_SWAP_LOCAL_WATERMARK_RATIO,
                          MAX_GROUP_SWAP_LOCAL_WATERMARK_RATIO, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigMigrateMode(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t migrateMode;
    int32_t ret = ConfigReadValueToInt(value, &migrateMode);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config migrate mode read failed, key:%s.", substr);
        return ret;
    }
    if (migrateMode < MIN_MIGRATE_MODE || migrateMode > MAX_MIGRATE_MODE) {
        SMAP_LOGGER_ERROR("Config migrate mode(%u) invalid, range(%d-%d), key:%s.", migrateMode, MIN_MIGRATE_MODE,
                          MAX_MIGRATE_MODE, substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.migrateMode = migrateMode;
    return RETURN_OK;
}

static int32_t ConfigMigrateModeEnable(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    if (strcmp(value, "true") == 0) {
        g_tmpStrategyConfig.migrateModeEnable = true;
    } else if (strcmp(value, "false") == 0) {
        g_tmpStrategyConfig.migrateModeEnable = false;
    } else {
        SMAP_LOGGER_ERROR("Config migrate mode enable(%s) invalid, need true or false, key:%s.", value, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigScanCpu(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    char *delim = strchr(value, '-');
    if (delim == NULL) {
        SMAP_LOGGER_ERROR("Config scan cpu(%s) invalid, need format min-max, key:%s.", value, substr);
        return RETURN_ERROR;
    }
    *delim = '\0';
    char *minStr = value;
    char *maxStr = delim + 1;

    uint32_t scanCpuMin;
    if (ConfigReadValueToInt(minStr, &scanCpuMin) != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config scan cpu min parse failed, key:%s.", substr);
        return RETURN_ERROR;
    }
    uint32_t scanCpuMax;
    if (ConfigReadValueToInt(maxStr, &scanCpuMax) != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config scan cpu max parse failed, key:%s.", substr);
        return RETURN_ERROR;
    }
    if (scanCpuMin > scanCpuMax) {
        SMAP_LOGGER_ERROR("Config scan cpu min(%u) > max(%u), key:%s.", scanCpuMin, scanCpuMax, substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.scanCpuMin = scanCpuMin;
    g_tmpStrategyConfig.scanCpuMax = scanCpuMax;
    return RETURN_OK;
}

static int32_t ConfigZeroFreqMigrateEnable(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    if (strcmp(value, "true") == 0) {
        g_tmpStrategyConfig.zeroFreqMigrateEnable = true;
    } else if (strcmp(value, "false") == 0) {
        g_tmpStrategyConfig.zeroFreqMigrateEnable = false;
    } else {
        SMAP_LOGGER_ERROR("Config zero freq migrate enable(%s) failed, need config true or false, key:%s.", value,
                          substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigAdaptiveRatioEnable(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    if (strcmp(value, "true") == 0) {
        g_tmpStrategyConfig.adaptiveRatioEnable = true;
    } else if (strcmp(value, "false") == 0) {
        g_tmpStrategyConfig.adaptiveRatioEnable = false;
    } else {
        SMAP_LOGGER_ERROR("Config adaptive ratio enable(%s) failed, need config true or false, key:%s.", value, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigFileConfSwitch(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    if (strcmp(value, "true") == 0) {
        g_tmpStrategyConfig.fileConfSwitch = true;
    } else if (strcmp(value, "false") == 0) {
        g_tmpStrategyConfig.fileConfSwitch = false;
    } else {
        SMAP_LOGGER_ERROR("Config file conf switch(%s) failed, need config true or false, key:%s.", value, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigUbBwThreshold(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    int32_t ret = ConfigReadValueToInt(value, &g_tmpStrategyConfig.ubBwThreshold);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config ub bw threshold read failed, key:%s.", substr);
        return ret;
    }
    if (g_tmpStrategyConfig.ubBwThreshold > MAX_UB_BW_THRESHOLD) {
        SMAP_LOGGER_ERROR("Config ub bw threshold(%u) invalid, range(%u-%u), key:%s.",
                          g_tmpStrategyConfig.ubBwThreshold, 0, MAX_UB_BW_THRESHOLD, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int32_t ConfigRemoteScanMode(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t remoteScanMode;
    int32_t ret = ConfigReadValueToInt(value, &remoteScanMode);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config remote scan mode read failed, key:%s.", substr);
        return ret;
    }
    if (remoteScanMode < MIN_REMOTE_SCAN_MODE || remoteScanMode > MAX_REMOTE_SCAN_MODE) {
        SMAP_LOGGER_ERROR("Config remote scan mode(%u) invalid, range(%d-%d), key:%s.", remoteScanMode,
                          MIN_REMOTE_SCAN_MODE, MAX_REMOTE_SCAN_MODE, substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.remoteScanMode = remoteScanMode;
    return RETURN_OK;
}

static StrategyConfigReadElem g_strategyConfigRead[] = { {
                                                             "smap.scan.period",
                                                             ConfigScanPeriod,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.migrate.period",
                                                             ConfigMigratePeriod,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.remote.freq.percentile",
                                                             ConfigRemoteFreqPercentile,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.slow.threshold",
                                                             ConfigSlowThreshold,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.freq.wt",
                                                             ConfigFreqWt,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.remote.hot.threshold",
                                                             ConfigRemoteHotThreshold,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.group.swap.ratio",
                                                             ConfigGroupSwapRatio,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.group.swap.min.remote.freq",
                                                             ConfigGroupSwapMinRemoteFreq,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.group.swap.min.freq.gain",
                                                             ConfigGroupSwapMinFreqGain,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.group.swap.local.watermark.ratio",
                                                             ConfigGroupSwapLocalWatermarkRatio,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.migrate.mode",
                                                             ConfigMigrateMode,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.migrate.mode.enable",
                                                             ConfigMigrateModeEnable,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.zero.freq.migrate.enable",
                                                             ConfigZeroFreqMigrateEnable,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.adaptive.ratio.enable",
                                                             ConfigAdaptiveRatioEnable,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.period.file.config.switch",
                                                             ConfigFileConfSwitch,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.ub.bw.threshold",
                                                             ConfigUbBwThreshold,
                                                             0UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.scan.cpu",
                                                             ConfigScanCpu,
                                                             1UL,
                                                             0UL,
                                                         },
                                                         {
                                                             "smap.remote.scan.mode",
                                                             ConfigRemoteScanMode,
                                                             1UL,
                                                             0UL,
                                                         } };

static void ConfigReadTrim(char *str)
{
    size_t len = strlen(str);

    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

static char *ConfigReadSearchSubString(const char *buf, const char *substr)
{
    char *ret = NULL;
    bool isMatch = false;

    if (!(buf && substr)) {
        return NULL;
    }

    ret = strstr((char *)buf, substr);
    if (ret != buf) {
        return NULL;
    }

    ret += strlen(substr);
    while (*ret == ' ' || *ret == '=' || *ret == '\n' || *ret == '\r') {
        isMatch = true;
        ret++;
    }
    if (!isMatch || strlen(ret) == 0) {
        return NULL;
    }
    ConfigReadTrim(ret);
    return ret;
}

static int StrategyConfigAnalyze(const char *str)
{
    size_t len = sizeof(g_strategyConfigRead) / sizeof(StrategyConfigReadElem);

    for (size_t index = 0; index < len; index++) {
        char *config = g_strategyConfigRead[index].substr;
        char *substr = ConfigReadSearchSubString(str, config);
        if (substr != NULL) {
            g_strategyConfigRead[index].realCfg = 1;
            return g_strategyConfigRead[index].func(config, substr);
        }
    }
    return 0;
}

static int32_t ConfigReadByLine(FILE *fp, char *buf, int32_t n)
{
    char *ret = (char *)fgets(buf, n, fp);

    if (ret) {
        return RETURN_OK;
    }
    if (ferror(fp)) {
        return RETURN_ERROR;
    }
    return STRATEGY_CONFIG_READ_OVER;
}

static int ConfigReadReadFile(const char *filepath)
{
    FILE *fp = NULL;
    char buf[STRATEGY_CONFIG_BUFFSIZE] = { 0 };
    int ret = RETURN_OK;
    char path[PATH_MAX + 1] = { 0 };

    if (strlen(filepath) > PATH_MAX || realpath(filepath, path) == NULL) {
        SMAP_LOGGER_ERROR("Read config path failed.");
        return RETURN_ERROR;
    }
    fp = fopen(path, "r");
    if (!fp) {
        SMAP_LOGGER_ERROR("Failed to open config file: %s.", path);
        return RETURN_ERROR;
    }

    while (true) {
        ret = memset_s(buf, STRATEGY_CONFIG_BUFFSIZE, 0, STRATEGY_CONFIG_BUFFSIZE);
        if (ret != 0) {
            break;
        }
        ret = ConfigReadByLine(fp, buf, STRATEGY_CONFIG_BUFFSIZE);
        if (ret == STRATEGY_CONFIG_READ_OVER) {
            ret = RETURN_OK;
            break;
        } else if (ret == RETURN_ERROR) {
            SMAP_LOGGER_ERROR("Read config failed.");
            break;
        }
        ret = StrategyConfigAnalyze(buf);
        if (ret != 0) {
            SMAP_LOGGER_ERROR("Analyze config failed(%s), str(%s).", filepath, (char *)buf);
            break;
        }
    }

    (void)fclose(fp);
    return ret;
}

static int32_t StrategyConfigReview(void)
{
    size_t num = sizeof(g_strategyConfigRead) / sizeof(StrategyConfigReadElem);
    size_t index;

    for (index = 0; index < num; index++) {
        if (g_strategyConfigRead[index].needCfg == 1UL && g_strategyConfigRead[index].realCfg == 0UL) {
            SMAP_LOGGER_ERROR("Read config key:%s, failed.", g_strategyConfigRead[index].substr);
            return RETURN_ERROR;
        }
    }
    if (g_tmpStrategyConfig.scanPeriod > g_tmpStrategyConfig.migratePeriod) {
        SMAP_LOGGER_ERROR("Scan period(%d) must be less than migrate period(%d).", g_tmpStrategyConfig.scanPeriod,
                          g_tmpStrategyConfig.migratePeriod);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static void PeriodConifgReset(void)
{
    size_t num = sizeof(g_strategyConfigRead) / sizeof(StrategyConfigReadElem);
    size_t index;

    for (index = 0; index < num; index++) {
        g_strategyConfigRead[index].realCfg = 0;
    }
}

static void InitStrategyConfig(void)
{
    g_strategyConfig.scanPeriod = DEFAULT_SCAN_PERIOD;
    g_strategyConfig.migratePeriod = DEFAULT_MIGRATE_PERIOD;
    g_strategyConfig.remoteFreqPercentile = DEFAULT_REMOTE_FREQ_PERCENTILE;
    g_strategyConfig.slowThreshold = DEFAULT_SLOW_THRESHOLD;
    g_strategyConfig.freqWt = DEFAULT_FREQ_WT;
    g_strategyConfig.remoteHotThreshold = DEFAULT_REMOTE_HOT_THRESHOLD;
    g_strategyConfig.groupSwapRatio = DEFAULT_GROUP_SWAP_RATIO;
    g_strategyConfig.groupSwapMinRemoteFreq = DEFAULT_GROUP_SWAP_MIN_REMOTE_FREQ;
    g_strategyConfig.groupSwapMinFreqGain = DEFAULT_GROUP_SWAP_MIN_FREQ_GAIN;
    g_strategyConfig.groupSwapLocalWatermarkRatio = DEFAULT_GROUP_SWAP_LOCAL_WATERMARK_RATIO;
    g_strategyConfig.migrateMode = DEFAULT_MIGRATE_MODE;
    g_strategyConfig.migrateModeChanged = false;
    g_strategyConfig.migrateModeEnable = false;
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_strategyConfig.fileConfSwitch = false;
    g_strategyConfig.scanPeriodChanged = false;
    g_strategyConfig.migratePeriodChanged = false;
    uint32_t sysCpuMin, sysCpuMax;
    GetSystemCpuRange(&sysCpuMin, &sysCpuMax);
    g_strategyConfig.scanCpuMin = sysCpuMin;
    g_strategyConfig.scanCpuMax = sysCpuMax;
    g_strategyConfig.scanCpuChanged = false;
    g_strategyConfig.ubBwThreshold = DEFAULT_UB_BW_THRESHOLD;
    g_strategyConfig.remoteScanMode = DEFAULT_REMOTE_SCAN_MODE;
    g_strategyConfig.remoteScanModeChanged = false;
    g_lastValidStrategyConfig = g_strategyConfig;
}

static int32_t EnsureDirectoryExists(const char *dirPath)
{
    struct stat st;
    if (stat(dirPath, &st) == 0) {
        // 目录已存在
        if (S_ISDIR(st.st_mode)) {
            return RETURN_OK; // 是一个目录，返回成功
        } else {
            SMAP_LOGGER_ERROR("Error: %s exists but is not a directory.", dirPath);
            return RETURN_ERROR; // 路径存在但不是目录
        }
    }

    if (mkdir(dirPath, 0755) != 0) { // 0755 是权限设置
        SMAP_LOGGER_ERROR("Error creating directory.");
        return RETURN_ERROR;
    }

    return RETURN_OK;
}

/* 按传入配置生成完整的配置文件文本，用于首次生成与非法配置时的文本恢复 */
static int32_t BuildStrategyConfigText(const StrategyConfig *config,
                                       char strategyConfigText[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE])
{
    struct ConfigNumEntry {
        const char *format;
        uint32_t value;
    } numEntryDefs[] = { { "smap.scan.period = %u\n", config->scanPeriod },
                         { "smap.migrate.period = %u\n", config->migratePeriod },
                         { "smap.remote.freq.percentile = %u\n", config->remoteFreqPercentile },
                         { "smap.slow.threshold = %u\n", config->slowThreshold },
                         { "smap.freq.wt = %u\n", (uint32_t)config->freqWt },
                         { "smap.remote.hot.threshold = %u\n", config->remoteHotThreshold },
                         { "smap.group.swap.ratio = %u\n", config->groupSwapRatio },
                         { "smap.group.swap.min.remote.freq = %u\n", config->groupSwapMinRemoteFreq },
                         { "smap.group.swap.min.freq.gain = %u\n", config->groupSwapMinFreqGain },
                         { "smap.group.swap.local.watermark.ratio = %u\n", config->groupSwapLocalWatermarkRatio },
                         { "smap.ub.bw.threshold = %u\n", config->ubBwThreshold } };
    size_t numEntries = sizeof(numEntryDefs) / sizeof(numEntryDefs[0]);
    size_t idx = 0;
    int32_t ret;

    for (size_t i = 0; i < numEntries; i++) {
        ret = snprintf_s(strategyConfigText[idx], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                         numEntryDefs[i].format, numEntryDefs[i].value);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Format strategy config text failed, index:%zu.", i);
            return RETURN_ERROR;
        }
        idx++;
    }

    struct ConfigBoolEntry {
        const char *format;
        bool value;
    } boolEntries[] = { { "smap.zero.freq.migrate.enable = %s\n", config->zeroFreqMigrateEnable },
                        { "smap.adaptive.ratio.enable = %s\n", config->adaptiveRatioEnable },
                        { "smap.period.file.config.switch = %s\n", config->fileConfSwitch },
                        { "smap.migrate.mode.enable = %s\n", config->migrateModeEnable } };
    size_t numBoolEntries = sizeof(boolEntries) / sizeof(boolEntries[0]);

    for (size_t i = 0; i < numBoolEntries; i++) {
        ret = snprintf_s(strategyConfigText[idx], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                         boolEntries[i].format, boolEntries[i].value ? "true" : "false");
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Format strategy config text failed, index:%zu.", i);
            return RETURN_ERROR;
        }
        idx++;
    }

    ret = snprintf_s(strategyConfigText[idx++], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                     "smap.migrate.mode = %u\n", config->migrateMode);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Format smap migrate mode failed.");
        return RETURN_ERROR;
    }
    ret = snprintf_s(strategyConfigText[idx++], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                     "smap.scan.cpu = %u-%u\n", config->scanCpuMin, config->scanCpuMax);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Format smap scan cpu range failed.");
        return RETURN_ERROR;
    }
    ret = snprintf_s(strategyConfigText[idx], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                     "smap.remote.scan.mode = %u\n", config->remoteScanMode);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Format smap remote scan mode failed.");
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

/* 先写临时文件再重命名，避免读取到写了一半的配置文件 */
static int32_t WriteStrategyConfigText(const char *configFile,
                                       char strategyConfigText[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE])
{
    char tmpPath[PATH_MAX + 1] = { 0 };
    int32_t ret = snprintf_s(tmpPath, sizeof(tmpPath), sizeof(tmpPath) - 1, "%s.tmp", configFile);
    if (ret < 0) {
        SMAP_LOGGER_ERROR("Format tmp config path failed.");
        return RETURN_ERROR;
    }

    int fd = open(tmpPath, (O_WRONLY | O_CREAT | O_TRUNC), (S_IRUSR | S_IWUSR | S_IRGRP));
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open file %s error: %s.", tmpPath, strerror(errno));
        return RETURN_ERROR;
    }

    for (size_t i = 0; i < STRATEGY_CONFIG_ENTRY; i++) {
        if (strategyConfigText[i][0] == '\0') {
            continue;
        }
        if (dprintf(fd, strategyConfigText[i]) < 0) {
            SMAP_LOGGER_ERROR("Write file %s error: %s.", tmpPath, strerror(errno));
            (void)close(fd);
            (void)unlink(tmpPath);
            return RETURN_ERROR;
        }
    }

    (void)close(fd);
    if (rename(tmpPath, configFile) != 0) {
        SMAP_LOGGER_ERROR("Rename %s to %s error: %s.", tmpPath, configFile, strerror(errno));
        (void)unlink(tmpPath);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

/* 配置非法时用最近一次有效配置恢复文件文本，保证磁盘上始终是可用的配置 */
static void RestoreStrategyConfigFile(const char *configFile)
{
    char strategyConfigText[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE] = { 0 };
    if (BuildStrategyConfigText(&g_lastValidStrategyConfig, strategyConfigText) != RETURN_OK) {
        SMAP_LOGGER_ERROR("Build strategy config text failed, skip restore.");
        return;
    }
    if (WriteStrategyConfigText(configFile, strategyConfigText) != RETURN_OK) {
        SMAP_LOGGER_ERROR("Restore strategy config file %s failed.", configFile);
        return;
    }
    SMAP_LOGGER_WARNING("Invalid period config detected, file %s restored to last valid values.", configFile);
}

int32_t GenerateStrategyConfigFile(const char *configFile)
{
    InitStrategyConfig();
    char strategyDefaultConfig[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE] = { 0 };
    int32_t res = BuildStrategyConfigText(&g_strategyConfig, strategyDefaultConfig);
    if (res != RETURN_OK) {
        return res;
    }

    if (strlen(configFile) > PATH_MAX) {
        SMAP_LOGGER_ERROR("Config period file path too long.");
        return RETURN_ERROR;
    }

    // 提取目录路径
    char dirPath[PATH_MAX + 1] = { 0 };
    errno_t ret = strncpy_s(dirPath, sizeof(dirPath), configFile, strlen(configFile));
    if (ret != EOK) {
        SMAP_LOGGER_ERROR("strncpy_s failed, ret(%d).", ret);
        return RETURN_ERROR;
    }
    char *lastSlash = strrchr(dirPath, '/');
    if (lastSlash == NULL) {
        SMAP_LOGGER_ERROR("Invalid config file path.");
        return RETURN_ERROR;
    }
    *lastSlash = '\0';

    if (EnsureDirectoryExists(dirPath) != RETURN_OK) { // 确保目录存在
        SMAP_LOGGER_ERROR("Failed to ensure directory exists: %s.", dirPath);
        return RETURN_ERROR;
    }

    if (access(configFile, F_OK) == 0) {
        return RETURN_OK;
    }

    return WriteStrategyConfigText(configFile, strategyDefaultConfig);
}

/* 记录不需要主动下发副作用的字段变化，便于现场排查配置生效情况 */
static void LogStrategyConfigValueChanges(const StrategyConfig *old)
{
    if (old->remoteFreqPercentile != g_tmpStrategyConfig.remoteFreqPercentile) {
        SMAP_LOGGER_INFO("Start update remote freq percentile from config to %u.",
                         g_tmpStrategyConfig.remoteFreqPercentile);
    }
    if (old->slowThreshold != g_tmpStrategyConfig.slowThreshold) {
        SMAP_LOGGER_INFO("Start update slow threshold from config to %u.", g_tmpStrategyConfig.slowThreshold);
    }
    if (old->freqWt != g_tmpStrategyConfig.freqWt) {
        SMAP_LOGGER_INFO("Start update freq wt from config to %lu.", g_tmpStrategyConfig.freqWt);
    }
    if (old->remoteHotThreshold != g_tmpStrategyConfig.remoteHotThreshold) {
        SMAP_LOGGER_INFO("Start update remote hot threshold from config to %lu.",
                         g_tmpStrategyConfig.remoteHotThreshold);
    }
    if (old->groupSwapRatio != g_tmpStrategyConfig.groupSwapRatio) {
        SMAP_LOGGER_INFO("Start update group swap ratio from config to %u.", g_tmpStrategyConfig.groupSwapRatio);
    }
    if (old->groupSwapMinRemoteFreq != g_tmpStrategyConfig.groupSwapMinRemoteFreq) {
        SMAP_LOGGER_INFO("Start update group swap min remote freq from config to %u.",
                         g_tmpStrategyConfig.groupSwapMinRemoteFreq);
    }
    if (old->groupSwapMinFreqGain != g_tmpStrategyConfig.groupSwapMinFreqGain) {
        SMAP_LOGGER_INFO("Start update group swap min freq gain from config to %u.",
                         g_tmpStrategyConfig.groupSwapMinFreqGain);
    }
    if (old->groupSwapLocalWatermarkRatio != g_tmpStrategyConfig.groupSwapLocalWatermarkRatio) {
        SMAP_LOGGER_INFO("Start update group swap local watermark ratio from config to %u.",
                         g_tmpStrategyConfig.groupSwapLocalWatermarkRatio);
    }
    if (old->zeroFreqMigrateEnable != g_tmpStrategyConfig.zeroFreqMigrateEnable) {
        SMAP_LOGGER_INFO("Start update zero freq migrate enable from config to %s.",
                         g_tmpStrategyConfig.zeroFreqMigrateEnable ? "true" : "false");
    }
    if (old->adaptiveRatioEnable != g_tmpStrategyConfig.adaptiveRatioEnable) {
        SMAP_LOGGER_INFO("Start update adaptive ratio enable from config to %s.",
                         g_tmpStrategyConfig.adaptiveRatioEnable ? "true" : "false");
    }
    if (old->ubBwThreshold != g_tmpStrategyConfig.ubBwThreshold) {
        SMAP_LOGGER_INFO("Start update ub bandwidth threshold from config to %u.", g_tmpStrategyConfig.ubBwThreshold);
    }
}

static void UpdateMigrateModeAndScanCpu(void)
{
    uint32_t oldMigrateMode = g_strategyConfig.migrateMode;
    g_strategyConfig.migrateModeEnable = g_tmpStrategyConfig.migrateModeEnable;

    if (g_strategyConfig.migrateModeEnable) {
        if (oldMigrateMode != g_tmpStrategyConfig.migrateMode) {
            g_strategyConfig.migrateMode = g_tmpStrategyConfig.migrateMode;
            g_tmpStrategyConfig.migrateModeChanged = true;
            g_strategyConfig.migrateModeChanged = true;
            SMAP_LOGGER_INFO("Update migrate mode from %u to %u.", oldMigrateMode, g_tmpStrategyConfig.migrateMode);
        }
    }

    if (g_strategyConfig.scanCpuMin != g_tmpStrategyConfig.scanCpuMin ||
        g_strategyConfig.scanCpuMax != g_tmpStrategyConfig.scanCpuMax) {
        g_strategyConfig.scanCpuMin = g_tmpStrategyConfig.scanCpuMin;
        g_strategyConfig.scanCpuMax = g_tmpStrategyConfig.scanCpuMax;
        g_strategyConfig.scanCpuChanged = true;
        g_tmpStrategyConfig.scanCpuChanged = true;
        SMAP_LOGGER_INFO("Update scan cpu range %u-%u.", g_strategyConfig.scanCpuMin, g_strategyConfig.scanCpuMax);
    }
}

static void UpdateRemoteScanMode(void)
{
    if (g_strategyConfig.remoteScanMode != g_tmpStrategyConfig.remoteScanMode) {
        uint32_t oldRemoteScanMode = g_strategyConfig.remoteScanMode;
        g_strategyConfig.remoteScanMode = g_tmpStrategyConfig.remoteScanMode;
        g_strategyConfig.remoteScanModeChanged = true;
        g_tmpStrategyConfig.remoteScanModeChanged = true;
        SMAP_LOGGER_INFO("Update remote scan mode from %u to %u.", oldRemoteScanMode, g_strategyConfig.remoteScanMode);
    }
}

static bool UpdateStrategyConfigChanged(void)
{
    if (!g_tmpStrategyConfig.fileConfSwitch) {
        g_strategyConfig.fileConfSwitch = false;
        return false;
    }
    g_strategyConfig.fileConfSwitch = true;

    StrategyConfig old = g_strategyConfig;
    bool changed = (old.scanPeriod != g_tmpStrategyConfig.scanPeriod) ||
                   (old.migratePeriod != g_tmpStrategyConfig.migratePeriod) ||
                   (old.remoteFreqPercentile != g_tmpStrategyConfig.remoteFreqPercentile) ||
                   (old.slowThreshold != g_tmpStrategyConfig.slowThreshold) ||
                   (old.freqWt != g_tmpStrategyConfig.freqWt) ||
                   (old.remoteHotThreshold != g_tmpStrategyConfig.remoteHotThreshold) ||
                   (old.groupSwapRatio != g_tmpStrategyConfig.groupSwapRatio) ||
                   (old.groupSwapMinRemoteFreq != g_tmpStrategyConfig.groupSwapMinRemoteFreq) ||
                   (old.groupSwapMinFreqGain != g_tmpStrategyConfig.groupSwapMinFreqGain) ||
                   (old.groupSwapLocalWatermarkRatio != g_tmpStrategyConfig.groupSwapLocalWatermarkRatio) ||
                   (old.zeroFreqMigrateEnable != g_tmpStrategyConfig.zeroFreqMigrateEnable) ||
                   (old.adaptiveRatioEnable != g_tmpStrategyConfig.adaptiveRatioEnable) ||
                   (old.ubBwThreshold != g_tmpStrategyConfig.ubBwThreshold);
    if (!changed) {
        return false;
    }

    if (old.scanPeriod != g_tmpStrategyConfig.scanPeriod) {
        g_tmpStrategyConfig.scanPeriodChanged = true;
    }
    if (old.migratePeriod != g_tmpStrategyConfig.migratePeriod) {
        g_tmpStrategyConfig.migratePeriodChanged = true;
    }
    LogStrategyConfigValueChanges(&old);
    return true;
}

void StrategyConfigRead(const char *configFile)
{
    /* 候选值以当前生效值为基线，再被文件内容逐键覆盖：
     * 避免上轮解析残留（尤其是先写后校验失败留下的非法值）被当作本轮配置 */
    g_tmpStrategyConfig = g_strategyConfig;
    int ret = ConfigReadReadFile(configFile);
    if (ret != RETURN_OK) {
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Read period config failed, ret(%d).", ret);
        RestoreStrategyConfigFile(configFile);
        return;
    }

    ret = StrategyConfigReview();
    if (ret != RETURN_OK) {
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Review period config failed, ret(%d).", ret);
        RestoreStrategyConfigFile(configFile);
        return;
    }

    g_lastValidStrategyConfig = g_tmpStrategyConfig;
    UpdateMigrateModeAndScanCpu();
    UpdateRemoteScanMode();
    if (UpdateStrategyConfigChanged()) {
        g_strategyConfig = g_tmpStrategyConfig;
    }
    PeriodConifgReset();
    SMAP_LOGGER_DEBUG("Scan: %d, Migrate: %d, FileConfSwitch: %d.", g_strategyConfig.scanPeriod,
                      g_strategyConfig.migratePeriod, g_strategyConfig.fileConfSwitch);
    return;
}
