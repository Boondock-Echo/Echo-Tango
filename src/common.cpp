#include "common.h"
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <climits>
#include <vector>
#include <algorithm>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "logger.h"
#include "timekeeper.h"
#include "network.h"
#include "settings.h"

extern void recorder_invalidatePendingDirectoryCache();

// Define global variables
int16_t audioBuffer[kAudioBufferSamples];
int16_t recordingBuffer[kMonoBufferSamples];

#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
uint8_t g_effectiveRecordInputChannel = DEFAULT_AUDIO_RECORD_INPUT_CHANNEL;
#endif

// Helper function to allocate a string (for initialization only)
// This properly allocates memory that will be freed by assignStringField() when
// settings are updated or reloaded. The strings persist until then, which is
// acceptable for a long-running embedded system.
static const char* allocateString(const char* str) {
    if (str == nullptr || str[0] == '\0') {
        char* empty = static_cast<char*>(malloc(1));
        if (empty != nullptr) {
            empty[0] = '\0';
        }
        return empty;
    }
    return strdup(str);
}

static const char *const kDefaultUploadHosts[] = DEFAULT_AUDIO_UPLOAD_HOSTS_IP;

AppSettings appSettings = []() {
    AppSettings settings = {};

    // Initialize WiFi settings with properly allocated strings
    // These will be freed by assignStringField when settings are updated
    settings.wifi[0] = {allocateString(DEFAULT_WIFI_SSID), allocateString(DEFAULT_WIFI_PASSWORD), DEFAULT_WIFI_CONNECT_TIMEOUT_MS, false, {0}, {0}, {0}, {0}, {0}};
    settings.wifi[1] = {allocateString(""), allocateString(""), DEFAULT_WIFI_CONNECT_TIMEOUT_MS, false, {0}, {0}, {0}, {0}, {0}};
    settings.wifi[2] = {allocateString(""), allocateString(""), DEFAULT_WIFI_CONNECT_TIMEOUT_MS, false, {0}, {0}, {0}, {0}, {0}};

    settings.audio.sampleRate = DEFAULT_AUDIO_SAMPLE_RATE;
    settings.audio.bufferSamples = kAudioBufferSamples;
    settings.audio.audioThreshold = DEFAULT_AUDIO_THRESHOLD;
    settings.audio.preRecordMs = DEFAULT_AUDIO_PRE_RECORD_MS;
    settings.audio.minRecordingMs = DEFAULT_AUDIO_MIN_RECORDING_MS;
    if(DEFAULT_SD_USE_SD_CARD && DEFAULT_SD_RECORD_TO_SD_CARD)
    {
        settings.audio.maxRecordingMs = DEFAULT_AUDIO_MAX_SD_RECORDING_MS;
    }
    else
    {
        settings.audio.maxRecordingMs = DEFAULT_AUDIO_MAX_RECORDING_MS;
    }
    settings.audio.silenceThresholdMs = DEFAULT_AUDIO_SILENCE_THRESHOLD_MS;
    settings.audio.discardSmallFilesEnabled = DEFAULT_AUDIO_DISCARD_SMALL_FILES_ENABLED;
    settings.audio.discardSmallFilesMinMs = DEFAULT_AUDIO_DISCARD_SMALL_FILES_MIN_MS;
    settings.audio.codecGainDb = DEFAULT_AUDIO_CODEC_GAIN_DB;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    settings.audio.recordInputChannel = DEFAULT_AUDIO_RECORD_INPUT_CHANNEL;
    g_effectiveRecordInputChannel = DEFAULT_AUDIO_RECORD_INPUT_CHANNEL;
#endif

    settings.upload.queueDepth = DEFAULT_UPLOAD_QUEUE_DEPTH;
    settings.upload.convertToMp3 = DEFAULT_UPLOAD_CONVERT_TO_MP3;

    for (size_t i = 0; i < kApiEndpointCount; ++i)
    {
        const char *host = (i < (sizeof(kDefaultUploadHosts) / sizeof(kDefaultUploadHosts[0]))) ? kDefaultUploadHosts[i] : "";
        if (host == nullptr)
        {
            host = "";
        }
        std::strncpy(settings.upload.apiHosts[i], host, sizeof(settings.upload.apiHosts[i]) - 1);
        settings.upload.apiHosts[i][sizeof(settings.upload.apiHosts[i]) - 1] = '\0';
        settings.upload.apiPorts[i] = DEFAULT_API_PORT;
        settings.upload.enabled[i] = (i < 3);  // Ohio, Oregon, Virginia on by default; Custom (slot 3) off
    }

    // Initialize RTC settings (default: disabled, pins 5 and 18)
    settings.rtc.enabled = false;
    settings.rtc.sdaPin = 5;
    settings.rtc.sclPin = 18;

    // Initialize SD card settings (product-specific defaults — see config.h DEFAULT_SD_*)
    settings.sdCard.useSdCard = DEFAULT_SD_USE_SD_CARD;
    settings.sdCard.recordToSdCard = DEFAULT_SD_RECORD_TO_SD_CARD;
    settings.sdCard.mode1bit = false;      // 4-bit mode (faster)
    settings.sdCard.frequency = 10000000;   // 10MHz
    settings.sdCard.formatIfMountFailed = false; // Don't auto-format (safer)

    // Initialize timezone settings (default: UTC)
    settings.timezone.offsetHours = DEFAULT_TIMEZONE_OFFSET_HOURS;
    settings.timezone.maintenanceHour = DEFAULT_MAINTENANCE_HOUR;
    settings.timezone.maintenanceMinute = DEFAULT_MAINTENANCE_MINUTE;

    // Initialize log filter settings (all enabled by default)
    settings.log.serialFatal = DEFAULT_LOG_SERIAL_FATAL;
    settings.log.serialError = DEFAULT_LOG_SERIAL_ERROR;
    settings.log.serialWarning = DEFAULT_LOG_SERIAL_WARNING;
    
    settings.log.serialInfo = DEFAULT_LOG_SERIAL_INFO;
    settings.log.serialDebug = DEFAULT_LOG_SERIAL_DEBUG;
    settings.log.serialEvent = DEFAULT_LOG_SERIAL_EVENT;
    settings.log.fileFatal = DEFAULT_LOG_FILE_FATAL;
    settings.log.fileError = DEFAULT_LOG_FILE_ERROR;
    settings.log.fileWarning = DEFAULT_LOG_FILE_WARNING;
    settings.log.fileInfo = DEFAULT_LOG_FILE_INFO;
    settings.log.fileDebug = DEFAULT_LOG_FILE_DEBUG;
    settings.log.fileEvent = DEFAULT_LOG_FILE_EVENT;

    // Initialize WiFi TX power (default: 10 = 84 = 21 dBm, highest power)
    settings.wifiTxPower = DEFAULT_TX_POWER;
    std::strncpy(settings.hostname, DEFAULT_HOSTNAME, MAX_HOSTNAME_LENGTH);
    settings.hostname[MAX_HOSTNAME_LENGTH] = '\0';
    settings.mqttKey[0] = '\0';
    
    // Initialize webserver enabled (default: true)
    settings.webserverEnabled = true;

    // Speaker defaults
    settings.speakerEnabled = DEFAULT_SPEAKER_ENABLED;
    settings.speakerVolume = DEFAULT_SPEAKER_VOLUME;

    // TX defaults (feature not implemented yet)
    settings.transmitEnabled = DEFAULT_AUDIO_TRANSMIT_ENABLED;
    settings.transmitVolume = DEFAULT_AUDIO_TRANSMIT_VOLUME;

    #if defined(ECHO)
    // Repeater defaults (ECHO-only)
    settings.repeaterEnabled = false;
    settings.repeaterMode = 1; // simplex
    #endif

    // CW (Morse) defaults
    settings.cwWpm = DEFAULT_CW_WPM;
    settings.cwToneHz = DEFAULT_CW_TONE_HZ;
    settings.cwVolume = DEFAULT_CW_VOLUME;
    settings.cwRepeat = DEFAULT_CW_REPEAT;

    // Legacy LED defaults
    settings.ledStyle = DEFAULT_LED_STYLE;
    settings.startupMode = DEFAULT_STARTUP_MODE;
    settings.offlineMode = DEFAULT_OFFLINE_MODE;

    return settings;
}();

#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
void syncEffectiveRecordInputChannelFromAppSettings()
{
    if (appSettings.audio.recordInputChannel > 1U)
    {
        g_effectiveRecordInputChannel = 1;
    }
    else
    {
        g_effectiveRecordInputChannel = appSettings.audio.recordInputChannel;
    }
}
#endif

bool parseUnsignedNumber(const String &text, size_t start, size_t length, int &value)
{
    if (start + length > text.length())
    {
        return false;
    }

    int result = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = text.charAt(start + i);
        if (c < '0' || c > '9')
        {
            return false;
        }
        result = result * 10 + (c - '0');
    }
    value = result;
    return true;
}

