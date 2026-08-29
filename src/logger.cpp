#include "logger.h"

#include "common.h"
#include "main.h"
#include "network.h"

#include <FS.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#ifdef ESP32
#include <esp_heap_caps.h>
#endif

namespace
{
    // PSRAM holds batched log text; FAT flush() is separate and less frequent (less SD metadata wear).
    constexpr size_t kLogSdBufferCapacityPsram = 32768;
    constexpr size_t kLogSdBufferCapacityFallback = 2048;
    constexpr unsigned long kLogRamToFileIntervalMs = 10000;  // push RAM buffer to SD append (no FAT flush)
    constexpr unsigned long kLogFileFatFlushIntervalMs = 15000; // fsync log file
    constexpr size_t kPrintfScratchSize = 512;
    constexpr size_t kMaxStoredErrors = 50; // Store up to 50 recent errors

    SemaphoreHandle_t g_logMutex = nullptr;
    bool g_initialized = false;

    uint8_t *g_logSdBuffer = nullptr;
    size_t g_logSdBufferCapacity = 0;
    static uint8_t g_logSdBufferDramFallback[kLogSdBufferCapacityFallback];
    size_t g_logBufferSize = 0;
    String g_lineBuffer;
    String g_serialLineBuffer;

    File g_logFile;
    uint32_t g_currentDateKey = 0;
    String g_currentLogPath;
    String g_unsyncedLogPath;
    unsigned long g_lastRamWriteToFileMs = 0;
    unsigned long g_lastFatFlushMs = 0;
    bool g_logFileDirty = false;

    // Error tracking
    ErrorMessage g_errorBuffer[kMaxStoredErrors];
    size_t g_errorBufferSize = 0;
    size_t g_errorBufferIndex = 0; // For circular buffer
    unsigned long g_lastErrorSequenceId = 0;

