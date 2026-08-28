# Debug Logging Recommendations for Networking and Recording

## Overview
This document provides specific recommendations for adding debug-level logging to diagnose networking and recording-related issues. These recommendations focus on areas where additional visibility would help troubleshoot problems without cluttering production logs.

## Logging Levels
- **DEBUG**: Detailed diagnostic information for troubleshooting
- **INFO**: General operational information
- **WARNING**: Potential issues that don't prevent operation
- **ERROR**: Errors that prevent operation
- **EVENT**: Important state changes

---

## Networking Debug Logging Recommendations

### 1. WiFi Connection Process (`network.cpp`)

#### 1.1 WiFi Connection Attempts (`connectToWiFi()` - ~line 1973)
**Current State**: Only logs final failure if all credentials fail
**Recommendation**: Add debug logging for each credential attempt

```cpp
// Add after line 2005 (WiFi.begin call)
logDebugf("[WiFi] Attempting connection to SSID: %s (timeout: %lums)", 
          ssid_c, timeoutMs);

// Add after line 2013 (connection success check)
if (WiFi.status() == WL_CONNECTED)
{
    logDebugf("[WiFi] Successfully connected to SSID: %s (took %lums)", 
              ssid_c, millis() - start);
    // ... existing code
}
else
{
    logDebugf("[WiFi] Connection to SSID: %s failed (status: %d, elapsed: %lums)", 
              ssid_c, WiFi.status(), millis() - start);
}
```

**Why**: Helps diagnose which SSID is being tried, connection timing, and failure reasons.

#### 1.2 WiFi Reconnection Logic (`network_loop()` - ~line 562)
**Current State**: Minimal logging on reconnection attempts
**Recommendation**: Add debug logging for reconnection attempts and backoff

```cpp
// Add after line 599 (before connectToWiFi call)
logDebugf("[WiFi] Reconnection attempt %u/%u (backoff: %lums)", 
          wifiReconnectAttemptCount, kMaxWiFiReconnectAttempts, reconnectInterval);

// Add after line 606 (after connectToWiFi call)
logDebugf("[WiFi] Reconnection attempt completed (status: %d)", WiFi.status());

// Add after line 610 (when resetting after extended wait)
logDebugf("[WiFi] Resetting reconnection counter after extended wait");
```

**Why**: Tracks reconnection attempts, backoff intervals, and helps identify connection instability patterns.

#### 1.3 WiFi Event Handler (`handleWiFiEvent()` - ~line 283)
**Current State**: Events are deferred, minimal immediate logging
**Recommendation**: Add debug logging for event types

```cpp
// Add at start of handleWiFiEvent function
logDebugf("[WiFi] Event received: %d", static_cast<int>(event));

// Add in ARDUINO_EVENT_WIFI_STA_CONNECTED case (after line 288)
logDebugf("[WiFi] Station connected to AP");

// Add in ARDUINO_EVENT_WIFI_STA_DISCONNECTED case (after line 321)
logDebugf("[WiFi] Station disconnected (reason: %d)", 
          info.wifi_sta_disconnected.reason);
```

**Why**: Provides visibility into WiFi state machine transitions and helps diagnose connection issues.

#### 1.4 WiFi Client Connection Retry (`connectWiFiClientWithRetry()` - ~line 178)
**Current State**: Logs errors but not successful connections
**Recommendation**: Add debug logging for successful connections

```cpp
// Add after line 199 (when connection succeeds)
logDebugf("[Network] Successfully connected to %s:%u (attempt %u/%u, elapsed: %lums)", 
          maskHostnameForLogging(host), port, attempt + 1, maxRetries, 
          millis() - connectionStartMs);
```

**Why**: Confirms successful connections and helps diagnose connection timing issues.

### 2. HTTP Upload Operations (`network.cpp`)

#### 2.1 Upload Start/Progress (`uploadAudioFile()` - ~line 637)
**Current State**: Logs attempt start but not progress
**Recommendation**: Add debug logging for upload progress