bool parseIsoTimestampToEpoch(const String &timestampInput, time_t &epochSeconds, long &microseconds)
{
    String timestamp = timestampInput;
    timestamp.trim();

    if (timestamp.length() < 19)
    {
        return false;
    }

    if (timestamp.charAt(4) != '-' || timestamp.charAt(7) != '-' || timestamp.charAt(10) != 'T' ||
        timestamp.charAt(13) != ':' || timestamp.charAt(16) != ':')
    {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (!parseUnsignedNumber(timestamp, 0, 4, year) || !parseUnsignedNumber(timestamp, 5, 2, month) ||
        !parseUnsignedNumber(timestamp, 8, 2, day) || !parseUnsignedNumber(timestamp, 11, 2, hour) ||
        !parseUnsignedNumber(timestamp, 14, 2, minute) || !parseUnsignedNumber(timestamp, 17, 2, second))
    {
        return false;
    }

    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    {
        return false;
    }

    static constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    auto isLeapYear = [](int y)
    {
        return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    };

    const bool leap = isLeapYear(year);
    const int maxDay = (month == 2 && leap) ? 29 : kDaysInMonth[month - 1];
    if (day > maxDay)
    {
        return false;
    }

    int64_t days = 0;
    for (int y = 1970; y < year; ++y)
    {
        days += isLeapYear(y) ? 366 : 365;
    }

    for (int m = 1; m < month; ++m)
    {
        days += kDaysInMonth[m - 1];
        if (m == 2 && leap)
        {
            days += 1;
        }
    }

    days += (day - 1);

    int64_t totalSeconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;

    size_t cursor = 19;

    microseconds = 0;
    if (timestamp.length() > cursor && timestamp.charAt(cursor) == '.')
    {
        size_t idx = cursor + 1;

        int fractionalDigits = 0;
        int fractionalValue = 0;
        while (idx < timestamp.length())
        {
            const char c = timestamp.charAt(idx);
            if (c >= '0' && c <= '9')
            {
                if (fractionalDigits < 6)
                {
                    fractionalValue = fractionalValue * 10 + (c - '0');
                    fractionalDigits++;
                }
                ++idx;
            }
            else
            {
                break;
            }
        }

        while (fractionalDigits < 6)
        {
            fractionalValue *= 10;
            ++fractionalDigits;
        }

        microseconds = fractionalValue;
        cursor = idx;
    }

    int offsetSeconds = 0;

    if (cursor < timestamp.length())
    {
        const char tzIndicator = timestamp.charAt(cursor);
        if (tzIndicator == 'Z' || tzIndicator == 'z')
        {
            ++cursor;
        }
        else if (tzIndicator == '+' || tzIndicator == '-')
        {
            const int sign = (tzIndicator == '+') ? 1 : -1;
            ++cursor;

            int offsetHours = 0;
            if (!parseUnsignedNumber(timestamp, cursor, 2, offsetHours))
            {
                return false;
            }
            cursor += 2;

            int offsetMinutes = 0;
            if (cursor < timestamp.length() && timestamp.charAt(cursor) == ':')
            {
                ++cursor;
                if (!parseUnsignedNumber(timestamp, cursor, 2, offsetMinutes))
                {
                    return false;
                }
                cursor += 2;
            }
            else
            {
                if (!parseUnsignedNumber(timestamp, cursor, 2, offsetMinutes))
                {
                    return false;
                }
                cursor += 2;
            }

            if (offsetHours > 14 || offsetMinutes > 59)
            {
                return false;
            }

            offsetSeconds = sign * (offsetHours * 3600 + offsetMinutes * 60);
        }
        else
        {
            return false;
        }
    }

    if (cursor != timestamp.length())
    {
        return false;
    }

    totalSeconds -= offsetSeconds;
    epochSeconds = static_cast<time_t>(totalSeconds);

    return true;
}

bool isEpochValid(time_t epoch)
{
    return epoch >= kMinimumValidEpochSeconds;
}

bool synchronizeClockWithNtp()
{
    return timeKeeper().ensureTimeFromNtp();
}

void syncClockFromApiResponse(const String &responseBody)
{
    timeKeeper().syncFromApiResponse(responseBody);
}

WaveHeader makeWaveHeader(size_t dataBytes, uint32_t sampleRate)
{
    WaveHeader header;
    header.numChannels = 1;
    header.bitsPerSample = 16;
    header.sampleRate = sampleRate;
    header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
    header.byteRate = header.sampleRate * header.blockAlign;
    header.subchunk2Size = dataBytes;
    header.chunkSize = 36 + dataBytes;
    return header;
}

float calculateDb(const int16_t *samples, size_t sampleCount)
{
    if (sampleCount == 0)
    {
        return -120.0f;
    }

    double sumSquares = 0.0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const double sample = static_cast<double>(samples[i]) / 32768.0;
        sumSquares += sample * sample;
    }

    const double mean = sumSquares / static_cast<double>(sampleCount);
    const double rms = std::sqrt(mean);
    const double db = 20.0 * std::log10(rms + 1e-9);
    return static_cast<float>(db);
}

// TO-DO: Do we still need Audio Level if we are using an offset of Db for the threshold?
float calculateAudioLevel(const int16_t *samples, size_t sampleCount)
{
    if (sampleCount == 0)
    {
        return 0.0f;
    }

    double sumSquares = 0.0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const double sample = static_cast<double>(samples[i]) / 32768.0;
        sumSquares += sample * sample;
    }

    const double mean = sumSquares / static_cast<double>(sampleCount);
    const double rms = std::sqrt(mean);
    double level = rms * 100.0;

    if (level < 0.0)
    {
        level = 0.0;
    }
    else if (level > 100.0)
    {
        level = 100.0;
    }

    return static_cast<float>(level);
}

// Calculate peak sample value (absolute maximum) and dynamic range utilization
// Returns peak sample value (0-32767) and sets utilization percentage (0-100)
int16_t calculatePeakSample(const int16_t *samples, size_t sampleCount, float &utilizationPercent)
{
    if (sampleCount == 0)
    {
        utilizationPercent = 0.0f;
        return 0;
    }

    int16_t peak = 0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        // Calculate absolute value safely (handle INT16_MIN case)
        int16_t absValue;
        if (samples[i] == INT16_MIN)
        {
            absValue = INT16_MAX; // INT16_MIN absolute value is INT16_MAX
        }
        else if (samples[i] < 0)
        {
            absValue = -samples[i];
        }
        else
        {
            absValue = samples[i];
        }
        
        if (absValue > peak)
        {
            peak = absValue;
        }
    }

    // Calculate utilization as percentage of maximum possible value (32767)
    utilizationPercent = (static_cast<float>(peak) / 32767.0f) * 100.0f;
    if (utilizationPercent > 100.0f)
    {
        utilizationPercent = 100.0f;
    }

    return peak;
}

String analyzeServerResponse(const String &response)
{
    DynamicJsonDocument jsonDoc(1024);
    if (deserializeJson(jsonDoc, response) == DeserializationError::Ok)
    {
        if (jsonDoc.containsKey("error"))
        {
            return jsonDoc["error"].as<String>();
        }

        if (jsonDoc.containsKey("message"))
        {
            const String message = jsonDoc["message"].as<String>();
            if (message.indexOf("uploaded") != -1 || message.indexOf("Uploaded") != -1)
            {
                return "OK";
            }
        }

        if (jsonDoc.containsKey("local_path"))
        {
            return "OK";
        }
    }

    if (response.indexOf("OK") != -1 || response.indexOf("success") != -1 || response.indexOf("Success") != -1)
    {
        return "OK";
    }
    if (response.indexOf("Audio uploaded successfully") != -1)
    {
        return "OK";
    }

    // Error cases
    if (response.indexOf("E01") != -1) return "Unknown error uploading audio to server";
    if (response.indexOf("E02") != -1) return "Invalid parameters uploading audio to server";
    if (response.indexOf("E03") != -1) return "Duplicate file error uploading audio to server";
    if (response.indexOf("E04") != -1) return "File too large error uploading audio to server";
    if (response.indexOf("E05") != -1) return "Empty file error uploading audio to server";
    if (response.indexOf("E06") != -1) return "File too small error uploading audio to server";
    if (response.indexOf("E07") != -1) return "File save error uploading audio to server";
    if (response.indexOf("E08") != -1) return "Error moving file while uploading audio to server";
    if (response.indexOf("E09") != -1) return "Database error uploading audio to server";
    if (response.indexOf("E10") != -1) return "Directory creation error uploading audio to server";
    if (response.indexOf("E11") != -1) return "Wrong file format uploading audio to server";
    return "Unknown error uploading audio to server";
}

String formatIsoTimestamp(time_t epochSeconds, unsigned long recordedAtMs)
{
    if (epochSeconds > 0)
    {
        struct tm timeinfo = {};
#if defined(ESP32)
        gmtime_r(&epochSeconds, &timeinfo);
#else
        struct tm *timePtr = gmtime(&epochSeconds);
        if (timePtr != nullptr)
        {
            timeinfo = *timePtr;
        }
#endif
        char buffer[25] = {0};
        if (timeinfo.tm_year > 0 && strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo) > 0)
        {
            return String(buffer);
        }
    }

    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "ms-%lu", recordedAtMs);
    return String(fallback);
}

String formatIsoTimestampWithMs(time_t epochSeconds, unsigned long recordedAtMs)
{
    if (epochSeconds > 0)
    {
        struct tm timeinfo = {};
#if defined(ESP32)
        gmtime_r(&epochSeconds, &timeinfo);
#else
        struct tm *timePtr = gmtime(&epochSeconds);
        if (timePtr != nullptr)
        {
            timeinfo = *timePtr;
        }
#endif

        char buffer[32] = {0};
        int ms = static_cast<int>(recordedAtMs % 1000U);
        if (timeinfo.tm_year > 0 && strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo) > 0)
        {
            char full[40];
            std::snprintf(full, sizeof(full), "%s.%03dZ", buffer, ms);
            return String(full);
        }
    }

    char fallback[48];
    std::snprintf(fallback, sizeof(fallback), "ms-%lu", recordedAtMs);
    return String(fallback);
}

String getFormattedTimeWithTimezone()
{
    time_t now = 0;
    time(&now);
    
    // Get local time structure (localtime_r respects timezone set by configTime)
    struct tm timeinfo = {};
#if defined(ESP32)
    localtime_r(&now, &timeinfo);
#else
    struct tm *timePtr = localtime(&now);
    if (timePtr != nullptr)
    {
        timeinfo = *timePtr;
    }
#endif
    
    // Format as "HH:MM:SS"
    char buffer[10] = {0};
    if (timeinfo.tm_year > 0 && strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo) > 0)
    {
        return String(buffer);
    }
    
    // Fallback if time is not valid - apply offset manually as backup
    int8_t offsetHours = appSettings.timezone.offsetHours;
    time_t localTime = now + (static_cast<time_t>(offsetHours) * 3600L);
#if defined(ESP32)
    gmtime_r(&localTime, &timeinfo);
#else
    timePtr = gmtime(&localTime);
    if (timePtr != nullptr)
    {
        timeinfo = *timePtr;
    }
#endif
    if (strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo) > 0)
    {
        return String(buffer);
    }
    
    return String("00:00:00");
}

size_t splitAudioBufferForRecording(const int16_t *source, size_t byteCount, int16_t *destination, size_t destinationSamples)
{
    if (source == nullptr || destination == nullptr || byteCount == 0)
    {
        return 0;
    }

    const size_t totalSamples = byteCount / sizeof(int16_t);
    const size_t stereoFrames = totalSamples / 2;
    const size_t framesToCopy = std::min(stereoFrames, destinationSamples);

#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    const unsigned hwCh = (g_effectiveRecordInputChannel > 1U) ? 1u : static_cast<unsigned>(g_effectiveRecordInputChannel);
#else
    // Echo (and any build without selectable channel): legacy fixed I2S slot (formerly “codec right” channel).
    const unsigned hwCh = 1u;
#endif
    for (size_t frame = 0; frame < framesToCopy; ++frame)
    {
        const size_t srcIndex = frame * 2;
        destination[frame] = source[srcIndex + hwCh];
    }

    return framesToCopy;
}

String createRecordingPath()
{
    time_t now;
    time(&now);

    if (!isEpochValid(now))
    {
        // Fallback for unsynchronized clock - use millis-based naming
        // This will be renamed later when time syncs
        const unsigned long timestamp = millis();
        return "/queue/unsynced_" + String(timestamp) + ".wav";
    }

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "/queue/%Y-%m-%d-%H-%M-%S.wav", &timeinfo);
    return String(buffer);
}

