/*
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
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "smap_user_log.h"
#include "securec.h"
#include "manage/thread_pool.h"
#include "strategy_config.h"

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

#define MAX_MIGRATE_MODE 1 /* only LD_ST/URMA modes */
#define DEFAULT_MIGRATE_MODE 0
#define MIN_MIGRATE_MODE 0

#define MAX_UB_BW_THRESHOLD 65535
#define DEFAULT_UB_BW_THRESHOLD 0

#define MAX_REMOTE_SCAN_MODE 2
#define DEFAULT_REMOTE_SCAN_MODE 1 /* soft+hard */
#define MIN_REMOTE_SCAN_MODE 0

#define RADIX_10 10UL

#define SYS_CPU_POSSIBLE "/sys/devices/system/cpu/possible"

/* common config key prefix, for unknown-key detection */
#define CONFIG_KEY_PREFIX "smap."
#define CONFIG_KEY_PREFIX_LEN (sizeof(CONFIG_KEY_PREFIX) - 1)

/* config file path is fixed in this module, installed by RPM, callers pass nothing */
#define STRATEGY_CONFIG_PATH "/opt/ubturbo/conf/smap/smap_strategy.conf"

#define MAX_CONFIG_VAL_LEN 64

/* read valid cpu range from /sys/devices/system/cpu/possible */
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

    /* parse "min-max" format, e.g. "0-127" */
    uint32_t min = 0, max = 0;
    if (sscanf_s(buf, "%u-%u", &min, &max) != 2) {
        SMAP_LOGGER_ERROR("Parse %s failed, content:%s, use default cpu range 0-0.", SYS_CPU_POSSIBLE, buf);
        return;
    }

    *cpuMin = min;
    *cpuMax = max;
    SMAP_LOGGER_DEBUG("System cpu range: %u-%u.", *cpuMin, *cpuMax);
}

/* struct StrategyConfig is defined in strategy_config.h (single source) */

static StrategyConfig g_tmpStrategyConfig;

static StrategyConfig g_strategyConfig;

/* idempotency guard: InitStrategyConfig runs only once (first StrategyConfigRead),
 * so periodic reload never resets runtime config to defaults */
static bool g_strategyConfigInited = false;

typedef struct StrategyConfigReadElem {
    char *substr;
    bool seen; /* seen in current pass, for duplicate-key detection */
    int32_t (*func)(char *substr, char *value);
} StrategyConfigReadElem;

