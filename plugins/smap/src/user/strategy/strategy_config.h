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

#ifndef STRATEGY_CONFIG_H
#define STRATEGY_CONFIG_H

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

bool GetScanCpuEnableConfig(void);

bool GetScanCpuChanged(void);

void SetScanCpuChanged(bool val);

uint32_t GetGroupSwapRatioConfig(void);

uint32_t GetGroupSwapMinRemoteFreqConfig(void);

uint32_t GetGroupSwapMinFreqGainConfig(void);

uint32_t GetGroupSwapLocalWatermarkRatioConfig(void);

bool GetZeroFreqMigrateEnableConfig(void);

bool GetAdaptiveRatioEnableConfig(void);

bool GetFileConfSwitchConfig(void);

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