time_t calculateEpochFromMillis(unsigned long recordedAtMs)
{
    // Calculate epoch time from millis() value using boot time tracking
    if (!timeKeeper().hasBootTime())
    {
        return 0; // Boot time not recorded yet
    }
    
    const unsigned long bootTimeMs = timeKeeper().getBootTimeMs();
    const time_t bootTimeEpoch = timeKeeper().getBootTimeEpoch();
    
    // If boot time epoch is not set yet, we can't calculate real time
    if (bootTimeEpoch == 0)
    {
        return 0;
    }
    
    // Calculate: realTime = bootTimeEpoch + (recordedAtMs - bootTimeMs) / 1000
    // Handle potential overflow by using int64_t
    const int64_t msSinceBoot = static_cast<int64_t>(recordedAtMs) - static_cast<int64_t>(bootTimeMs);
    const int64_t secondsSinceBoot = msSinceBoot / 1000LL;
    const time_t calculatedEpoch = static_cast<time_t>(static_cast<int64_t>(bootTimeEpoch) + secondsSinceBoot);
    
    return isEpochValid(calculatedEpoch) ? calculatedEpoch : 0;
}

bool renameRecordingFile(const String &oldPath, time_t correctedEpoch, unsigned long recordedAtMs)
{
    if (!isEpochValid(correctedEpoch))
    {
        return false;
    }
    
    // Only rename files in SD card mode
    if (!isStorageModeSdCard() || !isRecordingModeSdCard())
    {
        return false;
    }
    
    if (!SD_MMC.exists(oldPath))
    {
        return false;
    }
    
    // Create new path with corrected timestamp
    struct tm timeinfo;
    gmtime_r(&correctedEpoch, &timeinfo);
    
    // Determine if file is in /queue or /inbox
    String baseDir = "/queue";
    if (oldPath.startsWith("/inbox"))
    {
        baseDir = "/inbox";
        // Create dated subdirectory
        char dirPath[64];
        strftime(dirPath, sizeof(dirPath), "/inbox/%Y/%m/%d", &timeinfo);
        baseDir = String(dirPath);
        
        // Ensure directory exists
        auto ensureDirectoryRecursive = [&](const char *path) -> bool
        {
            String p = String(path);
            if (p.startsWith("/"))
            {
                p = p.substring(1);
            }
            
            String accum = String("");
            int start = 0;
            while (start < static_cast<int>(p.length()))
            {
                int slash = p.indexOf('/', start);
                String part;
                if (slash == -1)
                {
                    part = p.substring(start);
                    start = p.length();
                }
                else
                {
                    part = p.substring(start, slash);
                    start = slash + 1;
                }
                
                if (part.length() == 0)
                {
                    continue;
                }
                
                accum += "/" + part;
                if (!SD_MMC.exists(accum))
                {
                    if (!SD_MMC.mkdir(accum))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        
        if (!SD_MMC.exists(baseDir))
        {
            if (!ensureDirectoryRecursive(baseDir.c_str()))
            {
                return false;
            }
        }
    }
    
    // Create new filename with corrected timestamp
    char timePart[32];
    strftime(timePart, sizeof(timePart), "%Y-%m-%d-%H-%M-%S.wav", &timeinfo);
    String newPath = baseDir + "/" + String(timePart);
    
    // If file already exists with correct name, don't rename
    if (newPath == oldPath)
    {
        return true;
    }
    
    // Handle name collision (unlikely but possible)
    int counter = 1;
    String finalNewPath = newPath;
    while (SD_MMC.exists(finalNewPath))
    {
        char counterPart[16];
        std::snprintf(counterPart, sizeof(counterPart), "-%d", counter);
        int lastDot = finalNewPath.lastIndexOf('.');
        if (lastDot > 0)
        {
            finalNewPath = finalNewPath.substring(0, lastDot) + String(counterPart) + finalNewPath.substring(lastDot);
        }
        else
        {
            finalNewPath = newPath + String(counterPart);
        }
        counter++;
        if (counter > 100) // Safety limit
        {
            return false;
        }
    }
    
    // Rename the file
    logDebugf("[Storage] Renaming recording file: %s -> %s", oldPath.c_str(), finalNewPath.c_str());
    if (SD_MMC.rename(oldPath, finalNewPath))
    {
        logDebugf("[Storage] File renamed successfully: %s", finalNewPath.c_str());
        // Also rename .uploaded marker if it exists
        String oldMarker = oldPath + ".uploaded";
        if (SD_MMC.exists(oldMarker))
        {
            String newMarker = finalNewPath + ".uploaded";
            logDebugf("[Storage] Renaming .uploaded marker: %s -> %s", oldMarker.c_str(), newMarker.c_str());
            if (SD_MMC.rename(oldMarker, newMarker))
            {
                logDebugf("[Storage] Marker renamed successfully: %s", newMarker.c_str());
            }
            else
            {
                logDebugf("[Storage] Failed to rename marker: %s", newMarker.c_str());
            }
        }
        
        return true;
    }
    
    logDebugf("[Storage] Failed to rename file: %s -> %s", oldPath.c_str(), finalNewPath.c_str());
    return false;
}

// Storage health metrics (file scope for access from other files)
StorageHealthMetrics g_storageHealthMetrics = {};

namespace
{
    StorageMode g_storageMode = StorageMode::SD_CARD;
    bool g_storageInitialized = false;
    bool g_sdCardPermanentlyDisabled = false; // Set true if SD fails at startup - ignore until reboot
    uint32_t g_storageWriteErrorCount = 0;
    uint32_t g_storageReadErrorCount = 0;
    uint32_t g_storageMountFailureCount = 0;
    unsigned long g_lastStorageMountAttemptMs = 0;
    uint32_t g_storageMountRetryCount = 0;
    constexpr unsigned long kStorageMountRetryBackoffMs = 60000; // 1 minute initial backoff
    constexpr unsigned long kStorageMountMaxBackoffMs = 600000; // 10 minutes max backoff
    constexpr uint32_t kStorageMountMaxRetries = 10; // Max retries before giving up
    
    // Runtime SD card failure tracking (for cards that worked at startup but failed later)
    bool g_sdCardRuntimeFailure = false; // Set true when SD card fails during runtime
    unsigned long g_sdCardFailureStartMs = 0; // When runtime failure started
    unsigned long g_lastSdCardRetryAttemptMs = 0; // Last retry attempt time
    uint32_t g_sdCardRuntimeRetryCount = 0; // Retry count for runtime failures
    constexpr unsigned long kSdCardRuntimeRetryBackoffMs = 60000; // 1 minute initial backoff
    constexpr unsigned long kSdCardRuntimeMaxFailureMs = 600000; // 10 minutes before reboot
    bool g_sdCardWasAvailableAtStartup = false; // Track if SD was available at startup

    bool beginSdCardMount()
    {
#if defined(SD_MMC_MODE_LEGACY)
        // Legacy SD mode uses SPI-compatible initialization and ignores SDMMC tuning settings.
        return SD_MMC.begin("/sdcard", true, false);
#else
        return SD_MMC.begin(
            "/sdcard",
            appSettings.sdCard.mode1bit,
            appSettings.sdCard.formatIfMountFailed,
            appSettings.sdCard.frequency,
            SD_MMC_MAX_OPEN_FILES);
#endif
    }

    bool initializeSdCardWithRetries(int maxRetries)
    {
        for (int attempt = 0; attempt < maxRetries; ++attempt)
        {
            if (attempt > 0)
            {
                delay(200); // Wait before retry
                SD_MMC.end(); // End previous failed attempt
            }

            delay(100);
            if (beginSdCardMount())
            {
                return true;
            }
        }

        return false;
    }
}

StorageMode getStorageMode()
{
    return g_storageMode;
}

bool isStorageModePsram()
{
    return g_storageMode == StorageMode::PSRAM;
}

bool isStorageModeSdCard()
{
    // When SD has runtime failure, treat as not available so recording uses PSRAM until remount
    if (g_sdCardRuntimeFailure)
    {
        return false;
    }
    return g_storageMode == StorageMode::SD_CARD;
}

bool isSdCardPermanentlyDisabled()
{
    return g_sdCardPermanentlyDisabled;
}

void storage_permanentlyDisableSdCard()
{
    g_sdCardPermanentlyDisabled = true;
    recorder_invalidatePendingDirectoryCache();
    logWarnf("[Storage] SD card permanently disabled until next reboot");
}

// Force re-evaluation of storage mode based on current settings
// Call this when SD card settings (useSdCard, recordToSdCard) change
void storage_revaluateMode()
{
    // If user explicitly enables SD card, reset the permanently disabled flag
    // This allows retry if SD card was disabled due to startup failure
    if (appSettings.sdCard.useSdCard && g_sdCardPermanentlyDisabled)
    {
        logInfof("[Storage] SD card re-enabled in settings - resetting disabled flag");
        g_sdCardPermanentlyDisabled = false;
        g_storageMountRetryCount = 0; // Reset retry count
        g_lastStorageMountAttemptMs = 0;
    }
    
    // If SD card is currently mounted but useSdCard is now disabled, unmount it
    if (g_storageMode == StorageMode::SD_CARD && SD_MMC.cardType() != CARD_NONE)
    {
        if (!appSettings.sdCard.useSdCard)
        {
            logInfof("[Storage] SD card disabled in settings - unmounting");
            SD_MMC.end();
        }
    }
    
    // Reset initialization flag to force re-evaluation
    g_storageInitialized = false;
    
    // Re-evaluate storage based on current settings
    ensureStorage();
    
    if (g_storageMode == StorageMode::SD_CARD)
    {
        logInfof("[Storage] Storage mode: SD Card");
    }
    else if (g_storageMode == StorageMode::PSRAM)
    {
        logInfof("[Storage] Storage mode: PSRAM");
    }
}

// Auto-retry storage mount with exponential backoff (called from maintenance task)
bool storage_retryMount()
{
    if (g_storageInitialized)
    {
        return true; // Already initialized
    }
    
    // Don't retry if SD card is permanently disabled (failed at startup)
    if (g_sdCardPermanentlyDisabled)
    {
        return false; // SD card permanently disabled until reboot
    }
    
    const unsigned long now = millis();
    
    // Check if enough time has passed since last attempt (exponential backoff)
    unsigned long backoffMs = kStorageMountRetryBackoffMs;
    if (g_storageMountRetryCount > 0)
    {
        // Exponential backoff: 1min, 2min, 4min, 8min, 10min (capped)
        backoffMs = (1UL << (g_storageMountRetryCount - 1)) * kStorageMountRetryBackoffMs;
        if (backoffMs > kStorageMountMaxBackoffMs)
        {
            backoffMs = kStorageMountMaxBackoffMs;
        }
    }
    
    if (g_lastStorageMountAttemptMs > 0 && (now - g_lastStorageMountAttemptMs) < backoffMs)
    {
        return false; // Not time to retry yet
    }
    
    if (g_storageMountRetryCount >= kStorageMountMaxRetries)
    {
        // Max retries reached, give up and use PSRAM if available
        logErrorf("[Storage] Max mount retries (%u) reached, giving up on SD card\n",
                 static_cast<unsigned>(kStorageMountMaxRetries));
        return false;
    }
    
    g_lastStorageMountAttemptMs = now;
    g_storageMountRetryCount++;
    
    // Try to mount
    return ensureStorage();
}

bool ensureStorage()
{
    if (g_storageInitialized)
    {
        return true;
    }

    // Check if SD card should be used based on settings
    // Also check if SD card was permanently disabled due to startup failure
    bool shouldUseSdCard = appSettings.sdCard.useSdCard && !g_sdCardPermanentlyDisabled;
    
    if (shouldUseSdCard)
    {
        // Check if SD_MMC is already initialized/connected
        // If card type is not CARD_NONE, SD is already connected
        if (SD_MMC.cardType() != CARD_NONE)
        {
            // SD is already connected, don't end it, just mark as initialized
            g_storageMode = StorageMode::SD_CARD;
            g_storageInitialized = true;
            g_storageMountRetryCount = 0; // Reset retry count on success
            g_lastStorageMountAttemptMs = 0;
            return true;
        }

        // SD is not initialized, try to initialize with retry.
        const bool sdInitialized = initializeSdCardWithRetries(3);

        if (sdInitialized)
        {
            g_storageMountRetryCount = 0; // Reset retry count on success
            g_lastStorageMountAttemptMs = 0;
        }

        if (sdInitialized)
        {
            g_storageMode = StorageMode::SD_CARD;
            g_storageInitialized = true;
            g_sdCardWasAvailableAtStartup = true; // Mark that SD was available at startup
            g_sdCardRuntimeFailure = false; // Reset runtime failure flag
            g_sdCardFailureStartMs = 0;
            g_sdCardRuntimeRetryCount = 0;
            g_lastSdCardRetryAttemptMs = 0;
            
            // Update storage health metrics
            g_storageHealthMetrics.totalBytes = SD_MMC.totalBytes();
            g_storageHealthMetrics.usedBytes = SD_MMC.usedBytes();
            g_storageHealthMetrics.freeBytes = g_storageHealthMetrics.totalBytes - g_storageHealthMetrics.usedBytes;
            if (g_storageHealthMetrics.totalBytes > 0)
            {
                g_storageHealthMetrics.utilizationPercent = 
                    (static_cast<float>(g_storageHealthMetrics.usedBytes) / 
                     static_cast<float>(g_storageHealthMetrics.totalBytes)) * 100.0f;
            }
            g_storageHealthMetrics.mountStable = true;
            g_storageHealthMetrics.lastHealthCheckMs = millis();
            
            return true;
        }
        else
        {
            // SD card was requested but failed to initialize at startup
            // Permanently disable SD card and switch to PSRAM mode until reboot
            g_storageMountFailureCount++;
            g_storageHealthMetrics.mountFailureCount++;
            g_storageHealthMetrics.lastMountFailureMs = millis();
            g_storageHealthMetrics.mountStable = false;
            g_sdCardPermanentlyDisabled = true;
            logWarnf("[Storage] SD card failed at startup - permanently disabled until reboot");

            // Turn off SdCard setting and reduce MAX_RECORDING_MS since SdCard isn't available
            appSettings.sdCard.useSdCard = false;
            appSettings.sdCard.recordToSdCard = false;
            appSettings.audio.maxRecordingMs = DEFAULT_AUDIO_MAX_RECORDING_MS;

            // Continue to check PSRAM as fallback
        }
    }
    else
    {
        if (g_sdCardPermanentlyDisabled)
        {
        }
        else
        {
        }
    }

    // SD card not available or disabled, check for PSRAM
    #ifdef ESP32
    if (ESP.getPsramSize() > 0 && ESP.getFreePsram() > 0)
    {
        g_storageMode = StorageMode::PSRAM;
        g_storageInitialized = true;
        if (g_sdCardPermanentlyDisabled)
        {
            logWarnf("[Storage] Using PSRAM mode (SD card failed at startup)");
        }
        else if (!shouldUseSdCard)
        {
        }
        else
        {
        }
        return true;
    }
    #endif

    // Neither SD card nor PSRAM available
    g_storageMountFailureCount++;
    g_storageHealthMetrics.mountFailureCount++;
    g_storageHealthMetrics.lastMountFailureMs = millis();
    g_storageHealthMetrics.mountStable = false;
    return false;
}

// Storage monitoring functions
void storage_updateHealthMetrics()
{
    if (!isStorageModeSdCard())
    {
        // PSRAM mode - update PSRAM metrics
        #ifdef ESP32
        uint32_t psramTotal = ESP.getPsramSize();
        if (psramTotal > 0)
        {
            uint32_t psramFree = ESP.getFreePsram();
            uint32_t psramUsed = psramTotal - psramFree;
            g_storageHealthMetrics.totalBytes = static_cast<uint64_t>(psramTotal);
            g_storageHealthMetrics.usedBytes = static_cast<uint64_t>(psramUsed);
            g_storageHealthMetrics.freeBytes = static_cast<uint64_t>(psramFree);
            if (psramTotal > 0)
            {
                g_storageHealthMetrics.utilizationPercent = 
                    (static_cast<float>(psramUsed) / static_cast<float>(psramTotal)) * 100.0f;
            }
        }
        #endif
        g_storageHealthMetrics.lastHealthCheckMs = millis();
        return;
    }
    
    // SD card mode
    if (SD_MMC.cardType() == CARD_NONE)
    {
        g_storageHealthMetrics.mountStable = false;
        return;
    }
    
    g_storageHealthMetrics.mountStable = true;
    g_storageHealthMetrics.totalBytes = SD_MMC.totalBytes();
    g_storageHealthMetrics.usedBytes = SD_MMC.usedBytes();
    g_storageHealthMetrics.freeBytes = g_storageHealthMetrics.totalBytes - g_storageHealthMetrics.usedBytes;
    
    if (g_storageHealthMetrics.totalBytes > 0)
    {
        g_storageHealthMetrics.utilizationPercent = 
            (static_cast<float>(g_storageHealthMetrics.usedBytes) / 
             static_cast<float>(g_storageHealthMetrics.totalBytes)) * 100.0f;
    }
    
    g_storageHealthMetrics.writeErrorCount = g_storageWriteErrorCount;
    g_storageHealthMetrics.readErrorCount = g_storageReadErrorCount;
    g_storageHealthMetrics.mountFailureCount = g_storageMountFailureCount;
    g_storageHealthMetrics.lastHealthCheckMs = millis();
}

// Switch storage mode to PSRAM when SD card write fails
void storage_switchToPsramOnFailure()
{
    // Only switch if we're currently in SD card mode and SD was available at startup
    if (g_storageMode == StorageMode::SD_CARD && g_sdCardWasAvailableAtStartup && !g_sdCardRuntimeFailure)
    {
        g_sdCardRuntimeFailure = true;
        g_sdCardFailureStartMs = millis();
        g_sdCardRuntimeRetryCount = 0;
        g_lastSdCardRetryAttemptMs = 0;
        recorder_invalidatePendingDirectoryCache();
    }
}

void storage_recordWriteError()
{
    g_storageWriteErrorCount++;
    g_storageHealthMetrics.writeErrorCount = g_storageWriteErrorCount;
}

void storage_recordReadError()
{
    g_storageReadErrorCount++;
    g_storageHealthMetrics.readErrorCount = g_storageReadErrorCount;
}

// Handle SD card runtime failure: retry with exponential backoff, reboot after 10 minutes
// Call this from maintenance task periodically
void storage_handleRuntimeSdCardFailure()
{
    // Only handle if we have a runtime failure (SD was available at startup but failed later)
    if (!g_sdCardRuntimeFailure || !g_sdCardWasAvailableAtStartup)
    {
        return;
    }
    
    // Don't retry if SD card is permanently disabled (startup failure)
    if (g_sdCardPermanentlyDisabled)
    {
        return;
    }
    
    const unsigned long now = millis();
    
    // Check if 10 minutes have passed since failure started - reboot device
    if (g_sdCardFailureStartMs > 0 && (now - g_sdCardFailureStartMs) >= kSdCardRuntimeMaxFailureMs)
    {
        logErrorf("[Storage] SD card has been failing for 10 minutes - rebooting device");
        delay(100);
        ESP.restart();
        return;
    }
    
    // Check if it's time to retry (exponential backoff starting at 1 minute)
    unsigned long backoffMs = kSdCardRuntimeRetryBackoffMs;
    if (g_sdCardRuntimeRetryCount > 0)
    {
        // Exponential backoff: 1min, 2min, 4min, 8min, 10min (capped)
        backoffMs = (1UL << (g_sdCardRuntimeRetryCount - 1)) * kSdCardRuntimeRetryBackoffMs;
        if (backoffMs > kStorageMountMaxBackoffMs)
        {
            backoffMs = kStorageMountMaxBackoffMs;
        }
    }
    
    if (g_lastSdCardRetryAttemptMs > 0 && (now - g_lastSdCardRetryAttemptMs) < backoffMs)
    {
        return; // Not time to retry yet
    }
    
    g_lastSdCardRetryAttemptMs = now;
    g_sdCardRuntimeRetryCount++;
    
    logInfof("[Storage] Attempting to remount SD card after runtime failure...");
    
    // Try to remount SD card
    SD_MMC.end(); // End current connection
    delay(500); // Wait a bit
    
    if (initializeSdCardWithRetries(1))
    {
        // SD card remounted successfully - switch back to SD card mode
        logInfof("[Storage] SD card remounted successfully - switching back to SD card mode");
        g_storageMode = StorageMode::SD_CARD;
        g_storageInitialized = true;
        g_sdCardRuntimeFailure = false;
        g_sdCardFailureStartMs = 0;
        g_sdCardRuntimeRetryCount = 0;
        g_lastSdCardRetryAttemptMs = 0;
        
        // Update storage health metrics
        g_storageHealthMetrics.totalBytes = SD_MMC.totalBytes();
        g_storageHealthMetrics.usedBytes = SD_MMC.usedBytes();
        g_storageHealthMetrics.freeBytes = g_storageHealthMetrics.totalBytes - g_storageHealthMetrics.usedBytes;
        if (g_storageHealthMetrics.totalBytes > 0)
        {
            g_storageHealthMetrics.utilizationPercent = 
                (static_cast<float>(g_storageHealthMetrics.usedBytes) / 
                 static_cast<float>(g_storageHealthMetrics.totalBytes)) * 100.0f;
        }
        g_storageHealthMetrics.mountStable = true;
        g_storageHealthMetrics.lastHealthCheckMs = millis();
    }
    else
    {
        // Remount failed - continue using PSRAM
        logWarnf("[Storage] SD card remount attempt %u failed — continuing without SD (PSRAM-only until next boot or successful remount)",
                 static_cast<unsigned>(g_sdCardRuntimeRetryCount));
    }
}

// Storage cleanup: Remove old uploaded files to free space
// Returns number of files deleted
uint32_t storage_cleanupOldUploadedFiles(uint32_t maxAgeDays)
{
    if (!isStorageModeSdCard())
    {
        return 0; // Only cleanup SD card files
    }
    
    uint32_t deletedCount = 0;
    time_t now = 0;
    time(&now);
    if (!isEpochValid(now))
    {
        return 0; // Can't cleanup without valid time
    }
    
    const time_t maxAgeSeconds = static_cast<time_t>(maxAgeDays) * 86400LL;
    
    // Cleanup function for directory walk
    std::function<uint32_t(const String &)> cleanupDir = [&](const String &dirPath) -> uint32_t
    {
        uint32_t count = 0;
        File dir = SD_MMC.open(dirPath);
        if (!dir)
        {
            return 0;
        }
        
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry)
            {
                break;
            }
            
            if (entry.isDirectory())
            {
                String child = String(entry.name());
                if (!child.startsWith("/"))
                {
                    child = String(dirPath) + "/" + child;
                }
                count += cleanupDir(child);
            }
            else
            {
                String fname = String(entry.name());
                if (!fname.startsWith("/"))
                {
                    fname = String(dirPath) + "/" + fname;
                }
                
                // Only delete .uploaded marker files (audio files are kept)
                if (fname.endsWith(".uploaded"))
                {
                    // Try to get file modification time
                    // Note: SD_MMC doesn't provide mtime, so we'll use filename parsing
                    // For now, delete marker files older than maxAgeDays based on directory structure
                    // Files in /inbox/YYYY/MM/DD/ are organized by date
                    if (dirPath.startsWith("/inbox"))
                    {
                        // Extract date from path: /inbox/YYYY/MM/DD
                        int y = 0, m = 0, d = 0;
                        if (sscanf(dirPath.c_str(), "/inbox/%d/%d/%d", &y, &m, &d) == 3)
                        {
                            struct tm fileDate = {};
                            fileDate.tm_year = y - 1900;
                            fileDate.tm_mon = m - 1;
                            fileDate.tm_mday = d;
                            time_t fileEpoch = mktime(&fileDate);
                            
                            if (isEpochValid(fileEpoch) && (now - fileEpoch) > maxAgeSeconds)
                            {
                                if (SD_MMC.remove(fname))
                                {
                                    count++;
                                }
                            }
                        }
                    }
                    else if (dirPath == "/queue")
                    {
                        // Files in /queue - parse filename timestamp
                        int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
                        const char *base = strrchr(fname.c_str(), '/');
                        const char *namePtr = base ? base + 1 : fname.c_str();
                        // Remove .uploaded extension for parsing
                        String nameWithoutExt = String(namePtr);
                        nameWithoutExt.replace(".uploaded", "");
                        int matched = sscanf(nameWithoutExt.c_str(), "%d-%d-%d-%d-%d-%d", &y, &mo, &d, &hh, &mm, &ss);
                        if (matched == 6)
                        {
                            struct tm fileDate = {};
                            fileDate.tm_year = y - 1900;
                            fileDate.tm_mon = mo - 1;
                            fileDate.tm_mday = d;
                            fileDate.tm_hour = hh;
                            fileDate.tm_min = mm;
                            fileDate.tm_sec = ss;
                            time_t fileEpoch = mktime(&fileDate);
                            
                            if (isEpochValid(fileEpoch) && (now - fileEpoch) > maxAgeSeconds)
                            {
                                if (SD_MMC.remove(fname))
                                {
                                    count++;
                                }
                            }
                        }
                    }
                }
            }
            
            entry.close();
        }
        
        dir.close();
        return count;
    };
    
    // Cleanup /inbox directory
    if (SD_MMC.exists("/inbox"))
    {
        deletedCount += cleanupDir("/inbox");
    }
    
    // Cleanup /queue directory
    if (SD_MMC.exists("/queue"))
    {
        deletedCount += cleanupDir("/queue");
    }
    
    return deletedCount;
}

// Helper function for recursive directory cleanup (needed for recursive lambda)
namespace
{
    uint32_t cleanupCorruptedInDirHelper(const String &dirPath)
    {
        uint32_t count = 0;
        File dir = SD_MMC.open(dirPath);
        if (!dir || !dir.isDirectory())
        {
            return 0;
        }
        
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry)
            {
                break;
            }
            
            if (entry.isDirectory())
            {
                String child = String(entry.name());
                if (!child.startsWith("/"))
                {
                    child = String(dirPath) + "/" + child;
                }
                count += cleanupCorruptedInDirHelper(child);
            }
            else
            {
                String fname = String(entry.name());
                if (!fname.startsWith("/"))
                {
                    fname = String(dirPath) + "/" + fname;
                }
                
                // Skip marker files
                if (fname.endsWith(".uploaded"))
                {
                    entry.close();
                    continue;
                }
                
                // Check for corrupted WAV files (too small to be valid, or missing header)
                if (fname.endsWith(".wav"))
                {
                    size_t fileSize = entry.size();
                    entry.close();
                    
                    // WAV files should be at least 44 bytes (header) + some data
                    // Files smaller than 100 bytes are likely corrupted or incomplete
                    if (fileSize > 0 && fileSize < 100)
                    {
                        if (SD_MMC.remove(fname))
                        {
                            count++;
                            // Also remove marker file if it exists
                            String marker = fname + ".uploaded";
                            if (SD_MMC.exists(marker))
                            {
                                SD_MMC.remove(marker);
                            }
                        }
                    }
                    else if (fileSize == 0)
                    {
                        // Zero-byte files are definitely corrupted
                        if (SD_MMC.remove(fname))
                        {
                            count++;
                            String marker = fname + ".uploaded";
                            if (SD_MMC.exists(marker))
                            {
                                SD_MMC.remove(marker);
                            }
                        }
                    }
                }
            }
            
            entry.close();
        }
        
        dir.close();
        return count;
    }
}

