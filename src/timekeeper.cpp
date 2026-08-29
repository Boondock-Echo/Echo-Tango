#include "timekeeper.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>
#include <sys/time.h>
#include <cstdlib>
#include <cmath>

#include "common.h"
#include "logger.h"
#include "main.h"
#include "recorder.h"

namespace
{
    TimeKeeper g_timeKeeperInstance;
}

TimeKeeper &timeKeeper()
{
    return g_timeKeeperInstance;
}

void TimeKeeper::begin()
{
    // Record boot time immediately for millis-based time calculation
    recordBootTime();
    
    // Apply timezone from settings (must be done before NTP sync)
    applyTimezoneFromSettings();
    
    // Enable aggressive startup sync mode
    startupBeginMs = millis();
    startupSyncMode = true;
    lastNtpAttemptMs = 0; // Reset to allow immediate NTP attempt
    
    // Only initialize RTC if enabled in settings
    if (!appSettings.rtc.enabled)
    {
        rtcAvailable = false;
    }
    else
    {
        if (!wireInitialized)
        {
            Wire.begin(appSettings.rtc.sdaPin, appSettings.rtc.sclPin);
            wireInitialized = true;
            delay(10);
        }

        rtcAvailable = rtc.begin();
        if (!rtcAvailable)
        {
        }
        else if (rtc.lostPower())
        {
        }
        else if (syncFromRtc())
        {
            // Update boot time epoch if we got time from RTC
            time_t nowEpoch = 0;
            time(&nowEpoch);
            if (isEpochValid(nowEpoch))
            {
                bootTimeEpoch = nowEpoch;
                startupSyncMode = false; // Got time from RTC, exit startup mode
            }
        }
    }
    
    // If we still don't have valid time, we'll try NTP aggressively in loop()
}

void TimeKeeper::loop()
{
    // If RTC is enabled and available, try to sync from RTC first
    if (appSettings.rtc.enabled && rtcAvailable && !timeIsValid())
    {
        if (syncFromRtc())
        {
            startupSyncMode = false; // Got time from RTC, exit startup mode
            return;
        }
    }

    // Aggressive NTP retry during startup (first 5 minutes or until synced)
    if (startupSyncMode && !timeIsValid())
    {
        const unsigned long startupElapsed = millis() - startupBeginMs;
        if (startupElapsed < kStartupNtpTimeoutMs)
        {
            // Try NTP every 5 seconds during startup
            if (WiFi.isConnected())
            {
                const unsigned long nowMs = millis();
                if ((nowMs - lastNtpAttemptMs) >= kStartupNtpRetryIntervalMs)
                {
                    if (syncFromNtp(true))
                    {
                        startupSyncMode = false; // Got time from NTP, exit startup mode
                        return;
                    }
                }
            }
            return; // Don't do periodic sync during startup mode
        }
        else
        {
            // Startup timeout reached, exit startup mode
            startupSyncMode = false;
        }
    }

    if (!WiFi.isConnected())
    {
        return;
    }

    if (!timeIsValid())
    {
        syncFromNtp(true);
        return;
    }

    const unsigned long nowMs = millis();
    if ((nowMs - lastPeriodicNtpMs) >= kPeriodicNtpIntervalMs)
    {
        syncFromNtp(true);
    }
}

bool TimeKeeper::hasRtc() const
{
    return rtcAvailable;
}

bool TimeKeeper::timeIsValid() const
{
    time_t nowSeconds = 0;
    time(&nowSeconds);
    return isEpochValid(nowSeconds);
}

bool TimeKeeper::syncFromRtc()
{
    if (!rtcAvailable)
    {
        return false;
    }

    if (rtc.lostPower())
    {
        return false;
    }

    DateTime rtcNow = rtc.now();
    time_t epochSeconds = rtcNow.unixtime();
    if (!isEpochValid(epochSeconds))
    {
        return false;
    }

    // Monitor RTC accuracy if we have a previous sync
    if (isEpochValid(lastRtcSyncEpoch) && lastRtcSyncEpoch > 0)
    {
        double rtcDrift = difftime(epochSeconds, lastRtcSyncEpoch);
        metrics.rtcTotalDriftSeconds += fabs(rtcDrift);
        if (fabs(rtcDrift) > metrics.rtcMaxDriftSeconds)
        {
            metrics.rtcMaxDriftSeconds = fabs(rtcDrift);
        }
    }

    if (!setSystemTime(epochSeconds, 0))
    {
        return false;
    }

    metrics.rtcSyncCount++;
    lastRtcSyncEpoch = epochSeconds;
    return true;
}

