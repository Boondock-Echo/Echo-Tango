# API Events Analysis

## Overview
This document provides a comprehensive analysis of all events sent to the API endpoint `/api/v1/events` by the ESP32 device.

## API Endpoint Configuration

- **Endpoint Path:** `/api/v1/events` (defined in `config.h` as `DEFAULT_EVENT_PATH`)
- **Method:** POST
- **Content-Type:** application/json
- **Default Port:** 7001 (configurable per endpoint)
- **Default Hosts:** 
  - `api.oh.boondock.cloud`
  - `api.or.boondock.cloud`
  - `api.vi.boondock.cloud`

## Event Payload Structure

All events follow this JSON structure:

```json
{
  "event_type": "<event_type_string>",
  "mac_address": "<device_mac_address>",
  "event_data": {
    // Event-specific data (varies by event type)
  },
  "settings": {
    // Optional: Settings snapshot (only for some events)
  }
}
```

## Event Types

### 1. **`online`** - Device Online Event
**Location:** `src/network.cpp:484`

**Triggered When:**
- Device successfully connects to WiFi and obtains an IP address

**Event Data:**
```json
{
  "message": "Device started",
  "ip": "<device_ip_address>"
}
```

---

### 2. **`heartbeat`** - Periodic Device Online Ping
**Location:** `src/network.cpp` in `network_loop()`

**Triggered When:**
- About once per minute while WiFi is connected and the device is not busy with an upload (upload mutex free). If the network layer is busy, the heartbeat is skipped that cycle and tried again on the next loop.

**Event Data:**
```json
{
  "message": "Device online"
}
```

---

### 3. **`config`** - Configuration Events
**Location:** `src/main.cpp:529, 579`

**Triggered When:**
- Device sends configuration information (sent during startup/config changes)

**Two Variants:**

#### A. Recorder Configuration
```json
{
  "message": "Recorder Configuration",
  "config": {
    "tm": "<formatted_time_with_timezone>",
    "ty": "config",
    "se": <audio_threshold>,
    "mi": <min_recording_ms>,
    "mx": <max_recording_ms>,
    "sth": <silence_threshold_ms>,
    "pr": <pre_record_ms>,
    "gn": <codec_gain_db>,
    "is": <sample_rate>,
    "rsc": <record_to_sd_card_boolean>,
    "mc": "<device_mac_address>",
    "si": "<session_id>"
  }
}
```

#### B. General Configuration
```json
{
  "message": "General Configuration",
  "config": {
    "tm": "<formatted_time_with_timezone>",
    "ty": "config",
    "fw": "<firmware_version>",
    "ss": "<wifi_ssid>",
    "sie": <static_ip_enabled>,
    "rte": <rtc_enabled>,
    "usc": <use_sd_card>,
    "oh": <timezone_offset_hours>,
    "wtp": <wifi_tx_power>,
    "mc": "<device_mac_address>",
    "si": "<session_id>"
  }
}
```

---

### 4. **`record_begin`** - Recording Started
**Location:** `src/recorder.cpp:659`

**Triggered When:**
- Audio recording begins (audio threshold exceeded)

**Event Data:**
```json
{
  "path": "<recording_file_path>",
  "timestamp": "<ISO_timestamp_with_milliseconds>",
  "sample_rate": <sample_rate>
}
```

---

### 5. **`record_end`** - Recording Stopped
**Location:** `src/recorder.cpp:484`

**Triggered When:**
- Audio recording stops (due to silence, max duration, or other reason)

**Event Data:**
```json
{
  "path": "<recording_file_path>",
  "timestamp": "<ISO_timestamp_with_milliseconds>",
  "durationMs": <duration_milliseconds>,
  "sample_rate": <sample_rate>,
  "peakDb": <peak_decibels>,  // Optional, if > -120.0
  "endReason": "<stop_reason_string>"  // Optional
}
```

**Possible `endReason` values:**
- Silence threshold reached
- Maximum recording duration reached
- Other internal stop reasons

---

### 6. **`audio_upload_success`** - Audio Upload Success
**Location:** `src/network.cpp:1277`

**Triggered When:**
- Audio file successfully uploaded to API server

**Event Data:**
```json
{
  "message": "Audio file uploaded successfully",
  "filename": "<uploaded_filename>",
  "size_bytes": <file_size_bytes>,
  "duration_ms": <recording_duration_ms>,
  "peak_db": <peak_decibels>,
  "speed_x": <upload_speed_multiplier>
}
```

---

### 7. **`audio_upload_failed`** - Audio Upload Failure
**Location:** `src/network.cpp:813, 824, 834, 845, 855, 974`

**Triggered When:**
- Audio file upload fails (file open errors, network errors, etc.)

**Event Data:**
```json
{
  "reason": "file_open",
  "path": "<file_path_that_failed>"
}
```

**Possible `reason` values:**
- `"file_open"` - Failed to open file for reading
- Other failure reasons (network errors, etc.)

---

### 8. **`audio_upload_skipped`** - Audio Upload Skipped
**Location:** `src/network.cpp:683`

**Triggered When:**
- Upload is skipped because system is busy (mutex contention)

**Event Data:**
```json
{
  "reason": "busy"
}
```