// Auto-cleanup corrupted or incomplete files
uint32_t storage_cleanupCorruptedFiles()
{
    if (!isStorageModeSdCard())
    {
        return 0; // Only cleanup SD card files
    }
    
    uint32_t deletedCount = 0;
    
    // Cleanup /inbox directory
    if (SD_MMC.exists("/inbox"))
    {
        deletedCount += cleanupCorruptedInDirHelper("/inbox");
    }
    
    // Cleanup /queue directory
    if (SD_MMC.exists("/queue"))
    {
        deletedCount += cleanupCorruptedInDirHelper("/queue");
    }
    
    if (deletedCount > 0)
    {
    }
    
    return deletedCount;
}

// Check storage capacity and alert if thresholds exceeded
void storage_checkCapacityAlerts()
{
    storage_updateHealthMetrics();

    extern StorageHealthMetrics g_storageHealthMetrics;

    constexpr float kWarningThreshold = 80.0f;
    constexpr float kCriticalThreshold = 90.0f;

    const float pct = g_storageHealthMetrics.utilizationPercent;
    const unsigned long long used = static_cast<unsigned long long>(g_storageHealthMetrics.usedBytes);
    const unsigned long long total = static_cast<unsigned long long>(g_storageHealthMetrics.totalBytes);

    static uint8_t lastAlertLevel = 0; // 0 = ok, 1 = warning, 2 = critical

    uint8_t alertLevel = 0;
    if (pct >= kCriticalThreshold)
    {
        alertLevel = 2;
    }
    else if (pct >= kWarningThreshold)
    {
        alertLevel = 1;
    }

    if (alertLevel == 2 && alertLevel != lastAlertLevel)
    {
        logErrorf("[Storage] CRITICAL: Storage %.1f%% full (%llu/%llu bytes)\n",
                  pct, used, total);
    }
    else if (alertLevel == 1 && alertLevel != lastAlertLevel)
    {
        logWarnf("[Storage] WARNING: Storage %.1f%% full (%llu/%llu bytes)\n",
                 pct, used, total);
    }
    else if (lastAlertLevel >= 1 && alertLevel == 0)
    {
        logInfof("[Storage] Capacity recovered: %.1f%% full (%llu/%llu bytes)\n",
                 pct, used, total);
    }

    lastAlertLevel = alertLevel;
}