bool TimeKeeper::ensureTimeFromNtp()
{
    if (timeIsValid())
    {
        return true;
    }

    return syncFromNtp(true);
}

bool TimeKeeper::syncFromNtp(bool force)
{
    if (!WiFi.isConnected())
    {
        return false;
    }

    const unsigned long nowMs = millis();
    const unsigned long minInterval = force ? kForcedNtpRetryIntervalMs : kNtpRetryIntervalMs;
    if ((nowMs - lastNtpAttemptMs) < minInterval)
    {
        return false;
    }

    lastNtpAttemptMs = nowMs;
    metrics.ntpAttempts++;
    
    // Apply timezone from settings before NTP sync (configTime is called inside)
    applyTimezoneFromSettings();

    time_t currentTime = 0;
    const unsigned long start = millis();
    while ((millis() - start) < kNtpSyncTimeoutMs)
    {
        time(&currentTime);
        if (isEpochValid(currentTime))
        {
            // Monitor time drift before updating
            time_t previousSyncEpoch = lastSyncEpoch;
            if (isEpochValid(previousSyncEpoch) && previousSyncEpoch > 0)
            {
                double driftSeconds = difftime(currentTime, previousSyncEpoch);
            }
            
            const bool updated = setSystemAndRtcTime(currentTime, 0);
            if (updated)
            {
                lastPeriodicNtpMs = millis();
                metrics.ntpSuccesses++;
                lastSyncEpoch = currentTime;
                metrics.lastSyncEpoch = currentTime;
            }
            else
            {
                metrics.ntpFailures++;
            }
            return updated;
        }
        delay(kNtpPollIntervalMs);
    }

    metrics.ntpFailures++;
    return false;
}

bool TimeKeeper::syncFromApiResponse(const String &responseBody)
{
    if (responseBody.isEmpty())
    {
        return false;
    }

    if (!timeIsValid())
    {
        ensureTimeFromNtp();
    }

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, responseBody) != DeserializationError::Ok)
    {
        return false;
    }

    const char *timestampString = nullptr;
    if (doc.containsKey("timestamp") && doc["timestamp"].is<const char *>())
    {
        timestampString = doc["timestamp"].as<const char *>();
    }
    else if (doc.containsKey("current_time") && doc["current_time"].is<const char *>())
    {
        timestampString = doc["current_time"].as<const char *>();
    }

    if (timestampString == nullptr)
    {
        return false;
    }

    time_t epochSeconds = 0;
    long microseconds = 0;
    if (!parseIsoTimestampToEpoch(String(timestampString), epochSeconds, microseconds))
    {
        return false;
    }

    bool eventSynced = syncFromEventTimestamp(epochSeconds, microseconds);


    return eventSynced;
}

bool TimeKeeper::syncFromEventTimestamp(time_t epochSeconds, long microseconds)
{
    if (!isEpochValid(epochSeconds))
    {
        return false;
    }

    struct timeval current = {};
    gettimeofday(&current, nullptr);

    const long long currentSeconds = static_cast<long long>(current.tv_sec);
    const long long targetSeconds = static_cast<long long>(epochSeconds);
    const long long systemDelta = targetSeconds - currentSeconds;

    const bool systemNeedsUpdate = !isEpochValid(current.tv_sec) || llabs(systemDelta) >= kRtcSkewThresholdSeconds;
    const bool rtcNeedsUpdate = shouldResyncRtc(epochSeconds);

    if (!systemNeedsUpdate && !rtcNeedsUpdate)
    {
        return false;
    }

    return setSystemAndRtcTime(epochSeconds, microseconds);
}

void TimeKeeper::notifyNetworkConnected()
{
    lastNtpAttemptMs = 0; // Reset to allow immediate NTP attempt
    // If in startup mode and time is invalid, try to sync immediately
    if (startupSyncMode && !timeIsValid())
    {
        syncFromNtp(true);
    }
    else if (!timeIsValid())
    {
        syncFromNtp(true);
    }
}

