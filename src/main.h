#ifndef BOONDOCK_MAIN_H
#define BOONDOCK_MAIN_H

#include <Arduino.h>
#include <array>
#include "config.h"

// Maximum number of WiFi credential entries to store and try in order
constexpr size_t kMaxWifiCredentials = 3;
// Keep SerialTask stack consistent across startup + health monitoring + restarts.
constexpr uint32_t kSerialTaskStackSize = 8192;

// Constants for API endpoints from config.h
constexpr const char* const kApiEndpoints[] = DEFAULT_AUDIO_UPLOAD_HOSTS_IP;
constexpr size_t kApiEndpointCount = DEFAULT_AUDIO_UPLOAD_HOST_COUNT;
constexpr size_t kMaxApiHostLength = 63;
constexpr size_t kMaxMqttKeyLength = 127;

struct WiFiSettings {
    const char *ssid;
    const char *password;
    unsigned long connectTimeoutMs;
    bool staticIpEnabled;
    char staticIp[16];
    char staticSubnet[16];
    char staticGateway[16];
    char staticDns1[16];
    char staticDns2[16];
};

struct AudioSettings {
    uint32_t sampleRate;
    size_t bufferSamples;
    uint8_t audioThreshold;
    uint32_t preRecordMs;
    uint32_t minRecordingMs;
    uint32_t maxRecordingMs;
    uint32_t silenceThresholdMs;
    bool discardSmallFilesEnabled;
    uint32_t discardSmallFilesMinMs;
    int8_t codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    /** Codec stereo line index: 0 or 1. Web UI maps labels Right→0, Left→1. TANGO only. */
    uint8_t recordInputChannel;
#endif
};

struct UploadSettings {
    uint8_t queueDepth;  // Legacy - kept for compatibility
    bool convertToMp3;   // Legacy
    char apiHosts[kApiEndpointCount][kMaxApiHostLength + 1];
    uint16_t apiPorts[kApiEndpointCount];
    bool enabled[kApiEndpointCount];  // [0]=Ohio [1]=Oregon [2]=Virginia [3]=Custom
};

struct RtcSettings {
    bool enabled;
    uint8_t sdaPin;
    uint8_t sclPin;
};

struct SdCardSettings {
    bool useSdCard;
    bool recordToSdCard;
    bool mode1bit;
    uint32_t frequency;
    bool formatIfMountFailed;
};

struct TimezoneSettings {
    int8_t offsetHours;
    uint8_t maintenanceHour;
    uint8_t maintenanceMinute;
};

struct LogSettings {
    bool serialFatal;
    bool serialError;
    bool serialWarning;
    bool serialInfo;
    bool serialDebug;
    bool serialEvent;
    bool fileFatal;
    bool fileError;
    bool fileWarning;
    bool fileInfo;
    bool fileDebug;
    bool fileEvent;
};

struct AppSettings {
    WiFiSettings wifi[kMaxWifiCredentials];
    AudioSettings audio;
    UploadSettings upload;
    RtcSettings rtc;
    SdCardSettings sdCard;
    TimezoneSettings timezone;
    LogSettings log;
    char hostname[MAX_HOSTNAME_LENGTH + 1];
    char mqttKey[kMaxMqttKeyLength + 1];
    uint8_t wifiTxPower;
    bool webserverEnabled;
    bool speakerEnabled;
    uint8_t speakerVolume; // 0-100
    bool transmitEnabled;   // Master TX enable (MQTT tx_on / audio.transmitEnabled)
    uint8_t transmitVolume; // 0-100 volume used during transmit playback

#if defined(ECHO)
    // Repeater mode (ECHO-only)
    bool repeaterEnabled;
    uint8_t repeaterMode; // 1=simplex, 2=duplex
#endif

    // CW (Morse) settings (stored globally; persisted on explicit save)
    uint8_t cwWpm;          // 5-40 typical
    uint16_t cwToneHz;      // 500-800 (UI range)
    uint8_t cwVolume;       // 0-100
    uint8_t cwRepeat;       // 1-5

    // ECHO legacy LED compatibility settings
    uint8_t ledStyle;        // 0=flashing, 1=solid
    uint8_t startupMode;     // see EchoStartupMode numeric values
    bool offlineMode;        // legacy "offline" flag (affects wifi error LED behavior)
};

extern AppSettings appSettings;