bool isRecordingModePsram()
{
    // If recordToSdCard is false, always use PSRAM
    if (!appSettings.sdCard.recordToSdCard)
    {
        return true;
    }
    // If recordToSdCard is true, use SD card if available, otherwise PSRAM (fallback)
    return !isStorageModeSdCard();
}

bool isRecordingModeSdCard()
{
    // If recordToSdCard is false, never use SD card
    if (!appSettings.sdCard.recordToSdCard)
    {
        return false;
    }
    // If recordToSdCard is true, use SD card if available
    return isStorageModeSdCard();
}

bool hasSdCardRecordingError()
{
    // Returns true if recordToSdCard is enabled but SD card is not available
    if (!appSettings.sdCard.recordToSdCard)
    {
        return false; // Not an error if SD card recording is disabled
    }
    // Check if SD card is actually available
    return !isStorageModeSdCard();
}

// Maintenance functions removed for reliability

// Device ID (MAC address) functions
namespace
{
    String g_deviceId; // Stores 12-digit uppercase MAC address without colons
    bool g_deviceIdInitialized = false;
    
    String g_sessionId; // Stores 8-digit random session ID
    bool g_sessionIdInitialized = false;
}

const String &getDeviceId()
{
    if (!g_deviceIdInitialized)
    {
        // Fetch MAC address from WiFi
        String mac = WiFi.macAddress();
        if (mac.length() > 0)
        {
            // Remove colons and convert to uppercase
            mac.replace(":", "");
            mac.toUpperCase();
            g_deviceId = mac;
        }
        else
        {
            // Fallback if WiFi not initialized yet
            g_deviceId = "000000000000";
        }
        g_deviceIdInitialized = true;
    }
    return g_deviceId;
}

// Session ID functions
void initializeSessionId()
{
    if (!g_sessionIdInitialized)
    {
        // Generate 8-digit random number (10000000 to 99999999)
        uint32_t randomNum = esp_random() % 90000000 + 10000000;
        g_sessionId = String(randomNum);
        g_sessionIdInitialized = true;
    }
}

const String &getSessionId()
{
    // Ensure session ID is initialized
    if (!g_sessionIdInitialized)
    {
        initializeSessionId();
    }
    return g_sessionId;
}

// Old maintenance/index code removed - all functions are stubs above

String getLatestLogPath()
{
    time_t now = 0;
    time(&now);
    if (!isEpochValid(now))
    {
        return String(); // Time not synced
    }

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    
    char path[64];
    std::snprintf(path, sizeof(path), "/logs/%04d/%02d/%04d-%02d-%02d-LOG.txt",
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_mday);
    return String(path);
}

// WiFi TX power mapping function
// Maps user-facing level (1-10) to ESP32 TX power value (in 0.25 dBm units)
// Returns ESP32 TX power value: 8,20,34,44,52,60,68,74,78,84 for levels 1-10
uint8_t mapWifiTxPowerLevel(uint8_t level)
{
    // Clamp level to valid range (1-10)
    if (level < 1)
    {
        level = 1;
    }
    else if (level > 10)
    {
        level = 10;
    }
    
    // Map level to ESP32 TX power values (in 0.25 dBm units)
    // Level 1 = 8 (2 dBm), Level 10 = 84 (21 dBm)
    static const uint8_t txPowerMap[] = {
        8,   // Level 1: 2 dBm
        20,  // Level 2: 5 dBm
        34,  // Level 3: 8.5 dBm
        44,  // Level 4: 11 dBm
        52,  // Level 5: 13 dBm
        60,  // Level 6: 15 dBm
        68,  // Level 7: 17 dBm
        74,  // Level 8: 18.5 dBm
        78,  // Level 9: 19.5 dBm
        84   // Level 10: 21 dBm (highest)
    };
    
    return txPowerMap[level - 1]; // level is 1-10, array index is 0-9
}

