# Logger Functions Usage Analysis

This document provides a comprehensive analysis of all Logger functions and their usage counts across the codebase.

## Summary

| Function | Usage Count | Primary Files |
|----------|-------------|---------------|
| `logger_begin()` | 1 | main.cpp |
| `logger_tick()` | 1 | main.cpp |
| `logger_flush()` | 7 | main.cpp (3), settings.cpp (4) |
| `logFatalf()` | 12 | main.cpp (9), boondock_server.cpp (1), upload_queue.cpp (1), network.cpp (1) |
| `logErrorf()` | 45 | network.cpp (6), boondock_server.cpp (5), main.cpp (5), recorder.cpp (7), common.cpp (6), upload_queue.cpp (5), health.cpp (2), logger.cpp (0) |
| `logWarnf()` | 23 | network.cpp (12), main.cpp (3), recorder.cpp (1), common.cpp (6), upload_queue.cpp (1), boondock_server.cpp (1) |
| `logInfof()` | 17 | network.cpp (1), boondock_server.cpp (2), main.cpp (5), recorder.cpp (1), common.cpp (8) |
| `logDebugf()` | 14 | network.cpp (8), boondock_server.cpp (2), main.cpp (4) |
| `logEventf()` | 5 | network.cpp (4), recorder.cpp (1) |
| `logger_getRecentErrors()` | 1 | settings.cpp |
| `logger_getLastErrorSequenceId()` | 1 | settings.cpp |

**Total Function Calls: 127**

---

## Detailed Breakdown

### 1. `logger_begin()`
**Usage Count: 1**

**Locations:**
- `src/main.cpp:894` - Called during system initialization

**Purpose:** Initializes the logging subsystem. Must be called once after Serial.begin().

---

### 2. `logger_tick()`
**Usage Count: 1**

**Locations:**
- `src/main.cpp:1245` - Called periodically in main loop for maintenance

**Purpose:** Periodic maintenance call; flushes buffered data when needed.

---

### 3. `logger_flush()`
**Usage Count: 7**

**Locations:**
- `src/main.cpp:128` - Called before safety reboot
- `src/main.cpp:137` - Called during memory cleanup
- `src/main.cpp:859` - Called during system shutdown
- `src/settings.cpp:4390` - Called during settings save operation
- `src/settings.cpp:4423` - Called during settings save operation
- `src/settings.cpp:4452` - Called during settings save operation
- `src/settings.cpp:4659` - Called during settings save operation

**Purpose:** Forces a flush of any buffered log data to storage.

---

### 4. `logFatalf()`
**Usage Count: 12**

**Locations:**
- `src/main.cpp:901` - Insufficient heap memory
- `src/main.cpp:909` - Failed to initialize NVS
- `src/main.cpp:915` - Failed to initialize settings
- `src/main.cpp:950` - No storage available
- `src/main.cpp:963` - SD card failed and PSRAM unavailable
- `src/main.cpp:986` - Failed to create record task
- `src/main.cpp:1025` - Failed to create upload task
- `src/main.cpp:1050` - Failed to create serial task
- `src/main.cpp:1073` - Failed to create maintenance task
- `src/main.cpp:1100` - Failed to create web server task
- `src/boondock_server.cpp:609` - Failed to start Access Point
- `src/upload_queue.cpp:109` - Audio recording dropped due to queue full

**Purpose:** Logs fatal errors that affect system functioning.

---

### 5. `logErrorf()`
**Usage Count: 45**

**Breakdown by File:**
- `src/network.cpp` - 6 usages
  - Connection errors, WiFi disconnection, upload failures, endpoint failures
- `src/boondock_server.cpp` - 5 usages
  - SSID verification, AP startup, firmware update errors
- `src/main.cpp` - 5 usages
  - Safety reboot, upload task failures, startup errors
- `src/recorder.cpp` - 7 usages
  - PSRAM buffer allocation, write failures, file operations, storage unavailable
- `src/common.cpp` - 6 usages
  - Storage mount failures, SD card failures, storage full, summary creation failures
- `src/upload_queue.cpp` - 5 usages
  - Directory creation failures, file move failures, index file errors
- `src/health.cpp` - 2 usages
  - Task restart failures, storage critical warnings

**Purpose:** Logs errors that occurred during operation.

---

### 6. `logWarnf()`
**Usage Count: 23**

**Breakdown by File:**
- `src/network.cpp` - 12 usages
  - WiFi disconnection, upload timeouts, empty responses, event API errors, circuit breaker
- `src/main.cpp` - 3 usages
  - Upload task failures, WiFi credentials removed
- `src/recorder.cpp` - 1 usage
  - Storage mode switch
- `src/common.cpp` - 6 usages
  - SD card disabled, write failures, remount attempts, storage warnings
- `src/upload_queue.cpp` - 1 usage
  - Queue full warning