```cpp
// Add after line 1047 (before upload loop)
logDebugf("[Upload] Starting upload to %s:%u (file size: %lu bytes, mode: %s)", 
          maskHostnameForLogging(endpoint.host), endpointPort, 
          static_cast<unsigned long>(fileSize),
          request.isPsramMode ? "PSRAM" : "SD");

// Add in PSRAM upload loop (after line 1058, every N chunks)
if (chunkCount % 50 == 0) // Log every 50 chunks
{
    size_t uploaded = fileSize - remaining;
    float progress = (uploaded * 100.0f) / fileSize;
    logDebugf("[Upload] Upload progress: %.1f%% (%lu/%lu bytes)", 
              progress, static_cast<unsigned long>(uploaded), 
              static_cast<unsigned long>(fileSize));
}

// Add in SD card upload loop (after line 1077, every N chunks)
if (chunkCount % 50 == 0) // Log every 50 chunks
{
    logDebugf("[Upload] Upload progress: %lu bytes read", 
              static_cast<unsigned long>(audioFile.position()));
}
```

**Why**: Helps diagnose slow uploads, network interruptions, and upload failures.

#### 2.2 Upload Response Processing (`uploadAudioFile()` - ~line 1096)
**Current State**: Logs errors but not successful responses
**Recommendation**: Add debug logging for response details

```cpp
// Add after line 1123 (when response is received)
logDebugf("[Upload] Response received from %s:%u (length: %u bytes, elapsed: %lums)", 
          maskHostnameForLogging(endpoint.host), endpointPort, 
          response.length(), millis() - responseTimer);

// Add after line 1152 (when response analysis succeeds)
if (analysis == "OK")
{
    logDebugf("[Upload] Upload successful to %s:%u (status: %d, response_time: %lums)", 
              maskHostnameForLogging(endpoint.host), endpointPort, 
              statusCode, millis() - attemptStartMs);
}
```

**Why**: Confirms successful uploads and helps diagnose response parsing issues.

#### 2.3 Endpoint Selection (`uploadAudioFile()` - ~line 935)
**Current State**: No logging for endpoint selection logic
**Recommendation**: Add debug logging for endpoint selection

```cpp
// Add after line 950 (when endpoint is skipped)
if (!appSettings.upload.enabled[idx])
{
    logDebugf("[Upload] Skipping endpoint %zu: disabled", idx);
}
else if (endpoint.dead)
{
    logDebugf("[Upload] Skipping endpoint %zu: marked dead (failures: %u)", 
              idx, endpoint.failureCount);
}
else if (network_isCircuitBreakerOpen(idx))
{
    logDebugf("[Upload] Skipping endpoint %zu: circuit breaker open", idx);
}

// Add after line 960 (when endpoint is selected)
logDebugf("[Upload] Attempting endpoint %zu: %s:%u", 
          idx, maskHostnameForLogging(endpoint.host), endpointPort);
```

**Why**: Helps understand which endpoints are being tried and why others are skipped.

### 3. Event Sending (`network.cpp`)

#### 3.1 Event Queue Operations (`eventTask()` - ~line 2084)
**Current State**: No logging for queue operations
**Recommendation**: Add debug logging for queue processing

```cpp
// Add in eventTask loop (after dequeuing event)
logDebugf("[Event] Processing event from queue: type=%s, queue_size=%u", 
          entry.eventType, uxQueueMessagesWaiting(eventQueue));

// Add after successful event send (after line 2284)
logDebugf("[Event] Event sent successfully: type=%s, endpoint=%zu, response_time=%lums", 
          entry.eventType, idx, responseTimeMs);
```

**Why**: Helps diagnose event queue backlogs and event sending failures.

#### 3.2 Event Connection Attempts (`sendEvent()` - ~line 2174)
**Current State**: Logs endpoint but not connection details
**Recommendation**: Add debug logging for connection attempts

