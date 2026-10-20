/*
 * smap is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef STRATEGY_CONFIG_H
#define STRATEGY_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SCAN_PERIOD 60000
#define DEFAULT_SCAN_PERIOD 200
#define MIN_SCAN_PERIOD 50

#define MAX_MIGRATE_PERIOD 300000
#define DEFAULT_MIGRATE_PERIOD 2000
#define MIN_MIGRATE_PERIOD 500

#define MIGRATE_MODE_URMA 1
#define MIGRATE_MODE_LD_ST 0

/* 策略配置结构由实现和测试共用，避免布局漂移。 */
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
    uint32_t groupSwapLocalWatermarkRatio;
    uint32_t migrateMode;
    bool migrateModeChanged;
    bool migrateModeEnable;
    bool zeroFreqMigrateEnable;
    bool adaptiveRatioEnable;
    bool fileConfSwitch;
    bool scanPeriodChanged;
    bool migratePeriodChanged;
    bool scanCpuChanged;
    uint32_t scanCpuMin;
    uint32_t scanCpuMax;
    uint32_t ubBwThreshold;
    uint32_t remoteScanMode;
    bool remoteScanModeChanged;
} StrategyConfig;

uint32_t GetScanPeriodConfig(void);

uint32_t GetMigratePeriodConfig(void);

uint32_t GetRemoteFreqPercentileConfig(void);

uint32_t GetSlowThresholdConfig(void);

uint64_t GetFreqWtConfig(void);

uint32_t GetRemoteHotThreshold(void);

uint32_t GetMigrateModeConfig(void);

bool GetMigrateModeChanged(void);

void SetMigrateModeChanged(bool val);

bool GetMigrateModeEnableConfig(void);

uint32_t GetScanCpuMinConfig(void);

uint32_t GetScanCpuMaxConfig(void);

bool GetScanCpuChanged(void);

void SetScanCpuChanged(bool val);

uint32_t GetGroupSwapRatioConfig(void);

uint32_t GetGroupSwapMinRemoteFreqConfig(void);

uint32_t GetGroupSwapMinFreqGainConfig(void);

uint32_t GetGroupSwapLocalWatermarkRatioConfig(void);

bool GetZeroFreqMigrateEnableConfig(void);

bool GetAdaptiveRatioEnableConfig(void);

bool GetFileConfSwitchConfig(void);

/* smap.scan.migrate.mode 配置：true=event（事件驱动），false=poll（定时轮询） */
bool GetScanEventModeConfig(void);

/* smap.scan.pool.workers 配置：per-pid 任务线程池 worker 数 */
uint32_t GetScanPoolWorkersConfig(void);

uint32_t GetUbBwThresholdConfig(void);

uint32_t GetRemoteScanModeConfig(void);

bool GetRemoteScanModeChanged(void);

void SetRemoteScanModeChanged(bool val);

int32_t GenerateStrategyConfigFile(const char *configFile);

bool GetScanPeriodChanged(void);

bool GetMigratePeriodChanged(void);

void SetScanPeriodChanged(bool val);

void SetMigratePeriodChanged(bool val);

void StrategyConfigRead(const char *configFile);

#ifdef __cplusplus
}
#endif

#endif
