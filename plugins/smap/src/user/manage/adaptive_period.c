/*
 * smap is licensed under the Mulan PSL v2.
 * See http://license.coscl.org.cn/MulanPSL2 for the license text.
 */
#include <math.h>
#include "manage_internal.h"
#include "strategy/strategy_config.h"
#include "smap_user_log.h"

#define ADAPTIVE_STABLE_DELTA 0.05
#define ADAPTIVE_SURGE_DELTA 0.15
#define RATIO_EPSILON 1e-12

void ResetPidAdaptivePeriod(ProcessAttr *attr)
{
    attr->adaptivePeriod.previousValid = false;
    attr->adaptivePeriod.slow = false;
    attr->periodFeedbackReady = false;
    if (attr->scanType == NORMAL_SCAN) {
        attr->scanTime = DEFAULT_SCAN_PERIOD;
        attr->migPeriod = DEFAULT_MIGRATE_PERIOD;
        if (GetAdaptivePeriodEnableConfig()) {
            GetAdaptivePeriods(false, &attr->scanTime, &attr->migPeriod);
        }
    }
}

void InitPidAdaptivePeriod(ProcessAttr *attr)
{
    attr->adaptivePeriod = (AdaptivePeriod){ 0 };
    ResetPidAdaptivePeriod(attr);
    GetProcessPeriods(attr, &attr->adaptivePeriod.appliedScan, &attr->adaptivePeriod.appliedMigrate);
    attr->adaptivePeriod.appliedAdaptive = GetAdaptivePeriodEnableConfig();
}

bool PreparePidAdaptivePeriod(ProcessAttr *attr)
{
    bool enabled = GetAdaptivePeriodEnableConfig();
    if (attr->adaptivePeriod.appliedAdaptive != enabled) {
        ResetPidAdaptivePeriod(attr);
    } else if (enabled && attr->scanType == NORMAL_SCAN) {
        GetAdaptivePeriods(attr->adaptivePeriod.slow, &attr->scanTime, &attr->migPeriod);
    }
    return enabled;
}

void RecordPidAppliedPeriods(ProcessAttr *attr, uint32_t scan, uint32_t migrate, bool adaptive)
{
    AdaptivePeriod *period = &attr->adaptivePeriod;
    if (period->appliedScan != scan || period->appliedMigrate != migrate || period->appliedAdaptive != adaptive) {
        period->previousValid = false;
    }
    period->appliedScan = scan;
    period->appliedMigrate = migrate;
    period->appliedAdaptive = adaptive;
}

void ObservePidAdaptivePeriod(ProcessAttr *attr)
{
    AdaptivePeriod *period = &attr->adaptivePeriod;
    PeriodFeedback *feedback = &attr->periodFeedback;
    bool ready = attr->periodFeedbackReady;
    attr->periodFeedbackReady = false;
    if (attr->scanType != NORMAL_SCAN || !GetAdaptivePeriodEnableConfig() || !period->appliedAdaptive || !ready ||
        period->appliedScan != attr->scanTime || period->appliedMigrate != attr->migPeriod) {
        period->previousValid = false;
        return;
    }
    uint64_t hotPages = feedback->localHotPages + feedback->remoteHotPages;
    uint64_t frequency = feedback->localFreq + feedback->remoteFreq;
    if (hotPages == 0 || frequency == 0) {
        period->previousValid = false;
        /* A complete all-zero window is an idle signal, not a failed read. */
        if (!period->slow && feedback->localHotPages == 0 && feedback->remoteHotPages == 0 &&
            feedback->localFreq == 0 && feedback->remoteFreq == 0) {
            period->slow = true;
            GetAdaptivePeriods(true, &attr->scanTime, &attr->migPeriod);
            SMAP_LOGGER_INFO("[pid_period] pid=%d state=stable scan_ms=%u migrate_ms=%u reason=idle.", attr->pid,
                             attr->scanTime, attr->migPeriod);
        }
        return;
    }
    double ratio = feedback->remoteHotRatio;
    if (!period->previousValid) {
        period->previousValid = true;
        period->previousRatio = ratio;
        return;
    }
    double delta = ratio - period->previousRatio;
    period->previousRatio = ratio;
    bool switchToSlow = !period->slow && fabs(delta) <= ADAPTIVE_STABLE_DELTA + RATIO_EPSILON;
    bool switchToFast = period->slow && delta + RATIO_EPSILON >= ADAPTIVE_SURGE_DELTA;
    if (!switchToSlow && !switchToFast) {
        return;
    }
    period->slow = switchToSlow;
    GetAdaptivePeriods(period->slow, &attr->scanTime, &attr->migPeriod);
    period->previousValid = false;
    SMAP_LOGGER_INFO("[pid_period] pid=%d state=%s scan_ms=%u migrate_ms=%u delta_pp=%.3f.", attr->pid,
                     period->slow ? "stable" : "fast", attr->scanTime, attr->migPeriod, delta * 100.0);
}