- `src/boondock_server.cpp` - 1 usage
  - Settings push failure

**Purpose:** Logs warnings that can cause issues.

---

### 7. `logInfof()`
**Usage Count: 17**

**Breakdown by File:**
- `src/network.cpp` - 1 usage
  - No WiFi credentials message
- `src/boondock_server.cpp` - 2 usages
  - Settings pushed, AP mode instructions
- `src/main.cpp` - 5 usages
  - Startup messages, WiFi credentials detected, task starting, AP mode messages
- `src/recorder.cpp` - 1 usage
  - Recording summary
- `src/common.cpp` - 8 usages
  - SD card status, storage mode messages, remount attempts

**Purpose:** Logs information related to activity and events.

---

### 8. `logDebugf()`
**Usage Count: 14** (excluding documentation in DEBUG_LOGGING_RECOMMENDATIONS.md)

**Breakdown by File:**
- `src/network.cpp` - 8 usages
  - Upload progress, endpoint attempts, event processing, settings operations
- `src/boondock_server.cpp` - 2 usages
  - Firmware update checks
- `src/main.cpp` - 4 usages
  - Startup initialization messages

**Note:** The `DEBUG_LOGGING_RECOMMENDATIONS.md` file contains 48 additional recommended debug log statements, but these are not currently implemented in the code.

**Purpose:** Logs debugging information for development and troubleshooting.

---

### 9. `logEventf()`
**Usage Count: 5**

**Locations:**
- `src/network.cpp:475` - WiFi connected event
- `src/network.cpp:497` - Lost IP address event
- `src/network.cpp:580` - Connection lost event
- `src/network.cpp:1259` - Upload successful event
- `src/recorder.cpp:504` - Recording event

**Purpose:** Logs event notifications for significant system events.

---

### 10. `logger_getRecentErrors()`
**Usage Count: 1**

**Locations:**
- `src/settings.cpp:5280` - Used to retrieve recent errors for web UI display

**Purpose:** Gets recent error/fatal messages (up to maxCount) for error tracking.

---

### 11. `logger_getLastErrorSequenceId()`
**Usage Count: 1**

**Locations:**
- `src/settings.cpp:5281` - Used to get the last error sequence ID for polling detection

**Purpose:** Gets the sequence ID of the last error for polling-based error detection.

---

## Usage Patterns

### Most Frequently Used Functions
1. **logErrorf()** - 45 calls (35.4% of all logger calls)
2. **logWarnf()** - 23 calls (18.1% of all logger calls)
3. **logInfof()** - 17 calls (13.4% of all logger calls)
4. **logDebugf()** - 14 calls (11.0% of all logger calls)
5. **logFatalf()** - 12 calls (9.4% of all logger calls)

### Files with Most Logger Usage
1. **network.cpp** - ~31 calls (error, warn, info, debug, event)
2. **main.cpp** - ~25 calls (all levels, plus initialization/maintenance)
3. **common.cpp** - ~20 calls (error, warn, info)
4. **recorder.cpp** - ~15 calls (error, warn, info, event)
5. **boondock_server.cpp** - ~10 calls (fatal, error, warn, info, debug)
6. **upload_queue.cpp** - ~7 calls (fatal, error, warn)
7. **settings.cpp** - ~5 calls (flush, getRecentErrors, getLastErrorSequenceId)
8. **health.cpp** - ~2 calls (error)

### Log Level Distribution
- **FATAL**: 12 calls (9.4%)
- **ERROR**: 45 calls (35.4%)
- **WARNING**: 23 calls (18.1%)
- **INFO**: 17 calls (13.4%)
- **DEBUG**: 14 calls (11.0%)
- **EVENT**: 5 calls (3.9%)
- **System Functions**: 11 calls (8.7%)

---

## Recommendations

1. **Error Logging is Comprehensive**: With 45 error log calls, error tracking appears well-covered across critical paths.

2. **Debug Logging Could Be Expanded**: Only 14 debug calls are implemented, but 48 additional recommendations exist in the documentation. Consider implementing more debug logs for better troubleshooting.

3. **Event Logging is Minimal**: Only 5 event logs exist. Consider adding more event logs for significant state changes (e.g., recording started/stopped, settings changed, etc.).

4. **Info Logging is Well-Balanced**: 17 info logs provide good visibility into system operations without being excessive.

5. **Warning Logging is Appropriate**: 23 warning logs cover important edge cases and potential issues.

---

## Notes

- All logger functions use printf-style formatting (`logXxxf()`)
- Logger supports both Serial output (JSON format when WiFi connected) and file logging (SD card)
- Error and fatal messages are stored in a circular buffer for web UI access
- Logger uses mutex protection for thread-safe operation
- Logger automatically flushes buffered data periodically (every 5 seconds) or on demand