    const char* getLogEmoji(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::FATAL:
                return LOG_EMOJI_FATAL;
            case LogLevel::ERROR:
                return LOG_EMOJI_ERROR;
            case LogLevel::WARNING:
                return LOG_EMOJI_WARNING;
            case LogLevel::INFO:
                return LOG_EMOJI_INFO;
            case LogLevel::DEBUG:
                return LOG_EMOJI_DEBUG;
            case LogLevel::EVENT:
                return LOG_EMOJI_EVENT;
            default:
                return LOG_EMOJI_INFO;
        }
    }

    String buildTimestampPrefix()
    {
        time_t now = 0;
        time(&now);
        if (isEpochValid(now))
        {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);

            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d ",
                          timeinfo.tm_year + 1900,
                          timeinfo.tm_mon + 1,
                          timeinfo.tm_mday,
                          timeinfo.tm_hour,
                          timeinfo.tm_min,
                          timeinfo.tm_sec);
            return String(buffer);
        }

        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "0000-00-00 00:00:00 UNSYNCED %010lu ", static_cast<unsigned long>(millis()));
        return String(buffer);
    }

    bool ensureDirectories(const String &path)
    {
        // Only create directories in SD card mode
        if (!isStorageModeSdCard())
        {
            return false;
        }

        if (path.isEmpty())
        {
            return true;
        }

        String normalized = path;
        if (!normalized.startsWith("/"))
        {
            normalized = String("/") + normalized;
        }

        String accum;
        int start = 0;
        while (start < normalized.length())
        {
            int slash = normalized.indexOf('/', start);
            String part;
            if (slash == -1)
            {
                part = normalized.substring(start);
                start = normalized.length();
            }
            else
            {
                part = normalized.substring(start, slash);
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
    }

    struct DateInfo
    {
        bool valid = false;
        int year = 0;
        int month = 0;
        int day = 0;
    };

    DateInfo currentDate()
    {
        DateInfo info;
        time_t now = 0;
        time(&now);
        if (!isEpochValid(now))
        {
            return info;
        }

        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        info.valid = true;
        info.year = timeinfo.tm_year + 1900;
        info.month = timeinfo.tm_mon + 1;
        info.day = timeinfo.tm_mday;
        return info;
    }

    String buildLogPath(const DateInfo &info)
    {
        if (info.valid)
        {
            char path[64];
            std::snprintf(path, sizeof(path), "/logs/%04d/%02d/%04d-%02d-%02d-LOG.txt",
                          info.year, info.month, info.year, info.month, info.day);
            return String(path);
        }

        if (g_unsyncedLogPath.isEmpty())
        {
            char path[64];
            std::snprintf(path, sizeof(path), "/logs/unsynced/startup-%lu-LOG.txt", static_cast<unsigned long>(millis()));
            g_unsyncedLogPath = String(path);
        }

        return g_unsyncedLogPath;
    }

    bool ensureLogFileLocked()
    {
        // Only write logs to SD card in SD card mode
        // In PSRAM mode, logs are only written to Serial
        if (!isStorageModeSdCard())
        {
            return false;
        }

        if (!ensureStorage())
        {
            return false;
        }

        DateInfo info = currentDate();
        String desiredPath = buildLogPath(info);
        uint32_t desiredKey = info.valid ? static_cast<uint32_t>(info.year * 10000 + info.month * 100 + info.day) : 0;

        const bool rotating = (desiredKey != g_currentDateKey) || (desiredPath != g_currentLogPath);

        if (g_logFile && rotating)
        {
            if (g_lineBuffer.length() > 0)
            {
                String line = buildTimestampPrefix();
                line += getLogEmoji(LogLevel::INFO); // Default to INFO for rotation cases
                line += g_lineBuffer;
                line += '\n';
                g_logFile.write(reinterpret_cast<const uint8_t *>(line.c_str()), static_cast<size_t>(line.length()));
                g_lineBuffer.clear();
            }

            if (g_logBufferSize > 0 && g_logSdBuffer != nullptr)
            {
                g_logFile.write(g_logSdBuffer, g_logBufferSize);
                g_logBufferSize = 0;
                g_logFileDirty = true;
            }

            if (g_logFileDirty)
            {
                g_logFile.flush();
                g_logFileDirty = false;
                g_lastFatFlushMs = millis();
            }
            g_logFile.close();
        }

        if (!g_logFile || rotating)
        {
            g_currentDateKey = desiredKey;
            g_currentLogPath = desiredPath;

            int lastSlash = desiredPath.lastIndexOf('/');
            if (lastSlash > 0)
            {
                String dir = desiredPath.substring(0, lastSlash);
                if (!ensureDirectories(dir))
                {
                    return false;
                }
            }

            g_logFile = SD_MMC.open(desiredPath, FILE_APPEND);
            if (!g_logFile)
            {
                return false;
            }
        }

        return true;
    }

    void flushLogFileFatLocked()
    {
        if (!g_logFile || !g_logFileDirty)
        {
            return;
        }
        g_logFile.flush();
        g_logFileDirty = false;
        g_lastFatFlushMs = millis();
    }

    void writeRamBufferToFileLocked()
    {
        if (g_logBufferSize == 0 || g_logSdBuffer == nullptr)
        {
            return;
        }

        if (!ensureLogFileLocked())
        {
            return;
        }

        (void)g_logFile.write(g_logSdBuffer, g_logBufferSize);
        g_logBufferSize = 0;
        g_logFileDirty = true;
        g_lastRamWriteToFileMs = millis();
    }

    void appendToBufferLocked(const char *data, size_t length)
    {
        if (length == 0)
        {
            return;
        }

        const size_t cap = g_logSdBufferCapacity > 0 ? g_logSdBufferCapacity : kLogSdBufferCapacityFallback;

        if (length > cap)
        {
            if (!ensureLogFileLocked())
            {
                return;
            }

            (void)g_logFile.write(reinterpret_cast<const uint8_t *>(data), length);
            g_logFileDirty = true;
            flushLogFileFatLocked();
            g_lastRamWriteToFileMs = millis();
            return;
        }

        if (g_logBufferSize + length > cap)
        {
            writeRamBufferToFileLocked();
        }
        if (g_logBufferSize + length > cap)
        {
            g_logBufferSize = 0;
        }

        if (g_logSdBuffer == nullptr)
        {
            return;
        }

        memcpy(g_logSdBuffer + g_logBufferSize, data, length);
        g_logBufferSize += length;
    }

    void storeErrorLocked(LogLevel level)
    {
        // Only store FATAL and ERROR level messages
        if (level != LogLevel::FATAL && level != LogLevel::ERROR)
        {
            return;
        }

        // Build the error message
        String timestamp = buildTimestampPrefix();
        timestamp.trim();
        
        // Store in circular buffer
        ErrorMessage& err = g_errorBuffer[g_errorBufferIndex];
        err.message = g_lineBuffer;
        err.timestamp = timestamp;
        err.level = level;
        err.sequenceId = ++g_lastErrorSequenceId;

        g_errorBufferIndex = (g_errorBufferIndex + 1) % kMaxStoredErrors;
        if (g_errorBufferSize < kMaxStoredErrors)
        {
            g_errorBufferSize++;
        }
    }

    void commitLineLocked(LogLevel level = LogLevel::INFO)
    {
        // Store error before committing if it's an error/fatal
        if (level == LogLevel::FATAL || level == LogLevel::ERROR)
        {
            storeErrorLocked(level);
        }

        const char* emoji = getLogEmoji(level);
        String line = buildTimestampPrefix();
        line += emoji;
        line += g_lineBuffer;
        line += '\n';
        appendToBufferLocked(line.c_str(), static_cast<size_t>(line.length()));
        g_lineBuffer.clear();
    }

    void appendTextLocked(const String &text, LogLevel level = LogLevel::INFO)
    {
        if (text.length() == 0)
        {
            return;
        }

        size_t start = 0;
        while (start < static_cast<size_t>(text.length()))
        {
            int newlineIndex = text.indexOf('\n', start);
            if (newlineIndex == -1)
            {
                g_lineBuffer += text.substring(start);
                break;
            }

            g_lineBuffer += text.substring(start, newlineIndex);
            commitLineLocked(level);
            start = static_cast<size_t>(newlineIndex + 1);
        }
    }

    // Send JSON log message (always in JSON format)
    void sendJsonLogMessage(LogLevel level, const String &message)
    {
        // Determine message type based on log level
        const char *type = "info";
        switch (level)
        {
            case LogLevel::FATAL:
                type = "fatal";
                break;
            case LogLevel::ERROR:
                type = "error";
                break;
            case LogLevel::WARNING:
                type = "warning";
                break;
            case LogLevel::INFO:
                type = "info";
                break;
            case LogLevel::DEBUG:
                type = "debug";
                break;
            case LogLevel::EVENT:
                type = "event";
                break;
        }
        
        // Create JSON message (include time as first parameter, then session ID on all log/event messages)
        String timeStr = getFormattedTimeWithTimezone();
        String jsonMsg = "{\"tm\":\"" + timeStr + "\",\"ty\":\"" + String(type) + "\",\"ms\":\"";
        // Escape quotes and backslashes in the message
        for (size_t i = 0; i < message.length(); ++i)
        {
            char c = message.charAt(i);
            if (c == '"' || c == '\\')
            {
                jsonMsg += '\\';
                jsonMsg += c;
            }
            else if (c == '\n')
            {
                jsonMsg += "\\n";
            }
            else if (c == '\r')
            {
                jsonMsg += "\\r";
            }
            else if (c == '\t')
            {
                jsonMsg += "\\t";
            }
            else
            {
                jsonMsg += c;
            }
        }
        jsonMsg += "\",\"mc\":\"" + getDeviceId() + "\",\"si\":\"" + getSessionId() + "\"}";
        
        // Use atomic write to ensure message is sent with newline and flushed
        serialWriteJsonAtomic(jsonMsg);
    }

    void serialWriteWithTimestamp(const String &text, bool newline, LogLevel level = LogLevel::INFO)
    {
        String fullMessage = g_serialLineBuffer + text;
        
        // Check for embedded newlines in the message
        int newlineIdx = fullMessage.indexOf('\n');
        bool hasEmbeddedNewline = (newlineIdx >= 0);
        
        if ((newline || hasEmbeddedNewline) && fullMessage.length() > 0)
        {
            // Strip trailing newlines for cleaner output
            while (fullMessage.endsWith("\n") || fullMessage.endsWith("\r"))
            {
                fullMessage.remove(fullMessage.length() - 1);
            }
            if (fullMessage.length() > 0)
            {
                // Prepend emoji to the message
                const char* emoji = getLogEmoji(level);
                String messageWithEmoji = String(emoji) + fullMessage;
                
                // Check if we're in AP mode (no WiFi credentials) - output plain text
                if (!network_hasAnyWiFiCredentials())
                {
                    // In AP/setup mode - output plain text instead of JSON
                    Serial.println(messageWithEmoji);
                }
                else
                {
                    // Normal mode - output JSON format
                    sendJsonLogMessage(level, messageWithEmoji);
                }
            }
            g_serialLineBuffer.clear();
        }
        else if (text.length() > 0)
        {
            g_serialLineBuffer += text;
        }
    }

    void logWriteToBuffer(const String &text, bool newline, LogLevel level = LogLevel::INFO)
    {
        if (!g_initialized || g_logMutex == nullptr)
        {
            return;
        }

        if (xSemaphoreTake(g_logMutex, portMAX_DELAY) != pdTRUE)
        {
            return;
        }

        appendTextLocked(text, level);
        if (newline)
        {
            commitLineLocked(level);
        }

        xSemaphoreGive(g_logMutex);
    }

    bool shouldLogToSerial(LogLevel level)
    {
        // Check settings to determine if this log level should be sent to serial
        switch (level)
        {
            case LogLevel::FATAL:
                return appSettings.log.serialFatal;
            case LogLevel::ERROR:
                return appSettings.log.serialError;
            case LogLevel::WARNING:
                return appSettings.log.serialWarning;
            case LogLevel::INFO:
                return appSettings.log.serialInfo;
            case LogLevel::DEBUG:
                return appSettings.log.serialDebug;
            case LogLevel::EVENT:
                return appSettings.log.serialEvent;
            default:
                return true; // Default to enabled for unknown levels
        }
    }

    bool shouldLogToFile(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::FATAL:
                return appSettings.log.fileFatal;
            case LogLevel::ERROR:
                return appSettings.log.fileError;
            case LogLevel::WARNING:
                return appSettings.log.fileWarning;
            case LogLevel::INFO:
                return appSettings.log.fileInfo;
            case LogLevel::DEBUG:
                return appSettings.log.fileDebug;
            case LogLevel::EVENT:
                return appSettings.log.fileEvent;
            default:
                return true; // Default to enabled for unknown levels
        }
    }

    void dispatchLog(const String &text, bool newline, bool ignoreSerial, LogLevel level = LogLevel::INFO)
    {
        // Check if we should write to serial (unless explicitly ignored)
        if (!ignoreSerial && shouldLogToSerial(level))
        {
            serialWriteWithTimestamp(text, newline, level);
        }

        // Check if we should write to file
        if (shouldLogToFile(level))
        {
            logWriteToBuffer(text, newline, level);
        }
        
        // Send events for error/warning/fatal levels
        // Skip sending events if this message is about event send failures to prevent feedback loops
        // Check for various patterns that indicate event-related failures
        bool isEventFailureMessage = 
            text.indexOf("Event failed to send") != -1 ||
            text.indexOf("event_type:") != -1 ||
            (text.indexOf("Event API") != -1 && (text.indexOf("returned") != -1 || text.indexOf("error") != -1));
        
        if ((level == LogLevel::FATAL || level == LogLevel::ERROR || level == LogLevel::WARNING) &&
            !isEventFailureMessage)
        {
            String eventType;
            if (level == LogLevel::FATAL)
            {
                eventType = "fatal_error";
            }
            else if (level == LogLevel::ERROR)
            {
                eventType = "error";
            }
            else if (level == LogLevel::WARNING)
            {
                eventType = "warning";
            }
            
            DynamicJsonDocument eventData(512);
            eventData["message"] = text;
            String eventMessage;
            serializeJson(eventData, eventMessage);
            sendEvent(eventType, eventMessage);
        }
    }

    String toString(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%f", value);
        return String(buffer);
    }

} // namespace