```cpp
// Add after line 2184 (after connection attempt)
if (!connectWiFiClientWithRetry(...))
{
    logDebugf("[Event] Failed to connect to endpoint %zu for event: %s", 
              idx, entry.eventType);
}
else
{
    logDebugf("[Event] Connected to endpoint %zu for event: %s", 
              idx, entry.eventType);
}
```

**Why**: Helps diagnose event sending connection issues.

### 4. Settings Sync (`network.cpp`)

#### 4.1 Settings Push (`network_pushSettingsToServer()` - ~line 1686)
**Current State**: Logs attempt but not details
**Recommendation**: Add debug logging for push operations

```cpp
// Add after line 1698 (after connection attempt)
if (!connectWiFiClientWithRetry(...))
{
    logDebugf("[Settings] Failed to connect to endpoint %zu for settings push", idx);
}
else
{
    logDebugf("[Settings] Connected to endpoint %zu for settings push", idx);
}

// Add after successful push (need to identify success location)
logDebugf("[Settings] Settings pushed successfully to endpoint %zu (response_time: %lums)", 
          idx, millis() - attemptStartMs);
```

**Why**: Helps diagnose settings synchronization issues.

#### 4.2 Settings Pull (`network_pullSettingsFromServer()` - ~line 1782)
**Current State**: Logs skip but not pull details
**Recommendation**: Add debug logging for pull operations

```cpp
// Add after successful connection (need to identify location)
logDebugf("[Settings] Connected to endpoint %zu for settings pull", idx);

// Add after successful pull (need to identify location)
logDebugf("[Settings] Settings pulled successfully from endpoint %zu (response_time: %lums)", 
          idx, millis() - attemptStartMs);
```

**Why**: Helps diagnose settings retrieval issues.

### 5. Circuit Breaker and Health Metrics (`network.cpp`)

#### 5.1 Circuit Breaker State Changes (`network_isCircuitBreakerOpen()` - ~line 2576)
**Current State**: Logs when opened but not when closed
**Recommendation**: Add debug logging for state changes

```cpp
// Add when circuit breaker closes (need to identify location)
logDebugf("[Network] Circuit breaker closed for endpoint %s (health score: %.1f)", 
          maskHostnameForLogging(metrics.host), metrics.healthScore);

// Add periodic health score logging
logDebugf("[Network] Endpoint %s health: score=%.1f, failures=%u, min_response=%lums", 
          maskHostnameForLogging(metrics.host), metrics.healthScore, 
          metrics.failureCount, metrics.minResponseTimeMs);
```

**Why**: Helps diagnose circuit breaker behavior and endpoint health.

---

## Recording Debug Logging Recommendations

### 1. Recording Start/Stop (`recorder.cpp`)

#### 1.1 Recording Start (`startRecording()` - ~line 497)
**Current State**: Logs event but not detailed state
**Recommendation**: Add debug logging for recording initialization

```cpp
// Add after line 512 (PSRAM mode check)
if (isRecordingModePsram())
{
    logDebugf("[Record] Starting PSRAM recording (max_bytes: %zu, queue_slots: %u)", 
              maxBytes, psramQueue_getAvailableSlots());
}
else
{
    logDebugf("[Record] Starting SD card recording (path: %s)", 
              currentRecordingPath.c_str());
}

// Add after line 598 (recording start confirmed)
logDebugf("[Record] Recording started successfully (mode: %s, path: %s, pre_record: %ums)", 
          isRecordingModePsram() ? "PSRAM" : "SD", 
          currentRecordingPath.c_str(), 
          currentPreRecordMsApplied);
```

**Why**: Helps diagnose recording initialization failures and mode selection.

#### 1.2 Recording Finalization (`finalizeRecording()` - ~line 357)
**Current State**: Logs summary but not detailed state
**Recommendation**: Add debug logging for finalization steps