// Atomic serial write function for JSON messages
// Ensures complete messages are sent without interruption
// Uses mutex protection and atomic write to prevent message fragmentation
void serialWriteJsonAtomic(const String &jsonMessage)
{
    if (jsonMessage.length() == 0)
    {
        return;
    }
    
    // Get serial mutex to prevent interleaving with other serial output
    SemaphoreHandle_t serialMutex = settings_getSerialMutex();
    bool mutexAcquired = false;
    
    if (serialMutex != nullptr)
    {
        // Wait up to 5 seconds to acquire mutex
        mutexAcquired = (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(5000)) == pdTRUE);
    }
    
    // Only send if we acquired mutex or mutex is not available (fallback)
    if (mutexAcquired || serialMutex == nullptr)
    {
        // Add newline if not present
        String messageToSend = jsonMessage;
        if (!messageToSend.endsWith("\n") && !messageToSend.endsWith("\r\n"))
        {
            messageToSend += "\n";
        }
        
        // Send entire message atomically using write() instead of print()
        // This ensures the message is sent as a single unit
        // Note: If Serial buffer is full, write() will block or drop bytes
        // The ESP32 Serial library handles buffer overflow internally
        Serial.write(reinterpret_cast<const uint8_t*>(messageToSend.c_str()), messageToSend.length());
        
        // Flush to ensure message is fully transmitted
        Serial.flush();
    }
    
    // Release mutex if we acquired it
    if (mutexAcquired && serialMutex != nullptr)
    {
        xSemaphoreGive(serialMutex);
    }
}

// storage_scanLast7DaysForMissingUploads removed - using filesystem-based queue in /pending
// The upload task now scans /pending directly, no need for this function

// Delete oldest folder if SD card is more than 80% full
// Returns true if a folder was deleted
bool storage_deleteOldestFolderIfNeeded()
{
    if (!isStorageModeSdCard())
    {
        return false; // Only works with SD card
    }
    
    storage_updateHealthMetrics();
    extern StorageHealthMetrics g_storageHealthMetrics;
    
    if (g_storageHealthMetrics.utilizationPercent < 80.0f)
    {
        return false; // Not full enough
    }
    
    if (!timeKeeper().timeIsValid())
    {
        return false;
    }
    
    // Find oldest folder in /inbox/YYYY/MM/DD structure
    String oldestFolderPath;
    time_t oldestEpoch = 0;
    bool foundOldest = false;
    
    // Walk /inbox directory to find oldest YYYY/MM/DD folder
    std::function<void(const String &)> findOldest = [&](const String &dirPath)
    {
        File dir = SD_MMC.open(dirPath);
        if (!dir)
        {
            return;
        }
        
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry)
            {
                break;
            }
            
            if (entry.isDirectory())
            {
                String child = String(entry.name());
                if (!child.startsWith("/"))
                {
                    child = String(dirPath) + "/" + child;
                }
                
                // Check if this is a date folder: /inbox/YYYY/MM/DD
                int y = 0, m = 0, d = 0;
                if (sscanf(child.c_str(), "/inbox/%d/%d/%d", &y, &m, &d) == 3)
                {
                    struct tm folderDate = {};
                    folderDate.tm_year = y - 1900;
                    folderDate.tm_mon = m - 1;
                    folderDate.tm_mday = d;
                    folderDate.tm_hour = 0;
                    folderDate.tm_min = 0;
                    folderDate.tm_sec = 0;
                    time_t folderEpoch = mktime(&folderDate);
                    
                    if (isEpochValid(folderEpoch))
                    {
                        if (!foundOldest || folderEpoch < oldestEpoch)
                        {
                            oldestEpoch = folderEpoch;
                            oldestFolderPath = child;
                            foundOldest = true;
                        }
                    }
                }
                else
                {
                    // Recurse into subdirectories
                    findOldest(child);
                }
            }
            
            entry.close();
        }
        
        dir.close();
    };
    
    if (SD_MMC.exists("/inbox"))
    {
        findOldest("/inbox");
    }
    
    if (!foundOldest || oldestFolderPath.isEmpty())
    {
        return false;
    }
    
    // Delete the oldest folder and all its contents
    // Recursively delete folder contents
    std::function<bool(const String &)> deleteFolderRecursive = [&](const String &folderPath) -> bool
    {
        File dir = SD_MMC.open(folderPath);
        if (!dir)
        {
            return false;
        }
        
        bool success = true;
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry)
            {
                break;
            }
            
            String entryPath = String(entry.name());
            if (!entryPath.startsWith("/"))
            {
                entryPath = String(folderPath) + "/" + entryPath;
            }
            
            if (entry.isDirectory())
            {
                if (!deleteFolderRecursive(entryPath))
                {
                    success = false;
                }
                if (!SD_MMC.rmdir(entryPath))
                {
                    success = false;
                }
            }
            else
            {
                if (!SD_MMC.remove(entryPath))
                {
                    success = false;
                }
            }
            
            entry.close();
        }
        
        dir.close();
        return success;
    };
    
    bool deleted = deleteFolderRecursive(oldestFolderPath);
    if (deleted)
    {
        // Try to remove the folder itself (may fail if not empty, but that's okay)
        SD_MMC.rmdir(oldestFolderPath);
        
        // Update storage metrics after deletion
        storage_updateHealthMetrics();
        
        // Update monthly and yearly summaries for the affected month (since day was deleted)
        int delYear = 0, delMonth = 0, delDay = 0;
        if (sscanf(oldestFolderPath.c_str(), "/inbox/%d/%d/%d", &delYear, &delMonth, &delDay) == 3)
        {
            storage_updateMonthlySummary(delYear, delMonth);
            storage_updateYearlySummary(delYear);
            recordings_deleteSummaryForDay(delYear, delMonth, delDay);
        }
        
        return true;
    }
    else
    {
        return false;
    }
}

// Check if a day folder has a valid index file
// Returns true if the index file exists
bool storage_hasDayIndexFile(int year, int month, int day)
{
    if (!isStorageModeSdCard())
    {
        return false;
    }
    
    char indexPath[64];
    std::snprintf(indexPath, sizeof(indexPath), "/inbox/%04d/%02d/%02d/%04d-%02d-%02d-Index.txt",
                  year, month, day, year, month, day);
    
    return SD_MMC.exists(indexPath);
}

// Create an index file for a completed day folder
// Only creates index if all .wav files have corresponding .uploaded markers
// Returns true if index was created successfully
bool storage_createDayIndexFile(int year, int month, int day)
{
    if (!isStorageModeSdCard())
    {
        return false;
    }
    
    char folderPath[32];
    std::snprintf(folderPath, sizeof(folderPath), "/inbox/%04d/%02d/%02d", year, month, day);
    
    if (!SD_MMC.exists(folderPath))
    {
        return false; // Folder doesn't exist
    }
    
    // Check if index already exists
    if (storage_hasDayIndexFile(year, month, day))
    {
        return true; // Already has index
    }
    
    // Scan folder for .wav files and check if all have .uploaded markers
    File dir = SD_MMC.open(folderPath);
    if (!dir)
    {
        return false;
    }
    
    // Collect file information
    struct FileInfo {
        char filename[64];
        size_t fileSize;
        bool hasUploadedMarker;
    };
    
    constexpr size_t kMaxFilesPerDay = 500; // Max recordings per day to track
    FileInfo* files = new FileInfo[kMaxFilesPerDay];
    if (!files)
    {
        dir.close();
        return false; // Memory allocation failed
    }
    
    size_t fileCount = 0;
    size_t totalSize = 0;
    size_t uploadedCount = 0;
    bool allUploaded = true;
    
    while (fileCount < kMaxFilesPerDay)
    {
        File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        
        if (!entry.isDirectory())
        {
            String fname = String(entry.name());
            
            // Only consider .wav files (not .uploaded markers or index files)
            if (fname.endsWith(".wav"))
            {
                // Store file info
                fname.toCharArray(files[fileCount].filename, sizeof(files[fileCount].filename));
                files[fileCount].fileSize = entry.size();
                
                // Check for .uploaded marker
                String fullPath = String(folderPath) + "/" + fname;
                String markerPath = fullPath + ".uploaded";
                files[fileCount].hasUploadedMarker = SD_MMC.exists(markerPath);
                
                if (files[fileCount].hasUploadedMarker)
                {
                    uploadedCount++;
                }
                else
                {
                    allUploaded = false;
                }
                
                totalSize += files[fileCount].fileSize;
                fileCount++;
            }
        }
        
        entry.close();
    }
    
    dir.close();
    
    // Only create index if ALL files have been uploaded
    if (!allUploaded || fileCount == 0)
    {
        delete[] files;
        return false; // Not all files uploaded yet
    }
    
    // Create the index file
    char indexPath[64];
    std::snprintf(indexPath, sizeof(indexPath), "/inbox/%04d/%02d/%02d/%04d-%02d-%02d-Index.txt",
                  year, month, day, year, month, day);
    
    File indexFile = SD_MMC.open(indexPath, FILE_WRITE);
    if (!indexFile)
    {
        delete[] files;
        return false;
    }
    
    // Write header
    indexFile.println("# Day Index File");
    indexFile.printf("# Date: %04d-%02d-%02d\n", year, month, day);
    indexFile.printf("# Device: %s\n", getDeviceId().c_str());
    indexFile.printf("# Total Files: %u\n", static_cast<unsigned>(fileCount));
    indexFile.printf("# Total Size: %u bytes\n", static_cast<unsigned>(totalSize));
    indexFile.printf("# All Uploaded: true\n");
    indexFile.println("#");
    indexFile.println("# Format: filename,size_bytes,uploaded");
    indexFile.println("#");
    
    // Write file entries
    for (size_t i = 0; i < fileCount; ++i)
    {
        indexFile.printf("%s,%u,%s\n", 
                        files[i].filename, 
                        static_cast<unsigned>(files[i].fileSize),
                        files[i].hasUploadedMarker ? "yes" : "no");
    }
    
    indexFile.close();
    delete[] files;
    
    return true;
}

// Monthly Summary Implementation
// Creates/updates /inbox/YYYY/MM/summary.json files by aggregating daily index.json files