void logger_begin()
{
    if (g_initialized)
    {
        return;
    }

    g_logMutex = xSemaphoreCreateMutex();
    if (g_logMutex == nullptr)
    {
        // Can't use logger here, but we should at least try Serial
        Serial.println("[FATAL] Failed to create logger mutex - logging system compromised");
        // Still return, but this is a critical failure
        return;
    }

    if (g_logSdBuffer == nullptr)
    {
        g_logSdBuffer = static_cast<uint8_t *>(heap_caps_malloc(kLogSdBufferCapacityPsram, MALLOC_CAP_SPIRAM));
        if (g_logSdBuffer != nullptr)
        {
            g_logSdBufferCapacity = kLogSdBufferCapacityPsram;
        }
        else
        {
            g_logSdBuffer = g_logSdBufferDramFallback;
            g_logSdBufferCapacity = sizeof(g_logSdBufferDramFallback);
        }
    }

    g_lastRamWriteToFileMs = millis();
    g_lastFatFlushMs = millis();
    g_initialized = true;
    ensureLogFileLocked();
}

void logger_tick()
{
    if (!g_initialized || g_logMutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        return;
    }

    const unsigned long now = millis();
    if (g_logBufferSize > 0 && (now - g_lastRamWriteToFileMs >= kLogRamToFileIntervalMs))
    {
        writeRamBufferToFileLocked();
    }
    if (g_logFileDirty && (now - g_lastFatFlushMs >= kLogFileFatFlushIntervalMs))
    {
        flushLogFileFatLocked();
    }

    xSemaphoreGive(g_logMutex);
}