```cpp
// Add at start of function
logDebugf("[Record] Finalizing recording (upload: %d, reason: %s, bytes: %zu, duration: %ums)", 
          upload, endReason, recordedBytes, durationMs);

// Add after line 391 (WAV header written in PSRAM mode)
logDebugf("[Record] WAV header written to PSRAM buffer (size: %zu bytes)", 
          sizeof(WaveHeader));

// Add after line 394 (PSRAM queue add)
if (psramQueue_addRecording(...))
{
    logDebugf("[Record] Recording added to PSRAM queue (total_size: %zu bytes)", 
              totalSize);
}

// Add after line 433 (file rename in SD mode)
logDebugf("[Record] File renamed from .tmp to .wav: %s", 
          finishedRecordingPath.c_str());
```

**Why**: Helps diagnose finalization failures and queue operations.

### 2. Audio Data Operations (`recorder.cpp`)

#### 2.1 Audio Sample Writing (`appendAudioSamples()` - ~line 270)
**Current State**: Logs errors but not successful writes
**Recommendation**: Add debug logging for write operations

```cpp
// Add after line 316 (successful write)
if (writeSuccess)
{
    logDebugf("[Record] Audio write successful (bytes: %zu, total: %zu, attempts: %d)", 
              written, recordedBytes, attempt + 1);
}

// Add after line 323 (write retry)
if (attempt > 0)
{
    logDebugf("[Record] Audio write retry %d/%d (previous write: %zu bytes)", 
              attempt, kMaxWriteRetries, written);
}
```

**Why**: Helps diagnose write performance issues and retry patterns.

#### 2.2 Audio Buffer Operations (`monitorAndRecordAudio()` - ~line 635)
**Current State**: No logging for buffer operations
**Recommendation**: Add debug logging for buffer state

```cpp
// Add after line 637 (audio read)
if (bytesRead == 0)
{
    logDebugf("[Record] No audio data read (timeout or error)");
    return;
}

// Add after line 643 (mono conversion)
logDebugf("[Record] Audio buffer processed (stereo_bytes: %zu, mono_samples: %zu)", 
          bytesRead, monoSamples);

// Add after line 690 (recording start attempt)
if (!isRecording)
{
    logDebugf("[Record] Sound detected but recording not started (db: %.2f, threshold: %.2f)", 
              smoothedDb, thresholdDb);
}
```

**Why**: Helps diagnose audio pipeline issues and sound detection problems.

### 3. Storage Mode Operations (`recorder.cpp`)

#### 3.1 Storage Mode Switch (`appendAudioSamples()` - ~line 228)
**Current State**: Logs warning but not detailed state
**Recommendation**: Add debug logging for mode switches

```cpp
// Add after line 231 (mode switch detected)
logDebugf("[Record] Storage mode switch detected (from SD to PSRAM, recorded_bytes: %zu)", 
          recordedBytes);

// Add after line 255 (PSRAM allocation after switch)
if (psramRecordingBuffer != nullptr)
{
    logDebugf("[Record] PSRAM buffer allocated after mode switch (size: %zu bytes)", 
              psramRecordingCapacity);
}
```

**Why**: Helps diagnose storage mode transitions and buffer allocation issues.

#### 3.2 File Operations (`startRecording()` - ~line 567)
**Current State**: Logs errors but not successful operations
**Recommendation**: Add debug logging for file operations

```cpp
// Add after line 568 (file opened successfully)
if (currentRecordingFile)
{
    logDebugf("[Record] File opened for recording (path: %s, attempt: %d)", 
              currentRecordingPath.c_str(), attempt + 1);
}

// Add after line 584 (header written)
logDebugf("[Record] WAV header written to file (size: %zu bytes)", 
          sizeof(header));
```

**Why**: Helps diagnose file I/O issues and retry patterns.

### 4. Pre-Record Buffer (`recorder.cpp`)

#### 4.1 Pre-Record Operations (`appendPreRecordAudio()` - ~line 155)
**Current State**: No logging for pre-record operations
**Recommendation**: Add debug logging for pre-record buffer

