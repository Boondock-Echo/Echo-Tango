#include "timekeeper.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <sys/time.h>
#include <cstdlib>
#include <cmath>

#include "common.h"
#include "logger.h"
#include "main.h"
#include "recorder.h"

#if defined(TANGO)
#include "mcp7941x.h"
#endif

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
    rtcAvailable = false;

    // TANGO may already have a valid UTC time in the MCP7941x. NTP still runs
    // once Wi-Fi is up and writes that time back into the chip.
    refreshExternalRtc();
    if (bootTimeEpoch == 0 && timeIsValid())
    {
        time(&bootTimeEpoch);
    }
}

void TimeKeeper::loop()
{
    // Aggressive NTP retry during startup (first 5 minutes or until NTP succeeds).
    // A TANGO unit that already took time from the MCP7941x stays in this window
    // so the first network sync can correct the chip, and it does not block the
    // rest of the loop while the clock is already valid.
    if (startupSyncMode)
    {
        const unsigned long startupElapsed = millis() - startupBeginMs;
        if (startupElapsed >= kStartupNtpTimeoutMs)
        {
            startupSyncMode = false;
        }
        else if (WiFi.isConnected())
        {
            const unsigned long nowMs = millis();
            if ((nowMs - lastNtpAttemptMs) >= kStartupNtpRetryIntervalMs)
            {
                if (syncFromNtp(true))
                {
                    startupSyncMode = false;
                    return;
                }
            }
        }

        if (!timeIsValid())
        {
            return;
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
#if !defined(TANGO)
    return false;
#else
    if (!rtcAvailable)
    {
        return false;
    }

    time_t chipEpoch = 0;
    if (!mcp7941x().readUtc(chipEpoch) || !isEpochValid(chipEpoch))
    {
        return false;
    }

    struct timeval before = {};
    gettimeofday(&before, nullptr);
    if (isEpochValid(before.tv_sec))
    {
        const double drift = fabs(difftime(chipEpoch, before.tv_sec));
        metrics.rtcTotalDriftSeconds += drift;
        if (drift > metrics.rtcMaxDriftSeconds)
        {
            metrics.rtcMaxDriftSeconds = drift;
        }
    }

    if (!setSystemTime(chipEpoch, 0))
    {
        return false;
    }

    metrics.rtcSyncCount++;
    logInfof("[Clock] Time set from MCP7941x");
    return true;
#endif
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
    StaticJsonDocument<96> filter;
    filter["timestamp"] = true;
    filter["current_time"] = true;
    if (deserializeJson(doc, responseBody, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
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
    // Startup still wants NTP when the MCP7941x already provided a valid time,
    // so the chip can be corrected as soon as the network is up.
    if (!timeIsValid() || startupSyncMode)
    {
        if (syncFromNtp(true))
        {
            startupSyncMode = false;
        }
    }
}

bool TimeKeeper::setSystemAndRtcTime(time_t epochSeconds, long microseconds)
{
    if (!setSystemTime(epochSeconds, microseconds))
    {
        return false;
    }

#if defined(TANGO)
    if (rtcAvailable)
    {
        time_t chipEpoch = 0;
        if (mcp7941x().readUtc(chipEpoch) && isEpochValid(chipEpoch))
        {
            const double drift = fabs(difftime(epochSeconds, chipEpoch));
            metrics.rtcTotalDriftSeconds += drift;
            if (drift > metrics.rtcMaxDriftSeconds)
            {
                metrics.rtcMaxDriftSeconds = drift;
            }
        }

        if (mcp7941x().writeUtc(epochSeconds))
        {
            metrics.rtcSyncCount++;
        }
        else
        {
            logWarnf("[Clock] MCP7941x time write failed");
        }
    }
#endif

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

    // First accepted clock is the boot epoch used by millis-based reconstruction.
    // Timestamp recalculation was removed; files in /pending are uploaded as-is.
    if (bootTimeEpoch == 0 && bootTimeRecorded)
    {
        bootTimeEpoch = epochSeconds;
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
#if !defined(TANGO)
    (void)targetEpochSeconds;
    return false;
#else
    if (!rtcAvailable)
    {
        return false;
    }

    time_t chipEpoch = 0;
    if (!mcp7941x().readUtc(chipEpoch) || !isEpochValid(chipEpoch))
    {
        return true;
    }

    const long long delta = static_cast<long long>(targetEpochSeconds) - static_cast<long long>(chipEpoch);
    return llabs(delta) >= kRtcSkewThresholdSeconds;
#endif
}

void TimeKeeper::refreshExternalRtc()
{
#if !defined(TANGO)
    rtcAvailable = false;
#else
    if (!appSettings.rtc.enabled)
    {
        mcp7941x().end();
        rtcAvailable = false;
        return;
    }

    if (!mcp7941x().begin())
    {
        rtcAvailable = false;
        logWarnf("[Clock] MCP7941x not found (SDA %d, SCL %d)", Mcp7941x::kSdaPin, Mcp7941x::kSclPin);
        return;
    }

    rtcAvailable = true;
    logInfof("[Clock] MCP7941x ready (SDA %d, SCL %d)", Mcp7941x::kSdaPin, Mcp7941x::kSclPin);

    if (!timeIsValid())
    {
        (void)syncFromRtc();
        return;
    }

    time_t nowSeconds = 0;
    time(&nowSeconds);
    if (isEpochValid(nowSeconds) && shouldResyncRtc(nowSeconds))
    {
        if (mcp7941x().writeUtc(nowSeconds))
        {
            metrics.rtcSyncCount++;
            logInfof("[Clock] MCP7941x updated from system time");
        }
        else
        {
            logWarnf("[Clock] MCP7941x time write failed");
        }
    }
#endif
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