bool TimeKeeper::setSystemAndRtcTime(time_t epochSeconds, long microseconds)
{
    if (!setSystemTime(epochSeconds, microseconds))
    {
        return false;
    }

    // If boot time epoch wasn't set yet, set it now (first time sync)
    bool isFirstTimeSync = (bootTimeEpoch == 0 && bootTimeRecorded);
    if (isFirstTimeSync)
    {
        bootTimeEpoch = epochSeconds;
        
        // Timestamp recalculation removed - using filesystem-based queue
        // Files in /pending are uploaded as-is
    }

    if (rtcAvailable && shouldResyncRtc(epochSeconds))
    {
        DateTime newTime(epochSeconds);
        rtc.adjust(newTime);
    }

    return true;
}

bool TimeKeeper::setSystemTime(time_t epochSeconds, long microseconds)
{
    if (!isEpochValid(epochSeconds))
    {
        return false;
    }

    struct timeval target = {};
    target.tv_sec = epochSeconds;
    target.tv_usec = microseconds;

    struct timeval before = {};
    gettimeofday(&before, nullptr);

    if (settimeofday(&target, nullptr) != 0)
    {
        return false;
    }

    double deltaSeconds = static_cast<double>(target.tv_sec - before.tv_sec);
    deltaSeconds += static_cast<double>(target.tv_usec - before.tv_usec) / 1000000.0;
    
    // Track time drift and corrections
    if (isEpochValid(before.tv_sec))
    {
        double absDelta = fabs(deltaSeconds);
        metrics.totalDriftSeconds += absDelta;
        
        if (absDelta > metrics.largestTimeCorrectionSeconds)
        {
            metrics.largestTimeCorrectionSeconds = absDelta;
        }
        
        // Alert on large time corrections (>5 seconds)
        if (absDelta >= kLargeCorrectionThresholdSeconds)
        {
            metrics.largeCorrectionAlerts++;
            Serial.printf("[Clock] ALERT: Large time correction detected: %+0.3f seconds (threshold: %.1f s)\n",
                     deltaSeconds, kLargeCorrectionThresholdSeconds);
        }
    }
    return true;
}

bool TimeKeeper::shouldResyncRtc(time_t targetEpochSeconds)
{
    if (!rtcAvailable)
    {
        return false;
    }

    DateTime rtcNow = rtc.now();
    const long delta = static_cast<long>(targetEpochSeconds - rtcNow.unixtime());
    return labs(delta) >= kRtcSkewThresholdSeconds;
}

void TimeKeeper::recordBootTime()
{
    if (!bootTimeRecorded)
    {
        bootTimeMs = millis();
        bootTimeRecorded = true;
    }
}

unsigned long TimeKeeper::getBootTimeMs() const
{
    return bootTimeMs;
}

time_t TimeKeeper::getBootTimeEpoch() const
{
    return bootTimeEpoch;
}

bool TimeKeeper::hasBootTime() const
{
    return bootTimeRecorded;
}

bool TimeKeeper::validateTimeBeforeCriticalOperation() const
{
    if (!timeIsValid())
    {
        return false;
    }
    
    time_t nowSeconds = 0;
    time(&nowSeconds);
    
    // Check if time is reasonable (not too far in past/future)
    time_t currentEpoch = 0;
    time(&currentEpoch);
    const time_t maxReasonableFuture = currentEpoch + 86400; // 24 hours in future
    const time_t minReasonablePast = currentEpoch - 86400;  // 24 hours in past
    
    if (nowSeconds > maxReasonableFuture || nowSeconds < minReasonablePast)
    {
        Serial.printf("[Clock] WARNING: Time validation failed - time appears unreasonable: %ld\n",
                 static_cast<long>(nowSeconds));
        return false;
    }
    
    return true;
}

TimeKeeper::TimeSyncMetrics TimeKeeper::getTimeSyncMetrics() const
{
    return metrics;
}

void TimeKeeper::applyTimezoneFromSettings()
{
    // Convert hours to seconds for configTime
    // configTime expects: (gmtOffset_sec, daylightOffset_sec, ...)
    // We use a fixed offset (no daylight saving time handling)
    long gmtOffsetSeconds = static_cast<long>(appSettings.timezone.offsetHours) * 3600L;
    int daylightOffsetSeconds = 0; // No DST handling
    
    // Set timezone for NTP synchronization
    // Note: configTime must be called before NTP sync, and it persists until next call
    // This configures both the timezone offset and the NTP servers
    configTime(gmtOffsetSeconds, daylightOffsetSeconds, "pool.ntp.org", "time.nist.gov", "time.google.com");
}
