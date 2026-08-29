#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <time.h>
#include <sys/time.h>
#include "main.h" // Include main.h first for AppSettings definition
#include <config.h>

// Constants
constexpr size_t kAudioBufferSamples = AUDIO_SAMPLE_BUFFERS; // stereo samples (int16_t)
constexpr size_t kMonoBufferSamples = kAudioBufferSamples / 2;

// Global buffers
extern int16_t audioBuffer[kAudioBufferSamples];
extern int16_t recordingBuffer[kMonoBufferSamples];

// Audio processing constants
constexpr float kMostSensitiveDb = AUDIO_DB_SENSITIVITY_HIGH;
constexpr float kLeastSensitiveDb = AUDIO_DB_SENSITIVITY_LOW;
constexpr size_t kDbSmoothingWindow = AUDIO_DB_SMOOTHING_WINDOW;

// Time constants
constexpr time_t kMinimumValidEpochSeconds = 1609459200; // 2021-01-01T00:00:00Z
constexpr unsigned long kNtpSyncTimeoutMs = 15000;
constexpr unsigned long kNtpPollIntervalMs = 200;

// Storage mode
enum class StorageMode {
    SD_CARD,
    PSRAM
};

// PSRAM recording constants
constexpr uint32_t kPsramMaxRecordingMs = 30000; // 30 seconds max in PSRAM mode
constexpr size_t kPsramMaxQueueSize = 10; // Maximum number of recordings in PSRAM queue


struct WaveHeader
{
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize = 36;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1;
    uint16_t numChannels = 2;
    uint32_t sampleRate = 8000;
    uint32_t byteRate = 32000;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t subchunk2Size = 0;
};


extern AppSettings appSettings;

bool parseUnsignedNumber(const String &text, size_t start, size_t length, int &value);
bool parseIsoTimestampToEpoch(const String &timestampInput, time_t &epochSeconds, long &microseconds);
bool isEpochValid(time_t epoch);
bool synchronizeClockWithNtp();
void syncClockFromApiResponse(const String &responseBody);
WaveHeader makeWaveHeader(size_t dataBytes, uint32_t sampleRate);
float calculateDb(const int16_t *samples, size_t sampleCount);
float calculateAudioLevel(const int16_t *samples, size_t sampleCount);
int16_t calculatePeakSample(const int16_t *samples, size_t sampleCount, float &utilizationPercent);
String analyzeServerResponse(const String &response);
String formatIsoTimestamp(time_t epochSeconds, unsigned long recordedAtMs);
String formatIsoTimestampWithMs(time_t epochSeconds, unsigned long recordedAtMs);
String getFormattedTimeWithTimezone(); // Returns "HH:MM:SS" with timezone offset applied
size_t splitAudioBufferForRecording(const int16_t *source, size_t byteCount, int16_t *destination, size_t destinationSamples);
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
/** Live record channel index (0/1 = I2S slot). May differ from NVS until save/sync. */
extern uint8_t g_effectiveRecordInputChannel;
void syncEffectiveRecordInputChannelFromAppSettings();
#endif
String createRecordingPath();
bool ensureStorage();

// Storage mode functions
StorageMode getStorageMode();
bool isStorageModePsram();
bool isStorageModeSdCard();
bool isSdCardPermanentlyDisabled(); // Returns true if SD card failed at startup and is disabled until reboot
void storage_permanentlyDisableSdCard(); // Permanently disable SD card until reboot
void storage_revaluateMode(); // Force re-evaluation of storage mode based on current settings (call when SD card settings change)

// Recording storage mode functions (based on settings, not just what's mounted)
bool isRecordingModePsram();
bool isRecordingModeSdCard();
bool hasSdCardRecordingError(); // Returns true if recordToSdCard is enabled but SD card is not available

// File system maintenance and index functions removed for reliability

// Log file path helpers
String getLatestLogPath(); // Get path to today's (latest) log file

// Timestamp recalculation functions
time_t calculateEpochFromMillis(unsigned long recordedAtMs);
bool renameRecordingFile(const String &oldPath, time_t correctedEpoch, unsigned long recordedAtMs);

// Device ID (MAC address) functions
const String &getDeviceId(); // Returns 12-digit uppercase MAC address without colons

