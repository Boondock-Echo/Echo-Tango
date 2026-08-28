#pragma once

#include <Arduino.h>

// Log level enumeration
enum class LogLevel {
    FATAL,   // A fatal error that affects functioning
    ERROR,   // An error occurred
    WARNING, // A warning that can cause issues
    INFO,    // Information related to activity and events
    DEBUG,   // Debugging information
    EVENT    // Event notifications
};

// Emoji constants for each log level
#define LOG_EMOJI_FATAL   "💀"
#define LOG_EMOJI_ERROR   "❌"
#define LOG_EMOJI_WARNING "⚠️"
#define LOG_EMOJI_INFO    "ℹ️"
#define LOG_EMOJI_DEBUG   "🐛"
#define LOG_EMOJI_EVENT   "⚡"

// Initializes the logging subsystem. Call this once after Serial.begin().
void logger_begin();

// Periodic maintenance call; flushes buffered data when needed.
void logger_tick();

// Forces a flush of any buffered log data to storage.
void logger_flush();
 
void logFatalf(const char *format, ...);
  
void logErrorf(const char *format, ...);
  
void logWarnf(const char *format, ...);
 
void logInfof(const char *format, ...);
 
void logDebugf(const char *format, ...);

void logEventf(const char *format, ...);
   
// Error tracking for web UI
struct ErrorMessage {
    String message;
    String timestamp;
    LogLevel level;
    unsigned long sequenceId;
};

// Get recent error/fatal messages (up to maxCount, returns actual count)
size_t logger_getRecentErrors(ErrorMessage* out, size_t maxCount);

// Get the sequence ID of the last error (for polling detection)
unsigned long logger_getLastErrorSequenceId();