bool storage_updateMonthlySummary(int year, int month)
{
    if (!isStorageModeSdCard() || !timeKeeper().timeIsValid())
    {
        return false;
    }
    
    // Build month directory path
    char monthDirPath[32];
    std::snprintf(monthDirPath, sizeof(monthDirPath), "/inbox/%04d/%02d", year, month);
    
    if (!SD_MMC.exists(monthDirPath))
    {
        return false; // Month folder doesn't exist
    }
    
    // Summary data
    uint32_t totalFiles = 0;
    uint64_t totalSizeBytes = 0;
    uint64_t totalDurationMs = 0;
    uint32_t daysWithRecordings = 0;
    
    // Per-day data (max 31 days)
    struct DaySummary {
        int day;
        uint32_t fileCount;
        uint64_t sizeBytes;
        uint64_t durationMs;
    } days[31];
    std::memset(days, 0, sizeof(days));
    
    // Iterate through day folders (01-31)
    for (int day = 1; day <= 31; day++)
    {
        char dayDirPath[40];
        std::snprintf(dayDirPath, sizeof(dayDirPath), "/inbox/%04d/%02d/%02d", year, month, day);
        
        if (!SD_MMC.exists(dayDirPath))
        {
            continue;
        }
        
        // Check for index.json in this day folder
        char indexPath[64];
        std::snprintf(indexPath, sizeof(indexPath), "%s/index.json", dayDirPath);
        
        if (!SD_MMC.exists(indexPath))
        {
            continue; // No index file for this day
        }
        
        // Read and parse index.json (JSONL format - one JSON per line)
        File indexFile = SD_MMC.open(indexPath, FILE_READ);
        if (!indexFile)
        {
            continue;
        }
        
        uint32_t dayFileCount = 0;
        uint64_t daySizeBytes = 0;
        uint64_t dayDurationMs = 0;
        
        // Read line by line
        while (indexFile.available())
        {
            String line = indexFile.readStringUntil('\n');
            line.trim();
            if (line.isEmpty())
            {
                continue;
            }
            
            // Parse JSON line
            StaticJsonDocument<1024> doc;
            DeserializationError error = deserializeJson(doc, line);
            if (error)
            {
                continue; // Skip malformed lines
            }
            
            // Extract recorder data
            if (doc.containsKey("recorder"))
            {
                JsonObject recorder = doc["recorder"];
                dayFileCount++;
                
                if (recorder.containsKey("size"))
                {
                    daySizeBytes += recorder["size"].as<uint64_t>();
                }
                if (recorder.containsKey("durationMs"))
                {
                    dayDurationMs += recorder["durationMs"].as<uint64_t>();
                }
            }
        }
        
        indexFile.close();
        
        if (dayFileCount > 0)
        {
            days[day - 1].day = day;
            days[day - 1].fileCount = dayFileCount;
            days[day - 1].sizeBytes = daySizeBytes;
            days[day - 1].durationMs = dayDurationMs;
            
            totalFiles += dayFileCount;
            totalSizeBytes += daySizeBytes;
            totalDurationMs += dayDurationMs;
            daysWithRecordings++;
        }
    }
    
    // Create summary JSON
    DynamicJsonDocument summaryDoc(4096);
    
    summaryDoc["year"] = year;
    summaryDoc["month"] = month;
    summaryDoc["deviceId"] = getDeviceId();
    summaryDoc["totalFiles"] = totalFiles;
    summaryDoc["totalSizeBytes"] = totalSizeBytes;
    summaryDoc["totalDurationMs"] = totalDurationMs;
    summaryDoc["totalDurationSec"] = totalDurationMs / 1000;
    summaryDoc["daysWithRecordings"] = daysWithRecordings;
    
    // Add per-day breakdown
    JsonArray daysArray = summaryDoc.createNestedArray("days");
    for (int i = 0; i < 31; i++)
    {
        if (days[i].fileCount > 0)
        {
            JsonObject dayObj = daysArray.createNestedObject();
            dayObj["day"] = days[i].day;
            dayObj["fileCount"] = days[i].fileCount;
            dayObj["sizeBytes"] = days[i].sizeBytes;
            dayObj["durationMs"] = days[i].durationMs;
            dayObj["durationSec"] = days[i].durationMs / 1000;
        }
    }
    
    // Add generation timestamp
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    char timestampBuf[32];
    strftime(timestampBuf, sizeof(timestampBuf), "%Y-%m-%dT%H:%M:%S", timeInfo);
    summaryDoc["generatedAt"] = timestampBuf;
    summaryDoc["generatedAtEpoch"] = static_cast<unsigned long>(now);
    
    // Write summary file
    char summaryPath[48];
    std::snprintf(summaryPath, sizeof(summaryPath), "/inbox/%04d/%02d/summary.json", year, month);
    
    File summaryFile = SD_MMC.open(summaryPath, FILE_WRITE);
    if (!summaryFile)
    {
        logErrorf("[Storage] Failed to create monthly summary: %s\n", summaryPath);
        return false;
    }
    
    serializeJsonPretty(summaryDoc, summaryFile);
    summaryFile.close();
    
    return true;
}

// Yearly Summary Implementation
// Creates/updates /inbox/YYYY/summary.json files by aggregating monthly summary.json files

bool storage_updateYearlySummary(int year)
{
    if (!isStorageModeSdCard() || !timeKeeper().timeIsValid())
    {
        return false;
    }
    
    // Build year directory path
    char yearDirPath[24];
    std::snprintf(yearDirPath, sizeof(yearDirPath), "/inbox/%04d", year);
    
    if (!SD_MMC.exists(yearDirPath))
    {
        return false; // Year folder doesn't exist
    }
    
    // Summary data
    uint32_t totalFiles = 0;
    uint64_t totalSizeBytes = 0;
    uint64_t totalDurationMs = 0;
    uint32_t monthsWithRecordings = 0;
    uint32_t totalDaysWithRecordings = 0;
    
    // Per-month data (12 months)
    struct MonthSummary {
        int month;
        uint32_t fileCount;
        uint64_t sizeBytes;
        uint64_t durationMs;
        uint32_t daysWithRecordings;
    } months[12];
    std::memset(months, 0, sizeof(months));
    
    // Iterate through month folders (01-12)
    for (int month = 1; month <= 12; month++)
    {
        // Feed watchdog periodically during long loop (every 3 months = ~25% progress)
        if (month % 3 == 1)
        {
            esp_task_wdt_reset();
        }
        
        char monthSummaryPath[48];
        std::snprintf(monthSummaryPath, sizeof(monthSummaryPath), "/inbox/%04d/%02d/summary.json", year, month);
        
        if (!SD_MMC.exists(monthSummaryPath))
        {
            continue; // No summary for this month
        }
        
        // Read and parse monthly summary.json
        File summaryFile = SD_MMC.open(monthSummaryPath, FILE_READ);
        if (!summaryFile)
        {
            continue;
        }
        
        // Read file content
        String content = summaryFile.readString();
        summaryFile.close();
        
        // Parse JSON
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, content);
        if (error)
        {
            continue; // Skip malformed files
        }
        
        // Extract data from monthly summary
        uint32_t monthFileCount = doc["totalFiles"] | 0;
        uint64_t monthSizeBytes = doc["totalSizeBytes"] | 0ULL;
        uint64_t monthDurationMs = doc["totalDurationMs"] | 0ULL;
        uint32_t monthDays = doc["daysWithRecordings"] | 0;
        
        if (monthFileCount > 0)
        {
            months[month - 1].month = month;
            months[month - 1].fileCount = monthFileCount;
            months[month - 1].sizeBytes = monthSizeBytes;
            months[month - 1].durationMs = monthDurationMs;
            months[month - 1].daysWithRecordings = monthDays;
            
            totalFiles += monthFileCount;
            totalSizeBytes += monthSizeBytes;
            totalDurationMs += monthDurationMs;
            totalDaysWithRecordings += monthDays;
            monthsWithRecordings++;
        }
    }
    
    // Feed watchdog before JSON creation and file write operations
    esp_task_wdt_reset();
    
    // Create yearly summary JSON
    DynamicJsonDocument summaryDoc(4096);
    
    summaryDoc["year"] = year;
    summaryDoc["deviceId"] = getDeviceId();
    summaryDoc["totalFiles"] = totalFiles;
    summaryDoc["totalSizeBytes"] = totalSizeBytes;
    summaryDoc["totalDurationMs"] = totalDurationMs;
    summaryDoc["totalDurationSec"] = totalDurationMs / 1000;
    summaryDoc["totalDurationHours"] = totalDurationMs / 3600000;
    summaryDoc["monthsWithRecordings"] = monthsWithRecordings;
    summaryDoc["totalDaysWithRecordings"] = totalDaysWithRecordings;
    
    // Add per-month breakdown
    JsonArray monthsArray = summaryDoc.createNestedArray("months");
    for (int i = 0; i < 12; i++)
    {
        if (months[i].fileCount > 0)
        {
            JsonObject monthObj = monthsArray.createNestedObject();
            monthObj["month"] = months[i].month;
            monthObj["fileCount"] = months[i].fileCount;
            monthObj["sizeBytes"] = months[i].sizeBytes;
            monthObj["durationMs"] = months[i].durationMs;
            monthObj["durationSec"] = months[i].durationMs / 1000;
            monthObj["daysWithRecordings"] = months[i].daysWithRecordings;
        }
    }
    
    // Add generation timestamp
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    char timestampBuf[32];
    strftime(timestampBuf, sizeof(timestampBuf), "%Y-%m-%dT%H:%M:%S", timeInfo);
    summaryDoc["generatedAt"] = timestampBuf;
    summaryDoc["generatedAtEpoch"] = static_cast<unsigned long>(now);
    
    // Write summary file
    char summaryPath[32];
    std::snprintf(summaryPath, sizeof(summaryPath), "/inbox/%04d/summary.json", year);
    
    File summaryFile = SD_MMC.open(summaryPath, FILE_WRITE);
    if (!summaryFile)
    {
        logErrorf("[Storage] Failed to create yearly summary: %s\n", summaryPath);
        return false;
    }
    
    serializeJsonPretty(summaryDoc, summaryFile);
    summaryFile.close();
    
    return true;
}

// Run nightly summary update
// Updates current month and year, and previous month/year if needed
void storage_runNightlySummaryUpdate()
{
    if (!isStorageModeSdCard() || !timeKeeper().timeIsValid())
    {
        return;
    }
    
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    
    int currentYear = timeInfo->tm_year + 1900;
    int currentMonth = timeInfo->tm_mon + 1;
    int currentDay = timeInfo->tm_mday;
    
    // Always update current month and year
    storage_updateMonthlySummary(currentYear, currentMonth);
    storage_updateYearlySummary(currentYear);
    
    // If we're in the first 3 days of the month, also update previous month
    if (currentDay <= 3)
    {
        int prevMonth = currentMonth - 1;
        int prevYear = currentYear;
        if (prevMonth < 1)
        {
            prevMonth = 12;
            prevYear--;
            // Also update previous year summary if we crossed into new year
            storage_updateYearlySummary(prevYear);
        }
        storage_updateMonthlySummary(prevYear, prevMonth);
    }
}