```cpp
// Add at start of function
logDebugf("[Record] Appending pre-record audio (configured: %ums, available: %zu samples)", 
          configuredMs, preRecordRingCount);

// Add after line 196 (pre-record applied)
if (renderedMs > 0)
{
    logDebugf("[Record] Pre-record audio applied (%ums, samples: %zu, db: %.2f)", 
              renderedMs, copiedSamples, preRollDb);
}
```

**Why**: Helps diagnose pre-record buffer issues and timing problems.

#### 4.2 Pre-Record Buffer Management (`pushPreRecordSamples()` - ~line 118)
**Current State**: No logging for buffer management
**Recommendation**: Add debug logging for buffer state

```cpp
// Add periodic logging (every N samples) to track buffer fill
static size_t logCounter = 0;
if (++logCounter % 1000 == 0) // Log every 1000 samples
{
    logDebugf("[Record] Pre-record buffer state (count: %zu/%zu, write_idx: %zu)", 
              preRecordRingCount, kPreRecordBufferSamples, preRecordRingWriteIndex);
}
```

**Why**: Helps diagnose pre-record buffer overflow/underflow issues.

### 5. Queue Operations (`recorder.cpp`)

#### 5.1 PSRAM Queue Operations (`finalizeRecording()` - ~line 394)
**Current State**: Logs errors but not queue state
**Recommendation**: Add debug logging for queue operations

```cpp
// Add before queue add (after line 393)
logDebugf("[Record] Adding to PSRAM queue (available_slots: %u, total_size: %zu)", 
          psramQueue_getAvailableSlots(), totalSize);

// Add after queue add
if (psramQueue_addRecording(...))
{
    logDebugf("[Record] Recording queued successfully (queue_size: %u)", 
              psramQueue_getSize());
}
```

**Why**: Helps diagnose queue full conditions and queue management issues.

---

## Implementation Priority

### High Priority (Implement First)
1. WiFi connection attempts and reconnection logic
2. Upload progress and response processing
3. Recording start/stop and finalization
4. Audio write operations and retries

### Medium Priority
5. Endpoint selection and circuit breaker state
6. Event queue operations
7. Settings sync operations
8. Storage mode switches

### Low Priority (Nice to Have)
9. Pre-record buffer management
10. Audio buffer operations
11. File operation details
12. Health metrics logging

---

## Best Practices

1. **Use DEBUG level** for detailed diagnostic information that's not needed in production
2. **Include context** in log messages (file paths, sizes, durations, error codes)
3. **Log state transitions** (start/stop, connect/disconnect, mode switches)
4. **Log retry attempts** with attempt numbers and reasons
5. **Log timing information** (elapsed time, response times, connection times)
6. **Avoid logging in tight loops** - use counters or periodic logging
7. **Mask sensitive data** (already done for hostnames via `maskHostnameForLogging()`)
8. **Use consistent prefixes** (`[WiFi]`, `[Upload]`, `[Record]`, etc.)

---

## Testing Recommendations

After implementing these logging additions:

1. **Test WiFi connection scenarios**:
   - Successful connection
   - Failed connection (wrong password)
   - Connection timeout
   - Reconnection after disconnect

2. **Test upload scenarios**:
   - Successful upload
   - Failed upload (server error)
   - Slow upload (network issues)
   - Multiple endpoint attempts

3. **Test recording scenarios**:
   - Normal recording start/stop
   - Recording with storage mode switch
   - Recording with write retries
   - Queue full conditions

4. **Monitor log file size** to ensure debug logging doesn't cause storage issues

---

## Notes

- All debug logging should be conditional on `appSettings.log.serialDebug` and `appSettings.log.fileDebug` settings
- Consider adding a "verbose debug" mode for even more detailed logging if needed
- Some logging may need to be rate-limited to avoid log spam (e.g., in tight loops)
- Review log file rotation and retention policies to handle increased log volume