/* shared range check with unified logging */
static int32_t ValidateIntRange(uint64_t value, uint64_t min, uint64_t max, const char *substr)
{
    if (value < min || value > max) {
        SMAP_LOGGER_ERROR("Config value(%llu) invalid, range(%llu-%llu), key:%s.", (unsigned long long)value,
                          (unsigned long long)min, (unsigned long long)max, substr);
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

/* immediate warn on parse error: never blocks, bad key keeps current value */
static void WarnParseError(int lineNo, const char *fmt, ...)
{
    char msg[STRATEGY_CONFIG_BUFFSIZE];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf_s(msg, sizeof(msg), sizeof(msg) - 1, fmt, args);
    va_end(args);
    if (n < 0) {
        msg[0] = '\0'; /* on format failure keep line-number-only log, avoid uninitialized output */
    }
    SMAP_LOGGER_WARNING("Line %d: %s", lineNo, msg);
}

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

bool GetScanCpuConfigured(void)
{
    return g_strategyConfig.scanCpuConfigured;
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
    if (*pvalue == '\0') {
        return RETURN_ERROR;
    }
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

static void ConfigReadTrim(char *str);

static int32_t ConfigScanPeriod(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config scan period read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_SCAN_PERIOD, MAX_SCAN_PERIOD, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.scanPeriod = v;
    return RETURN_OK;
}

static int32_t ConfigMigratePeriod(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config migrate period read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_MIGRATE_PERIOD, MAX_MIGRATE_PERIOD, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.migratePeriod = v;
    return RETURN_OK;
}

static int32_t ConfigRemoteFreqPercentile(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config remote freq percentile read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_REMOTE_FREQ_PERCENTILE, MAX_REMOTE_FREQ_PERCENTILE, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.remoteFreqPercentile = v;
    return RETURN_OK;
}

static int32_t ConfigSlowThreshold(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config slow threshold read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_SLOW_THRESHOLD, MAX_SLOW_THRESHOLD, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.slowThreshold = v;
    return RETURN_OK;
}

static int32_t ConfigFreqWt(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t tempFreqWt;
    int32_t ret = ConfigReadValueToInt(value, &tempFreqWt);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config freq wt read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(tempFreqWt, MIN_FREQ_WT, MAX_FREQ_WT, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
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
    if (ValidateIntRange(remoteHotThreshold, MIN_REMOTE_HOT_THRESHOLD, MAX_REMOTE_HOT_THRESHOLD, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.remoteHotThreshold = remoteHotThreshold;
    return RETURN_OK;
}

static int32_t ConfigGroupSwapRatio(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap ratio read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_GROUP_SWAP_RATIO, MAX_GROUP_SWAP_RATIO, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.groupSwapRatio = v;
    return RETURN_OK;
}

static int32_t ConfigGroupSwapMinRemoteFreq(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap min remote freq read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_GROUP_SWAP_MIN_REMOTE_FREQ, MAX_GROUP_SWAP_MIN_REMOTE_FREQ, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.groupSwapMinRemoteFreq = v;
    return RETURN_OK;
}

static int32_t ConfigGroupSwapMinFreqGain(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap min freq gain read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_GROUP_SWAP_MIN_FREQ_GAIN, MAX_GROUP_SWAP_MIN_FREQ_GAIN, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.groupSwapMinFreqGain = v;
    return RETURN_OK;
}

static int32_t ConfigGroupSwapLocalWatermarkRatio(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config group swap local watermark ratio read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, MIN_GROUP_SWAP_LOCAL_WATERMARK_RATIO, MAX_GROUP_SWAP_LOCAL_WATERMARK_RATIO, substr) !=
        RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.groupSwapLocalWatermarkRatio = v;
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
    if (ValidateIntRange(migrateMode, MIN_MIGRATE_MODE, MAX_MIGRATE_MODE, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.migrateMode = migrateMode;
    return RETURN_OK;
}

static int32_t ConfigScanCpu(char *substr, char *value)
{
    SMAP_LOGGER_DEBUG("Read config key:%s, value:%s.", substr, value);
    /* empty value = unset: no kernel push, kernel default cpumask (all cpus) applies */
    if (value[0] == '\0') {
        g_tmpStrategyConfig.scanCpuConfigured = false;
        return RETURN_OK;
    }
    char *delim = strchr(value, '-');
    if (delim == NULL) {
        SMAP_LOGGER_ERROR("Config scan cpu(%s) invalid, need format min-max, key:%s.", value, substr);
        return RETURN_ERROR;
    }
    *delim = '\0';
    char *minStr = value;
    char *maxStr = delim + 1;
    /* tolerate "35 - 63": trim spaces then parse as plain digits */
    while (*minStr == ' ') {
        minStr++;
    }
    while (*maxStr == ' ') {
        maxStr++;
    }
    ConfigReadTrim(minStr);
    ConfigReadTrim(maxStr);

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
    /* aligned with kernel ioctl_set_scan_cpu: both min/max must be within system
     * possible cpu range, otherwise the kernel rejects with EINVAL and thread
     * affinity falls through (min<=max checked above, so min<sysMin and
     * max>sysMax cover the whole range) */
    uint32_t sysCpuMin, sysCpuMax;
    GetSystemCpuRange(&sysCpuMin, &sysCpuMax);
    if (scanCpuMin < sysCpuMin || scanCpuMax > sysCpuMax) {
        SMAP_LOGGER_ERROR("Config scan cpu range(%u-%u) out of system possible cpu range(%u-%u), key:%s.", scanCpuMin,
                          scanCpuMax, sysCpuMin, sysCpuMax, substr);
        return RETURN_ERROR;
    }
    g_tmpStrategyConfig.scanCpuMin = scanCpuMin;
    g_tmpStrategyConfig.scanCpuMax = scanCpuMax;
    g_tmpStrategyConfig.scanCpuConfigured = true;
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
    uint32_t v;
    int32_t ret = ConfigReadValueToInt(value, &v);
    if (ret != RETURN_OK) {
        SMAP_LOGGER_ERROR("Config ub bw threshold read failed, key:%s.", substr);
        return ret;
    }
    if (ValidateIntRange(v, 0U, MAX_UB_BW_THRESHOLD, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.ubBwThreshold = v;
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
    if (ValidateIntRange(remoteScanMode, MIN_REMOTE_SCAN_MODE, MAX_REMOTE_SCAN_MODE, substr) != RETURN_OK) {
        return RETURN_ERROR; /* bad key keeps current value, not written to tmp */
    }
    g_tmpStrategyConfig.remoteScanMode = remoteScanMode;
    return RETURN_OK;
}

static StrategyConfigReadElem g_strategyConfigRead[] = {
    { "smap.scan.period", false, ConfigScanPeriod },
    { "smap.migrate.period", false, ConfigMigratePeriod },
    { "smap.remote.freq.percentile", false, ConfigRemoteFreqPercentile },
    { "smap.slow.threshold", false, ConfigSlowThreshold },
    { "smap.freq.wt", false, ConfigFreqWt },
    { "smap.remote.hot.threshold", false, ConfigRemoteHotThreshold },
    { "smap.group.swap.ratio", false, ConfigGroupSwapRatio },
    { "smap.group.swap.min.remote.freq", false, ConfigGroupSwapMinRemoteFreq },
    { "smap.group.swap.min.freq.gain", false, ConfigGroupSwapMinFreqGain },
    { "smap.group.swap.local.watermark.ratio", false, ConfigGroupSwapLocalWatermarkRatio },
    { "smap.migrate.mode", false, ConfigMigrateMode },
    { "smap.zero.freq.migrate.enable", false, ConfigZeroFreqMigrateEnable },
    { "smap.adaptive.ratio.enable", false, ConfigAdaptiveRatioEnable },
    { "smap.period.file.config.switch", false, ConfigFileConfSwitch },
    { "smap.ub.bw.threshold", false, ConfigUbBwThreshold },
    { "smap.scan.cpu", false, ConfigScanCpu },
    { "smap.remote.scan.mode", false, ConfigRemoteScanMode },
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
    /* empty value is legal (e.g. smap.scan.cpu = means unset), parsers decide */
    if (!isMatch) {
        return NULL;
    }
    ConfigReadTrim(ret);
    /* value whitelist + length limit: alnum/./_/-/space only, blocks injection and
     * over-long values; space allowed for "35 - 63" style, parsers stay strict */
    size_t vlen = strlen(ret);
    if (vlen > MAX_CONFIG_VAL_LEN) {
        SMAP_LOGGER_WARNING("Config value too long(%zu), max %d, key-prefix:%s.", vlen, MAX_CONFIG_VAL_LEN, substr);
        return NULL;
    }
    for (size_t i = 0; i < vlen; i++) {
        char c = ret[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-' || c == ' ')) {
            SMAP_LOGGER_WARNING("Config value has illegal char(0x%02x), key-prefix:%s.", (unsigned char)c, substr);
            return NULL;
        }
    }
    return ret;
}

/* per-line parse: errors are warn-only (bad key keeps current value), no return */
static void StrategyConfigAnalyze(const char *str, int lineNo)
{
    /* skip comments (#/;) and blank lines so smap. inside comments is not a false unknown-key */
    const char *p = str;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#' || *p == ';') {
        return;
    }

    size_t len = sizeof(g_strategyConfigRead) / sizeof(StrategyConfigReadElem);
    for (size_t index = 0; index < len; index++) {
        char *config = g_strategyConfigRead[index].substr;
        char *substr = ConfigReadSearchSubString(str, config);
        if (substr != NULL) {
            /* duplicate key: warn only, keep first value */
            if (g_strategyConfigRead[index].seen) {
                WarnParseError(lineNo, "duplicate config key, keep first value: %s", config);
                return;
            }
            g_strategyConfigRead[index].seen = true;
            if (g_strategyConfigRead[index].func(config, substr) != RETURN_OK) {
                /* bad key keeps current value (default/previous), other keys apply as usual */
                WarnParseError(lineNo, "parse failed, keep current value, key=%s value=%s", config, substr);
            }
            return;
        }
    }
    /* unknown key: smap. prefixed but no table match, warn only */
    if (strncmp(p, CONFIG_KEY_PREFIX, CONFIG_KEY_PREFIX_LEN) == 0) {
        WarnParseError(lineNo, "unknown config key (ignored): %s", str);
    }
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

    /* keep reading after a bad line (bad key keeps current value), errors logged per line */
    int lineNo = 0;
    int ret;
    while (true) {
        ret = memset_s(buf, STRATEGY_CONFIG_BUFFSIZE, 0, STRATEGY_CONFIG_BUFFSIZE);
        if (ret != 0) {
            break;
        }
        ret = ConfigReadByLine(fp, buf, STRATEGY_CONFIG_BUFFSIZE);
        if (ret == STRATEGY_CONFIG_READ_OVER) {
            ret = RETURN_OK;
            break;
        }
        if (ret == RETURN_ERROR) {
            SMAP_LOGGER_ERROR("Read config line %d failed.", lineNo + 1);
            break;
        }
        lineNo++;
        StrategyConfigAnalyze(buf, lineNo);
    }

    (void)fclose(fp);
    return ret;
}

static int32_t ValidatePeriodConstraint(void)
{
    /* absent key falls back to default, no required-key check; cross-field constraint only */
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
        g_strategyConfigRead[index].seen = false;
    }
}

static void InitStrategyConfig(void)
{
    /* zero everything first (runtime flags default false), then assign per-member defaults */
    if (memset_s(&g_strategyConfig, sizeof(StrategyConfig), 0, sizeof(StrategyConfig)) != EOK) {
        SMAP_LOGGER_ERROR("memset_s strategy config failed.");
        return;
    }
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
    g_strategyConfig.zeroFreqMigrateEnable = true;
    g_strategyConfig.adaptiveRatioEnable = true;
    g_strategyConfig.fileConfSwitch = false;
    g_strategyConfig.ubBwThreshold = DEFAULT_UB_BW_THRESHOLD;
    g_strategyConfig.remoteScanMode = DEFAULT_REMOTE_SCAN_MODE;
    g_strategyConfig.scanCpuConfigured = false;
}

/* apply table defaults at load time so early consumers (SmapStartULog) read
 * defaults instead of zeros; no IO/kernel access, safe in constructor */
__attribute__((constructor)) static void StrategyConfigConstructor(void)
{
    if (!g_strategyConfigInited) {
        InitStrategyConfig();
        g_strategyConfigInited = true;
    }
}

/* log value changes without side effects, for field debugging */
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
    if (oldMigrateMode != g_tmpStrategyConfig.migrateMode) {
        g_strategyConfig.migrateMode = g_tmpStrategyConfig.migrateMode;
        /* tmp-side flag must be set too: the later whole-struct assign
         * g_strategyConfig = g_tmpStrategyConfig would roll it back to false otherwise */
        g_tmpStrategyConfig.migrateModeChanged = true;
        g_strategyConfig.migrateModeChanged = true;
        SMAP_LOGGER_INFO("Update migrate mode from %u to %u.", oldMigrateMode, g_tmpStrategyConfig.migrateMode);
    }

    g_strategyConfig.scanCpuConfigured = g_tmpStrategyConfig.scanCpuConfigured;
    if (g_strategyConfig.scanCpuMin != g_tmpStrategyConfig.scanCpuMin ||
        g_strategyConfig.scanCpuMax != g_tmpStrategyConfig.scanCpuMax) {
        g_strategyConfig.scanCpuMin = g_tmpStrategyConfig.scanCpuMin;
        g_strategyConfig.scanCpuMax = g_tmpStrategyConfig.scanCpuMax;
        g_strategyConfig.scanCpuChanged = true;
        g_tmpStrategyConfig.scanCpuChanged = true; /* keep flag through whole-struct assign, same as above */
        SMAP_LOGGER_INFO("Update scan cpu range %u-%u.", g_strategyConfig.scanCpuMin, g_strategyConfig.scanCpuMax);
    }
}

static void UpdateRemoteScanMode(void)
{
    if (g_strategyConfig.remoteScanMode != g_tmpStrategyConfig.remoteScanMode) {
        uint32_t oldRemoteScanMode = g_strategyConfig.remoteScanMode;
        g_strategyConfig.remoteScanMode = g_tmpStrategyConfig.remoteScanMode;
        g_strategyConfig.remoteScanModeChanged = true;
        g_tmpStrategyConfig.remoteScanModeChanged = true; /* keep flag through whole-struct assign, same as above */
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

void StrategyConfigRead(void)
{
    /* init in-memory defaults on first call (config file is RPM-installed, never
     * generated/rewritten at runtime); idempotent against periodic reload */
    if (!g_strategyConfigInited) {
        InitStrategyConfig();
        g_strategyConfigInited = true;
    }
    /* candidate starts from current effective values, then overwritten key by key
     * from file: stale leftovers from a failed previous parse never apply */
    g_tmpStrategyConfig = g_strategyConfig;
    int ret = ConfigReadReadFile(STRATEGY_CONFIG_PATH);
    if (ret != RETURN_OK) {
        /* missing file / IO error: warn and keep current values; file is user/RPM owned, never rewritten */
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Read period config failed, ret(%d).", ret);
        return;
    }

    ret = ValidatePeriodConstraint();
    if (ret != RETURN_OK) {
        /* cross-field check failed: skip applying this round, keep current values */
        PeriodConifgReset();
        SMAP_LOGGER_ERROR("Review period config failed, ret(%d).", ret);
        return;
    }

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