---

### 9. **`settings_updated`** - Settings Updated
**Location:** `src/settings.cpp:1979`

**Triggered When:**
- Settings are successfully saved to NVS (Non-Volatile Storage)

**Event Data:**
```json
{
  "message": "Settings updated"
}
```

---

### 10. **`setting_changed`** - Individual Setting Changed
**Location:** `src/settings.cpp:2651`

**Triggered When:**
- An individual setting is changed (sent for each setting change)

**Event Data:**
```json
{
  "key": "<setting_key_path>",
  "old": "<old_value>",  // Or "old_length" for sensitive data
  "new": "<new_value>",  // Or "new_length" for sensitive data
  "sensitive": <boolean>  // true if value is sensitive (passwords, etc.)
}
```

**Note:** For sensitive settings, actual values are not sent - only lengths are included.

**Example `key` values:**
- `"upload.apiHosts[0]"`
- `"upload.apiPorts[1]"`
- `"audio.audioThreshold"`
- `"wifi[0].ssid"`
- etc.

---

### 11. **`fatal_error`** - Fatal Error Log Event
**Location:** `src/logger.cpp:575`

**Triggered When:**
- A fatal error is logged (if event logging is enabled)

**Event Data:**
```json
{
  "message": "<error_message_text>"
}
```

---

### 12. **`error`** - Error Log Event
**Location:** `src/logger.cpp:575`

**Triggered When:**
- An error is logged (if event logging is enabled)

**Event Data:**
```json
{
  "message": "<error_message_text>"
}
```

---

### 13. **`warning`** - Warning Log Event
**Location:** `src/logger.cpp:575`

**Triggered When:**
- A warning is logged (if event logging is enabled)

**Event Data:**
```json
{
  "message": "<warning_message_text>"
}
```

---

## Event Queue System

### Queue Management
- **Queue Size:** Configurable (default in code)
- **Queue Behavior:** 
  - Events are queued asynchronously
  - If queue is full, oldest event is dropped
  - Events are sent in background FreeRTOS task

### Event Sending Process
1. Event is queued via `sendEvent()` function
2. Background task (`network_eventTask()`) processes queue
3. Events are only sent when WiFi is connected
4. Multiple API endpoints are tried in sequence until one succeeds
5. Response is validated (HTTP 200-299 + JSON with "received" message)
6. Success metrics are tracked

### Response Validation
The device expects this response format:
```json
{
  "message": "Event received",  // Must contain "received"
  "timestamp": "<server_timestamp>",
  "expires_at": "<optional>",
  "new_token": "<optional>",
  "warning": "<optional>"
}
```

**Success Criteria:**
- HTTP status code: 200-299
- JSON response contains "message" field with "received" substring
- Response timeout: 10 seconds

---

## Event Statistics

The following metrics are tracked:
- `totalEventCount` - Total successfully confirmed events
- `lastEventEpoch` - Timestamp of last confirmed event
- Per-endpoint health scores and response times

---

## Event Suppression

Settings can suppress per-setting change events to avoid event storms:
- `g_suppressSettingChangeEvents` flag prevents `setting_changed` events
- Used during bulk settings updates from server
- Controlled via `settings_setChangeEventsSuppressed()`

---

## Code Locations

### Main Event Sending Function
- **File:** `src/network.cpp`
- **Function:** `sendEvent()` (line ~2390)
- **Task:** `network_eventTask()` (line ~2113)

### Event Type Definitions
- **Recording Events:** `src/recorder.cpp`
- **Network Events:** `src/network.cpp`
- **Settings Events:** `src/settings.cpp`
- **Config Events:** `src/main.cpp`
- **Log Events:** `src/logger.cpp`

---

## Summary Table

| Event Type | Frequency | Purpose | Contains Settings? |
|------------|-----------|---------|-------------------|
| `online` | On WiFi connect | Device status | No |
| `config` | On startup/config change | Device configuration | No (config in event_data) |
| `record_begin` | Per recording start | Recording lifecycle | No |
| `record_end` | Per recording end | Recording lifecycle | No |
| `audio_upload_success` | Per successful upload | Upload status | No |
| `audio_upload_failed` | Per failed upload | Upload status | No |
| `audio_upload_skipped` | When upload skipped | Upload status | No |
| `settings_updated` | On settings save | Settings sync | No |
| `setting_changed` | Per setting change | Settings sync | No |
| `fatal_error` | On fatal errors | Error tracking | No |
| `error` | On errors (if enabled) | Error tracking | No |
| `warning` | On warnings (if enabled) | Warning tracking | No |

---

## Notes

1. **Event Logging Control:** Log-based events (`fatal_error`, `error`, `warning`) are only sent if event logging is enabled in settings (`appSettings.log.serialEvent` or `appSettings.log.fileEvent`).

2. **WiFi Dependency:** All events require WiFi connection. Events are silently dropped if WiFi is not connected.

3. **Endpoint Failover:** The system tries multiple API endpoints in sequence until one succeeds.

4. **Watchdog Protection:** Event sending includes watchdog resets to prevent system timeouts during network operations.

5. **Time Limits:** Event sending has a maximum time limit (10 seconds) to prevent watchdog timeouts.