void logger_flush()
{
    if (!g_initialized || g_logMutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(g_logMutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    writeRamBufferToFileLocked();
    flushLogFileFatLocked();
    xSemaphoreGive(g_logMutex);
}
   

void logFatalf(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::FATAL);
}
 
 

void logErrorf(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::ERROR);
}
 
 

void logWarnf(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::WARNING);
}


void logInfof(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::INFO);
}
 

void logDebugf(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::DEBUG);
}

  
void logEventf(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    char buffer[kPrintfScratchSize];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    buffer[kPrintfScratchSize - 1] = '\0';
    dispatchLog(String(buffer), true, false, LogLevel::EVENT);
}

// Error tracking functions
size_t logger_getRecentErrors(ErrorMessage* out, size_t maxCount)
{
    if (!g_initialized || g_logMutex == nullptr || out == nullptr || maxCount == 0)
    {
        return 0;
    }

    if (xSemaphoreTake(g_logMutex, portMAX_DELAY) != pdTRUE)
    {
        return 0;
    }

    size_t count = 0;
    if (g_errorBufferSize > 0)
    {
        // Copy errors in reverse chronological order (newest first)
        size_t startIndex = (g_errorBufferIndex == 0) ? (kMaxStoredErrors - 1) : (g_errorBufferIndex - 1);
        size_t actualSize = g_errorBufferSize;
        size_t toCopy = (actualSize < maxCount) ? actualSize : maxCount;

        for (size_t i = 0; i < toCopy; ++i)
        {
            size_t idx = (startIndex + kMaxStoredErrors - i) % kMaxStoredErrors;
            out[count] = g_errorBuffer[idx];
            count++;
        }
    }

    xSemaphoreGive(g_logMutex);
    return count;
}

unsigned long logger_getLastErrorSequenceId()
{
    if (!g_initialized || g_logMutex == nullptr)
    {
        return 0;
    }

    if (xSemaphoreTake(g_logMutex, portMAX_DELAY) != pdTRUE)
    {
        return 0;
    }

    unsigned long seqId = g_lastErrorSequenceId;
    xSemaphoreGive(g_logMutex);
    return seqId;
}
