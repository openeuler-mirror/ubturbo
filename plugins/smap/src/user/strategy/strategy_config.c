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
#include "strategy_config.h"

#define STRATEGY_CONFIG_ENTRY 12
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

#define SCAN_MULTIPLE 1UL

#define RADIX_10 10UL

typedef struct {
    uint32_t scanPeriod;
    uint32_t migratePeriod;
    uint32_t remoteFreqPercentile;
    uint32_t slowThreshold;
    uint64_t freqWt;
    uint32_t remoteHotThreshold;
    uint32_t groupSwapRatio;
    uint32_t groupSwapMinRemoteFreq;
    uint32_t groupSwapMinFreqGain;
    bool zeroFreqMigrateEnable;
    bool adaptiveRatioEnable;
    bool fileConfSwitch;
    bool scanPeriodChanged;
    bool migratePeriodChanged;
} StrategyConfig;

static StrategyConfig g_tmpStrategyConfig;

static StrategyConfig g_strategyConfig;

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
    if (g_tmpStrategyConfig.scanPeriod % SCAN_MULTIPLE != 0) {
        SMAP_LOGGER_ERROR("Scan period(%d) must be a multiple of %d, key:%s.", g_tmpStrategyConfig.scanPeriod,
                          SCAN_MULTIPLE, substr);
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
        SMAP_LOGGER_ERROR("Config freq wt(%d) invalid, range(%d-%d), key:%s.", g_tmpStrategyConfig.freqWt, MIN_FREQ_WT,
                          MAX_FREQ_WT, substr);
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

static StrategyConfigReadElem g_strategyConfigRead[] = {
    {
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
};

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
            SMAP_LOGGER_INFO("Read config over.");
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
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_strategyConfig.fileConfSwitch = false;
    g_strategyConfig.scanPeriodChanged = false;
    g_strategyConfig.migratePeriodChanged = false;
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

static int32_t InitStrategyConfigFileBuffer(char strategyDefaultConfig[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE])
{
    int32_t ret;

    // 定义配置项的结构体数组
    struct ConfigEntry {
        const char *format;
        int value;
    } configs[] = {
        { "smap.scan.period = %d\n", DEFAULT_SCAN_PERIOD },
        { "smap.migrate.period = %d\n", DEFAULT_MIGRATE_PERIOD },
        { "smap.remote.freq.percentile = %d\n", DEFAULT_REMOTE_FREQ_PERCENTILE },
        { "smap.slow.threshold = %d\n", DEFAULT_SLOW_THRESHOLD },
        { "smap.freq.wt = %d\n", DEFAULT_FREQ_WT },
        { "smap.remote.hot.threshold = %d\n", DEFAULT_REMOTE_HOT_THRESHOLD },
        { "smap.group.swap.ratio = %d\n", DEFAULT_GROUP_SWAP_RATIO },
        { "smap.group.swap.min.remote.freq = %d\n", DEFAULT_GROUP_SWAP_MIN_REMOTE_FREQ },
        { "smap.group.swap.min.freq.gain = %d\n", DEFAULT_GROUP_SWAP_MIN_FREQ_GAIN },
    };
    size_t numConfigs = sizeof(configs) / sizeof(configs[0]);

    // 使用循环处理snprintf_s部分
    for (size_t i = 0; i < numConfigs; i++) {
        ret = snprintf_s(strategyDefaultConfig[i], STRATEGY_CONFIG_BUFFSIZE, STRATEGY_CONFIG_BUFFSIZE - 1,
                         configs[i].format, configs[i].value);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Snprintf failed for config index %zu.", i);
            return RETURN_ERROR;
        }
    }
    const char *zeroFreqMigrateEnableStr = "smap.zero.freq.migrate.enable = true\n";
    size_t zeroFreqStrLen = strlen(zeroFreqMigrateEnableStr);
    errno_t res = strncpy_s(strategyDefaultConfig[numConfigs], STRATEGY_CONFIG_BUFFSIZE, zeroFreqMigrateEnableStr,
                            zeroFreqStrLen);
    if (res != EOK) {
        SMAP_LOGGER_ERROR("Strncpy smap zero freq migrate enable failed.");
        return RETURN_ERROR;
    }
    const char *adaptiveRatioEnableStr = "smap.adaptive.ratio.enable = true\n";
    size_t adaptiveRatioStrLen = strlen(adaptiveRatioEnableStr);
    res = strncpy_s(strategyDefaultConfig[numConfigs + 1], STRATEGY_CONFIG_BUFFSIZE, adaptiveRatioEnableStr,
                    adaptiveRatioStrLen);
    if (res != EOK) {
        SMAP_LOGGER_ERROR("Strncpy smap adaptive ratio enable failed.");
        return RETURN_ERROR;
    }
    const char *switchConfigStr = "smap.period.file.config.switch = false\n";
    size_t configStrLen = strlen(switchConfigStr);
    res = strncpy_s(strategyDefaultConfig[numConfigs + 2], STRATEGY_CONFIG_BUFFSIZE, switchConfigStr, configStrLen);
    if (res != EOK) {
        SMAP_LOGGER_ERROR("Strncpy smap period switch failed.");
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

int32_t GenerateStrategyConfigFile(const char *configFile)
{
    InitStrategyConfig();
    char strategyDefaultConfig[STRATEGY_CONFIG_ENTRY][STRATEGY_CONFIG_BUFFSIZE];
    int32_t res = InitStrategyConfigFileBuffer(strategyDefaultConfig);
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

    int fd = open(configFile, (O_WRONLY | O_CREAT | O_TRUNC), (S_IRUSR | S_IWUSR | S_IRGRP));
    if (fd < 0) {
        SMAP_LOGGER_ERROR("Open file %s error: %s.", configFile, strerror(errno));
        return RETURN_ERROR;
    }

    size_t arraySize = sizeof(strategyDefaultConfig) / sizeof(strategyDefaultConfig[0]);
    for (size_t i = 0; i < arraySize; i++) {
        ret = dprintf(fd, strategyDefaultConfig[i]);
        if (ret < 0) {
            SMAP_LOGGER_ERROR("Dprintf file %s error: %s.", configFile, strerror(errno));
            (void)close(fd);
            return RETURN_ERROR;
        }
    }

    (void)close(fd);
    return RETURN_OK;
}

static bool UpdateStrategyConfigChanged(void)
{
    uint32_t oldScanPeriod, oldMigratePeriod, oldRemoteHotThreshold, scanPeriod, migratePeriod, remoteHotThreshold;
    uint32_t oldRemoteFreqPercentile, oldSlowThreshold, remoteFreqPercentile, slowThreshold;
    uint32_t oldGroupSwapRatio, oldGroupSwapMinRemoteFreq, oldGroupSwapMinFreqGain;
    uint32_t groupSwapRatio, groupSwapMinRemoteFreq, groupSwapMinFreqGain;
    uint64_t oldFreqWt, freqWt;
    bool oldZeroFreqMigrateEnable, zeroFreqMigrateEnable;
    bool oldAdaptiveRatioEnable, adaptiveRatioEnable;

    if (!g_tmpStrategyConfig.fileConfSwitch) {
        return false;
    }

    oldScanPeriod = g_strategyConfig.scanPeriod;
    oldMigratePeriod = g_strategyConfig.migratePeriod;
    oldRemoteFreqPercentile = g_strategyConfig.remoteFreqPercentile;
    oldSlowThreshold = g_strategyConfig.slowThreshold;
    oldFreqWt = g_strategyConfig.freqWt;
    oldRemoteHotThreshold = g_strategyConfig.remoteHotThreshold;
    oldGroupSwapRatio = g_strategyConfig.groupSwapRatio;
    oldGroupSwapMinRemoteFreq = g_strategyConfig.groupSwapMinRemoteFreq;
    oldGroupSwapMinFreqGain = g_strategyConfig.groupSwapMinFreqGain;
    oldZeroFreqMigrateEnable = g_strategyConfig.zeroFreqMigrateEnable;
    oldAdaptiveRatioEnable = g_strategyConfig.adaptiveRatioEnable;

    scanPeriod = g_tmpStrategyConfig.scanPeriod;
    migratePeriod = g_tmpStrategyConfig.migratePeriod;
    remoteFreqPercentile = g_tmpStrategyConfig.remoteFreqPercentile;
    slowThreshold = g_tmpStrategyConfig.slowThreshold;
    freqWt = g_tmpStrategyConfig.freqWt;
    remoteHotThreshold = g_tmpStrategyConfig.remoteHotThreshold;
    groupSwapRatio = g_tmpStrategyConfig.groupSwapRatio;
    groupSwapMinRemoteFreq = g_tmpStrategyConfig.groupSwapMinRemoteFreq;
    groupSwapMinFreqGain = g_tmpStrategyConfig.groupSwapMinFreqGain;
    zeroFreqMigrateEnable = g_tmpStrategyConfig.zeroFreqMigrateEnable;
    adaptiveRatioEnable = g_tmpStrategyConfig.adaptiveRatioEnable;

    if (oldScanPeriod == scanPeriod && oldMigratePeriod == migratePeriod &&
        oldRemoteHotThreshold == remoteHotThreshold && oldRemoteFreqPercentile == remoteFreqPercentile &&
        oldSlowThreshold == slowThreshold && oldFreqWt == freqWt && oldGroupSwapRatio == groupSwapRatio &&
        oldGroupSwapMinRemoteFreq == groupSwapMinRemoteFreq && oldGroupSwapMinFreqGain == groupSwapMinFreqGain &&
        oldZeroFreqMigrateEnable == zeroFreqMigrateEnable && oldAdaptiveRatioEnable == adaptiveRatioEnable) {
        return false;
    }

    if (oldScanPeriod != scanPeriod) {
        g_tmpStrategyConfig.scanPeriodChanged = true;
    }

    if (oldMigratePeriod != migratePeriod) {
        g_tmpStrategyConfig.migratePeriodChanged = true;
    }

    if (oldRemoteFreqPercentile != remoteFreqPercentile) {
        SMAP_LOGGER_INFO("Start update remote freq percentile from config to %u.", remoteFreqPercentile);
    }

    if (oldSlowThreshold != slowThreshold) {
        SMAP_LOGGER_INFO("Start update slow threshold from config to %u.", slowThreshold);
    }

    if (oldFreqWt != freqWt) {
        SMAP_LOGGER_INFO("Start update freq wt from config to %lu.", freqWt);
    }

    if (oldRemoteHotThreshold != remoteHotThreshold) {
        SMAP_LOGGER_INFO("Start update remote hot threshold from config to %lu.", remoteHotThreshold);
    }

    if (oldGroupSwapRatio != groupSwapRatio) {
        SMAP_LOGGER_INFO("Start update group swap ratio from config to %u.", groupSwapRatio);
    }

    if (oldGroupSwapMinRemoteFreq != groupSwapMinRemoteFreq) {
        SMAP_LOGGER_INFO("Start update group swap min remote freq from config to %u.", groupSwapMinRemoteFreq);
    }

    if (oldGroupSwapMinFreqGain != groupSwapMinFreqGain) {
        SMAP_LOGGER_INFO("Start update group swap min freq gain from config to %u.", groupSwapMinFreqGain);
    }

    if (oldZeroFreqMigrateEnable != zeroFreqMigrateEnable) {
        SMAP_LOGGER_INFO("Start update zero freq migrate enable from config to %s.",
                         zeroFreqMigrateEnable ? "true" : "false");
    }

    if (oldAdaptiveRatioEnable != adaptiveRatioEnable) {
        SMAP_LOGGER_INFO("Start update adaptive ratio enable from config to %s.",
                         adaptiveRatioEnable ? "true" : "false");
    }

    return true;
}

void StrategyConfigRead(const char *configFile)
{
    int ret = ConfigReadReadFile(configFile);
    if (ret != RETURN_OK) {
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Read period config failed, ret(%d).", ret);
        return;
    }

    ret = StrategyConfigReview();
    if (ret != RETURN_OK) {
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Review period config failed, ret(%d).", ret);
        return;
    }
    if (UpdateStrategyConfigChanged()) {
        g_strategyConfig = g_tmpStrategyConfig;
    }
    PeriodConifgReset();
    SMAP_LOGGER_DEBUG("Scan: %d, Migrate: %d, FileConfSwitch: %d.", g_strategyConfig.scanPeriod,
                      g_strategyConfig.migratePeriod, g_strategyConfig.fileConfSwitch);
    return;
}