// Session ID functions
void initializeSessionId(); // Initialize 8-digit random session ID (called on startup)
const String &getSessionId(); // Returns 8-digit random session ID (persists until reboot)

// Storage monitoring functions
void storage_updateHealthMetrics(); // Update storage health metrics
void storage_recordWriteError(); // Record a storage write error
void storage_switchToPsramOnFailure(); // Switch to PSRAM when SD write fails at runtime (call after persistent write failure)
void storage_recordReadError(); // Record a storage read error
void storage_handleRuntimeSdCardFailure(); // Handle SD card runtime failure retry and reboot logic (call from maintenance task)
uint32_t storage_cleanupOldUploadedFiles(uint32_t maxAgeDays); // Cleanup old uploaded marker files (returns count deleted)
void storage_checkCapacityAlerts(); // Check storage capacity and log alerts if thresholds exceeded
uint32_t storage_cleanupCorruptedFiles(); // Auto-cleanup corrupted or incomplete files (returns count deleted)
bool storage_retryMount(); // Auto-retry storage mount with exponential backoff (returns true if mounted)
// storage_scanLast7DaysForMissingUploads removed - using filesystem-based queue
bool storage_deleteOldestFolderIfNeeded(); // Delete oldest folder if SD card >80% full (returns true if deleted)
bool storage_createDayIndexFile(int year, int month, int day); // Create index file for a completed day folder (returns true if created)
bool storage_hasDayIndexFile(int year, int month, int day); // Check if a day folder has a valid index file

// Monthly summary functions
// Creates/updates monthly summary files at /inbox/YYYY/MM/summary.json
// Aggregates daily index.json files into monthly statistics
bool storage_updateMonthlySummary(int year, int month); // Update summary for specific month

// Yearly summary functions
// Creates/updates yearly summary files at /inbox/YYYY/summary.json
// Aggregates monthly summary.json files into yearly statistics
bool storage_updateYearlySummary(int year); // Update summary for specific year

// Nightly summary update (runs at maintenance hour)
void storage_runNightlySummaryUpdate(); // Run nightly summary update for month and year

// WiFi TX power mapping function
// Maps user-facing level (1-10) to ESP32 TX power value (in 0.25 dBm units)
// Returns ESP32 TX power value: 8,20,34,44,52,60,68,74,78,84 for levels 1-10
uint8_t mapWifiTxPowerLevel(uint8_t level);

// Inbox web API helpers (paths under /inbox only; filenames are UTC)
bool inbox_canonicalizePath(const String &input, String &outPath);
bool inbox_isDayFolderPath(const String &canonicalPath);
bool inbox_parseWavBasenameUtcEpoch(const String &basename, time_t &outUtc);
// Recordings catalog (JSONL at /recordings/YYYY/MM/DD/summary.json) — web UI listing; playback uses inboxPath
bool storage_ensureDirectoryPath(const char *dirPath);
bool pendingWavToPredictedInboxPath(const String &pendingWavPath, String &outInboxPath);

struct RecordingsSummaryLine
{
    String pendingPath;
    String inboxPath;
    uint32_t durationMs = 0;
    String endReason;
    float peakDb = -120.0f;
    uint64_t sizeBytes = 0;
    uint32_t sampleRate = 0;
};

// Catalog on SD only when "record to SD card" is enabled (isRecordingModeSdCard); not for PSRAM recording mode.
bool recordings_appendSummaryLine(const RecordingsSummaryLine &line);
void recordings_deleteSummaryForDay(int year, int month, int day);
void storage_pruneRecordingsSummariesWithoutInbox();

// Recordings web API helpers (paths under /recordings only)
bool recordings_canonicalizePath(const String &input, String &outPath);
bool recordings_isDayFolderPath(const String &canonicalPath);
// /recordings/1970/... is from unsynced RTC (epoch); hide from UI and skip when pruning catalog tree.
bool recordings_isIgnoredEpochFolderPath(const String &canonicalPath);

// Atomic serial write function for JSON messages
// Ensures complete messages are sent without interruption
// Uses mutex protection and atomic write to prevent message fragmentation
void serialWriteJsonAtomic(const String &jsonMessage);