// System functions
const char* system_getResetReasonString();  // Reset reason for current boot (e.g. for online event)
bool system_rebootFromCli();
// Waits for recorder to finish (or timeout), then restarts. Does not tear down the record task like CLI reboot.
void system_requestGracefulReboot(const char* reason);
bool system_isUploading();
int system_getUploadQueueSize();
void system_notifySettingsChanged();
// Clears debounced config-send state after an immediate sendConfigMessage() (e.g. Echo CLI SET).
void system_clearPendingConfigMessage();
void system_checkAndStartTasksIfWiFiConfigured(); // Check if WiFi credentials were added and start tasks
void sendConfigMessage();
void sendHealthMessage(bool mutexAlreadyHeld = false);

// Mutex timeout metrics
struct MutexMetrics {
    uint32_t totalAttempts = 0;
    uint32_t timeoutCount = 0;
    uint32_t successCount = 0;
    float timeoutRate = 0.0f;
    unsigned long lastTimeoutMs = 0;
    uint32_t consecutiveTimeouts = 0;
};

// Network quality metrics
struct NetworkQualityMetrics {
    int32_t currentRssi = 0;
    int32_t minRssi = 0;
    int32_t maxRssi = 0;
    float averageRssi = 0.0f;
    uint32_t rssiSampleCount = 0;
    uint32_t packetLossCount = 0;
    uint32_t totalPackets = 0;
    float packetLossRate = 0.0f;
    unsigned long lastRssiUpdateMs = 0;
};

// Endpoint health metrics
struct EndpointHealthMetrics {
    const char* host = nullptr;
    uint32_t totalRequests = 0;
    uint32_t successCount = 0;
    uint32_t failureCount = 0;
    unsigned long totalResponseTimeMs = 0;
    unsigned long averageResponseTimeMs = 0;
    unsigned long minResponseTimeMs = UINT32_MAX;
    unsigned long maxResponseTimeMs = 0;
    float healthScore = 100.0f;
    unsigned long lastSuccessMs = 0;
    unsigned long lastFailureMs = 0;
    bool circuitBreakerOpen = false;
    unsigned long circuitBreakerOpenMs = 0;
    float successRate = 1.0f;
};

// Storage health metrics
struct StorageHealthMetrics {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;
    float utilizationPercent = 0.0f;
    uint32_t writeErrorCount = 0;
    uint32_t readErrorCount = 0;
    uint32_t mountFailureCount = 0;
    unsigned long lastMountFailureMs = 0;
    bool mountStable = true;
    unsigned long lastHealthCheckMs = 0;
};

// Task health metrics
struct TaskHealthMetrics {
    const char* taskName = nullptr;
    TaskHandle_t taskHandle = nullptr;
    uint32_t allocatedStackSize = 0;
    uint32_t minFreeStack = UINT32_MAX;
    uint32_t currentFreeStack = 0;
    float stackUtilizationPercent = 0.0f;
    uint32_t restartCount = 0;
    unsigned long lastRestartMs = 0;
    unsigned long lastCheckMs = 0;
    bool isRunning = false;
};

// Yearly summary cache (loaded from SD card)
struct YearlySummaryCache {
    int year = 0;
    uint32_t totalFiles = 0;
    uint64_t totalSizeBytes = 0;
    uint64_t totalDurationMs = 0;
    uint32_t monthsWithRecordings = 0;
    uint32_t totalDaysWithRecordings = 0;
    bool loaded = false;
    unsigned long loadedAtMs = 0;
};

// System health metrics
struct SystemHealthMetrics {
    MutexMetrics mutexMetrics;
    NetworkQualityMetrics networkQuality;
    StorageHealthMetrics storageHealth;
    TaskHealthMetrics recordTaskHealth;
    TaskHealthMetrics uploadTaskHealth;
    TaskHealthMetrics serialTaskHealth;
    TaskHealthMetrics maintenanceTaskHealth;
    TaskHealthMetrics webServerTaskHealth;
    float overallUploadSuccessRate = -1.0f;
    unsigned long totalUploadAttempts = 0;
    unsigned long totalSuccessfulUploads = 0;
    uint32_t safetyRebootCount = 0;
    unsigned long lastSafetyRebootMs = 0;
    uint32_t networkReconnectCount = 0;
    unsigned long lastNetworkReconnectMs = 0;
    uint32_t totalRecordedDurationSec = 0;
    YearlySummaryCache yearlySummary;

    // Aggregated API latency metrics across all endpoints (milliseconds)
    unsigned long apiMinResponseTimeMs = UINT32_MAX;
    unsigned long apiMaxResponseTimeMs = 0;
    unsigned long apiAverageResponseTimeMs = 0;
};

// Get system health metrics
SystemHealthMetrics system_getHealthMetrics();
void system_resetMetrics();
TaskHealthMetrics system_getTaskHealthMetrics(const char* taskName);

// Yearly summary functions
void health_loadYearlySummary();  // Load yearly summary from SD card into cache

#endif // BOONDOCK_MAIN_H
