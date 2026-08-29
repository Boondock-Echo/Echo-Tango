#pragma once

#include <Arduino.h>
#include <RTClib.h>
#include <time.h>

class TimeKeeper
{
public:
    void begin();
    void loop();

    bool hasRtc() const;
    bool timeIsValid() const;

    bool syncFromRtc();
    bool ensureTimeFromNtp();
    bool syncFromNtp(bool force);
    bool syncFromApiResponse(const String &responseBody);
    bool syncFromEventTimestamp(time_t epochSeconds, long microseconds);

    void notifyNetworkConnected();
    
    // Apply timezone from settings (call when timezone setting changes)
    void applyTimezoneFromSettings();
    
    // Get boot time tracking for millis-based time calculation
    unsigned long getBootTimeMs() const;
    time_t getBootTimeEpoch() const;
    bool hasBootTime() const;

    // Time synchronization robustness features
    bool validateTimeBeforeCriticalOperation() const;
    struct TimeSyncMetrics {
        uint32_t ntpAttempts = 0;
        uint32_t ntpSuccesses = 0;
        uint32_t ntpFailures = 0;
        double largestTimeCorrectionSeconds = 0.0;
        time_t lastSyncEpoch = 0;
        double totalDriftSeconds = 0.0;
        uint32_t largeCorrectionAlerts = 0;
        uint32_t rtcSyncCount = 0;
        double rtcTotalDriftSeconds = 0.0;
        double rtcMaxDriftSeconds = 0.0;
    };
    TimeSyncMetrics getTimeSyncMetrics() const;

private:
    static constexpr unsigned long kPeriodicNtpIntervalMs = 30UL * 60UL * 1000UL;
    static constexpr unsigned long kNtpRetryIntervalMs = 60UL * 1000UL;
    static constexpr unsigned long kForcedNtpRetryIntervalMs = 5000UL;
    static constexpr unsigned long kStartupNtpRetryIntervalMs = 5000UL; // Aggressive retry on startup (5 seconds)
    static constexpr unsigned long kStartupNtpTimeoutMs = 300000UL; // 5 minutes max startup retry time
    static constexpr long kRtcSkewThresholdSeconds = 30;
    static constexpr double kLargeCorrectionThresholdSeconds = 5.0;

    bool setSystemAndRtcTime(time_t epochSeconds, long microseconds);
    bool setSystemTime(time_t epochSeconds, long microseconds);
    bool shouldResyncRtc(time_t targetEpochSeconds);
    void recordBootTime();

    RTC_DS3231 rtc;
    bool rtcAvailable = false;
    bool wireInitialized = false;
    unsigned long lastPeriodicNtpMs = 0;
    unsigned long lastNtpAttemptMs = 0;
    
    // Boot time tracking for millis-based time calculation
    unsigned long bootTimeMs = 0;  // millis() value at boot
    time_t bootTimeEpoch = 0;       // Real time epoch at boot (0 if not synced yet)
    bool bootTimeRecorded = false;
    
    // Time synchronization monitoring
    TimeSyncMetrics metrics;
    time_t lastSyncEpoch = 0;  // Last successful sync epoch time
    time_t lastRtcSyncEpoch = 0;  // Last RTC sync epoch time
    
    // Startup time sync tracking
    unsigned long startupBeginMs = 0;  // When startup time sync began
    bool startupSyncMode = false;      // True during aggressive startup sync period
};

TimeKeeper &timeKeeper();