bool inbox_canonicalizePath(const String &input, String &outPath)
{
    String s = input;
    s.trim();
    if (s.length() == 0)
    {
        return false;
    }
    while (s.indexOf("//") >= 0)
    {
        s.replace("//", "/");
    }
    if (!s.startsWith("/"))
    {
        s = "/" + s;
    }
    if (!s.startsWith("/inbox"))
    {
        return false;
    }
    if (s.indexOf("..") >= 0)
    {
        return false;
    }

    String accum;
    int start = 1;
    while (start <= static_cast<int>(s.length()))
    {
        int next = s.indexOf('/', start);
        String part = (next < 0) ? s.substring(start) : s.substring(start, next);
        if (part.length() == 0)
        {
            start = (next < 0) ? s.length() + 1 : next + 1;
            continue;
        }
        if (part == "..")
        {
            return false;
        }
        accum += "/" + part;
        start = (next < 0) ? s.length() + 1 : next + 1;
    }
    if (accum.length() == 0)
    {
        outPath = "/inbox";
        return true;
    }
    outPath = accum;
    return true;
}

bool inbox_isDayFolderPath(const String &canonicalPath)
{
    if (!canonicalPath.startsWith("/inbox"))
    {
        return false;
    }
    String rest = canonicalPath.length() > 6 ? canonicalPath.substring(6) : "";
    while (rest.length() && rest.charAt(0) == '/')
    {
        rest = rest.substring(1);
    }
    if (rest.length() == 0)
    {
        return false;
    }
    int c1 = rest.indexOf('/');
    if (c1 < 0)
    {
        return false;
    }
    int c2 = rest.indexOf('/', c1 + 1);
    if (c2 < 0)
    {
        return false;
    }
    if (rest.indexOf('/', c2 + 1) >= 0)
    {
        return false;
    }
    return true;
}

bool inbox_parseWavBasenameUtcEpoch(const String &basename, time_t &outUtc)
{
    String lower = basename;
    lower.toLowerCase();
    if (!lower.endsWith(".wav"))
    {
        return false;
    }
    String base = basename.substring(0, basename.length() - 4);
    if (base.length() != 19)
    {
        return false;
    }
    if (base.charAt(4) != '-' || base.charAt(7) != '-' || base.charAt(10) != '-' || base.charAt(13) != '-' ||
        base.charAt(16) != '-')
    {
        return false;
    }
    String iso = base.substring(0, 10) + "T" + base.substring(11, 13) + ":" + base.substring(14, 16) + ":" +
                 base.substring(17, 19);
    long microseconds = 0;
    if (!parseIsoTimestampToEpoch(iso, outUtc, microseconds))
    {
        return false;
    }
    return isEpochValid(outUtc);
}

bool storage_ensureDirectoryPath(const char *dirPath)
{
    if (dirPath == nullptr || !isStorageModeSdCard())
    {
        return false;
    }
    String path = dirPath;
    String currentPath = "";

    int start = 0;
    if (path.startsWith("/"))
    {
        currentPath = "/";
        start = 1;
    }

    while (start < static_cast<int>(path.length()))
    {
        int nextSlash = path.indexOf('/', start);
        if (nextSlash < 0)
        {
            nextSlash = path.length();
        }

        String segment = path.substring(start, nextSlash);
        if (segment.length() > 0)
        {
            if (currentPath.length() > 1)
            {
                currentPath += "/";
            }
            currentPath += segment;

            if (!SD_MMC.exists(currentPath.c_str()))
            {
                if (!SD_MMC.mkdir(currentPath.c_str()))
                {
                    logErrorf("[Storage] Failed to create directory: %s\n", currentPath.c_str());
                    return false;
                }
            }
        }
        start = nextSlash + 1;
    }
    return true;
}

bool pendingWavToPredictedInboxPath(const String &pendingWavPath, String &outInboxPath)
{
    if (!pendingWavPath.startsWith("/pending/"))
    {
        return false;
    }
    const int lastSlash = pendingWavPath.lastIndexOf('/');
    const String filename =
        (lastSlash >= 0) ? pendingWavPath.substring(lastSlash + 1) : pendingWavPath;
    if (filename.length() < 19)
    {
        return false;
    }
    const String year = filename.substring(0, 4);
    const String month = filename.substring(5, 7);
    const String day = filename.substring(8, 10);
    outInboxPath = "/inbox/" + year + "/" + month + "/" + day + "/" + filename;
    return true;
}

static bool recordings_summaryPathForInboxWav(const String &inboxWavPath, char *outSummaryPath, size_t outLen)
{
    if (outSummaryPath == nullptr || outLen < 64)
    {
        return false;
    }
    int y = 0, m = 0, d = 0;
    if (sscanf(inboxWavPath.c_str(), "/inbox/%d/%d/%d/", &y, &m, &d) != 3)
    {
        return false;
    }
    std::snprintf(outSummaryPath, outLen, "/recordings/%04d/%02d/%02d/summary.json", y, m, d);
    return true;
}

bool recordings_appendSummaryLine(const RecordingsSummaryLine &line)
{
    if (!isStorageModeSdCard() || !isRecordingModeSdCard() || line.inboxPath.isEmpty())
    {
        return false;
    }
    char summaryPath[72];
    if (!recordings_summaryPathForInboxWav(line.inboxPath, summaryPath, sizeof(summaryPath)))
    {
        return false;
    }
    String dirPath = String(summaryPath);
    const int lastS = dirPath.lastIndexOf('/');
    if (lastS <= 0)
    {
        return false;
    }
    dirPath = dirPath.substring(0, lastS);
    if (!storage_ensureDirectoryPath(dirPath.c_str()))
    {
        return false;
    }

    StaticJsonDocument<1024> doc;
    if (line.pendingPath.length() > 0)
    {
        doc["pendingPath"] = line.pendingPath;
    }
    doc["inboxPath"] = line.inboxPath;
    doc["path"] = line.inboxPath;
    doc["durationMs"] = line.durationMs;
    if (line.endReason.length() > 0)
    {
        doc["endReason"] = line.endReason;
    }
    if (line.peakDb > -120.0f)
    {
        doc["peakDb"] = line.peakDb;
    }
    doc["sizeBytes"] = static_cast<uint64_t>(line.sizeBytes);
    doc["sampleRate"] = line.sampleRate;
    doc["deviceId"] = getDeviceId();

    File f = SD_MMC.open(summaryPath, FILE_APPEND);
    if (!f)
    {
        logErrorf("[Recordings] Failed to open summary for append: %s\n", summaryPath);
        return false;
    }
    serializeJson(doc, f);
    f.println();
    f.close();
    return true;
}

void recordings_deleteSummaryForDay(int year, int month, int day)
{
    if (!isStorageModeSdCard())
    {
        return;
    }
    char summaryPath[72];
    std::snprintf(summaryPath, sizeof(summaryPath), "/recordings/%04d/%02d/%02d/summary.json", year, month, day);
    if (SD_MMC.exists(summaryPath))
    {
        SD_MMC.remove(summaryPath);
    }
    char dayDir[48];
    std::snprintf(dayDir, sizeof(dayDir), "/recordings/%04d/%02d/%02d", year, month, day);
    if (SD_MMC.exists(dayDir))
    {
        File dir = SD_MMC.open(dayDir);
        if (dir && dir.isDirectory())
        {
            bool empty = true;
            while (true)
            {
                File e = dir.openNextFile();
                if (!e)
                {
                    break;
                }
                empty = false;
                e.close();
            }
            dir.close();
            if (empty)
            {
                SD_MMC.rmdir(dayDir);
            }
        }
        else if (dir)
        {
            dir.close();
        }
    }
}

static void pruneRecordingsRecursive(const String &dirPath)
{
    File dir = SD_MMC.open(dirPath);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }
    if (recordings_isIgnoredEpochFolderPath(dirPath))
    {
        dir.close();
        return;
    }
    int dayY = 0, dayM = 0, dayD = 0;
    const bool isDayFolder = (sscanf(dirPath.c_str(), "/recordings/%d/%d/%d", &dayY, &dayM, &dayD) == 3);
    if (isDayFolder)
    {
        char inboxDay[48];
        std::snprintf(inboxDay, sizeof(inboxDay), "/inbox/%04d/%02d/%02d", dayY, dayM, dayD);
        char summaryPath[80];
        std::snprintf(summaryPath, sizeof(summaryPath), "%s/summary.json", dirPath.c_str());
        if (SD_MMC.exists(summaryPath) && !SD_MMC.exists(inboxDay))
        {
            SD_MMC.remove(summaryPath);
        }
    }
    while (true)
    {
        File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        String child = String(entry.name());
        if (!child.startsWith("/"))
        {
            child = String(dirPath) + "/" + child;
        }
        if (entry.isDirectory())
        {
            pruneRecordingsRecursive(child);
        }
        entry.close();
        esp_task_wdt_reset();
    }
    dir.close();
}

void storage_pruneRecordingsSummariesWithoutInbox()
{
    if (!isStorageModeSdCard())
    {
        return;
    }
    if (!SD_MMC.exists("/recordings"))
    {
        return;
    }
    pruneRecordingsRecursive("/recordings");
}

bool recordings_isIgnoredEpochFolderPath(const String &canonicalPath)
{
    return canonicalPath == "/recordings/1970" || canonicalPath.startsWith("/recordings/1970/");
}

bool recordings_canonicalizePath(const String &input, String &outPath)
{
    String s = input;
    s.trim();
    if (s.length() == 0)
    {
        return false;
    }
    while (s.indexOf("//") >= 0)
    {
        s.replace("//", "/");
    }
    if (!s.startsWith("/"))
    {
        s = "/" + s;
    }
    if (!s.startsWith("/recordings"))
    {
        return false;
    }
    if (s.indexOf("..") >= 0)
    {
        return false;
    }

    String accum;
    int start = 1;
    while (start <= static_cast<int>(s.length()))
    {
        int next = s.indexOf('/', start);
        String part = (next < 0) ? s.substring(start) : s.substring(start, next);
        if (part.length() == 0)
        {
            start = (next < 0) ? s.length() + 1 : next + 1;
            continue;
        }
        if (part == "..")
        {
            return false;
        }
        accum += "/" + part;
        start = (next < 0) ? s.length() + 1 : next + 1;
    }
    if (accum.length() == 0)
    {
        outPath = "/recordings";
        return true;
    }
    outPath = accum;
    return true;
}

bool recordings_isDayFolderPath(const String &canonicalPath)
{
    if (!canonicalPath.startsWith("/recordings"))
    {
        return false;
    }
    String rest = canonicalPath.length() > 12 ? canonicalPath.substring(12) : "";
    while (rest.length() && rest.charAt(0) == '/')
    {
        rest = rest.substring(1);
    }
    if (rest.length() == 0)
    {
        return false;
    }
    int c1 = rest.indexOf('/');
    if (c1 < 0)
    {
        return false;
    }
    int c2 = rest.indexOf('/', c1 + 1);
    if (c2 < 0)
    {
        return false;
    }
    if (rest.indexOf('/', c2 + 1) >= 0)
    {
        return false;
    }
    return true;
}
