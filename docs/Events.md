# Device Events Reference

This document describes all event types sent by the device to the API endpoint `POST /api/v1/events`.

## Payload Structure

Every event is sent as a JSON body with:

| Field | Required | Description |
|-------|----------|-------------|
| `mac_address` | Yes | Device MAC address (12-character hex, uppercase, no colons). |
| `event_type` | Yes | Event type string (see below). |
| `event_data` | No | JSON object with event-specific fields. |
| `settings` | No | Optional; present only when the event was queued with a settings snapshot. |

---

## Event Types

### 1. `online`

**When:** Device obtains an IP address (WiFi connect or reconnect).

**Source:** `src/network.cpp` (on deferred WiFi "got IP" handling).

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"Device started"` |
| `ip` | string | Device local IP address. |
| `resetReason` | string | Last reset reason (e.g. `"Software reset CPU"`). |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "online",
  "event_data": {
    "message": "Device started",
    "ip": "192.168.1.100",
    "resetReason": "Software reset CPU"
  }
}
```

---

### 2. `ping`

**When:** Periodic heartbeat (about every 60 seconds when not busy with an upload).

**Source:** `src/network.cpp` (heartbeat logic in `network_loop()`).

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"Ping"` |
| `health` | object | Health snapshot (see below). |

**health object:**

| Field | Type | Description |
|-------|------|-------------|
| `tm` | string | Formatted time with timezone (e.g. `"16:56:22"`). |
| `rc` | int | Session recording count. |
| `uc` | int | Session uploaded count. |
| `pq` | int | Upload queue size. |
| `td` | int | Total recording duration this session (seconds). |
| `am` | number | API min response time (ms). |
| `ax` | number | API max response time (ms). |
| `aa` | number | API average response time (ms). |
| `si` | string | Session ID (8-digit, persists until reboot). |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "ping",
  "event_data": {
    "message": "Ping",
    "health": {
      "tm": "16:56:22",
      "rc": 5,
      "uc": 3,
      "pq": 2,
      "td": 120,
      "am": 218,
      "ax": 784,
      "aa": 364,
      "si": "63024822"
    }
  }
}
```

---

### 3. `config` (Recorder)

**When:** Startup or when configuration is sent (recorder settings).

**Source:** `src/main.cpp` — `sendConfigMessage()`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"Recorder Configuration"` |
| `config` | object | Recorder config (see below). |

**config object (recorder):**

| Field | Type | Description |
|-------|------|-------------|
| `tm` | string | Formatted time. |
| `ty` | string | `"config"` |
| `ath` | number | Audio threshold. |
| `mrm` | number | Min recording (ms). |
| `xrm` | number | Max recording (ms). |
| `stm` | number | Silence threshold (ms). |
| `prm` | number | Pre-record (ms). |
| `cg` | number | Codec gain (dB). |
| `is` | number | Sample rate. |
| `rsc` | bool | Record to SD card. |
| `mc` | string | Device ID (MAC). |
| `si` | string | Session ID. |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "config",
  "event_data": {
    "message": "Recorder Configuration",
    "config": {
      "tm": "16:56:22",
      "ty": "config",
      "ath": 25,
      "mrm": 1000,
      "xrm": 300000,
      "stm": 2000,
      "prm": 500,
      "cg": 0,
      "is": 16000,
      "rsc": true,
      "mc": "E08CFE63F7B4",
      "si": "63024822"
    }
  }
}
```

---

### 4. `config` (General)

**When:** Same as recorder config; second event in the same flow (general/device settings).

**Source:** `src/main.cpp` — `sendConfigMessage()`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"General Configuration"` |
| `config` | object | General config (see below). |

**config object (general):** includes `tm`, `ty`, `fw`, `ss`, `sie`, `rte`, `usc`, `oh`, `wtp`, `mc`, `si`.

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "config",
  "event_data": {
    "message": "General Configuration",
    "config": {
      "tm": "16:56:22",
      "ty": "config",
      "fw": "ECHO-2026-03-10",
      "ss": "MyWiFi",
      "sie": false,
      "rte": true,
      "usc": true,
      "oh": -8,
      "wtp": 8,
      "mc": "E08CFE63F7B4",
      "si": "63024822"
    }
  }
}
```

---

### 5. `record_begin`

**When:** A recording starts (audio threshold exceeded).

**Source:** `src/recorder.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `path` | string | Recording file path. |
| `timestamp` | string | ISO timestamp with milliseconds. |
| `sample_rate` | number | Audio sample rate. |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "record_begin",
  "event_data": {
    "path": "/sd/recordings/2026-03-10_165622_001.wav",
    "timestamp": "2026-03-10T16:56:22.123Z",
    "sample_rate": 16000
  }
}
```

---

### 6. `record_end`

**When:** A recording stops (silence, max duration, or other reason).

**Source:** `src/recorder.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `path` | string | Recording file path. |
| `timestamp` | string | ISO timestamp (optional if epoch invalid). |
| `durationMs` | number | Duration in milliseconds. |
| `sample_rate` | number | Audio sample rate. |
| `peakDb` | number | Optional. Peak level in dB. |
| `endReason` | string | Optional. Stop reason (e.g. `"Silence threshold reached"`). |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "record_end",
  "event_data": {
    "path": "/sd/recordings/2026-03-10_165622_001.wav",
    "timestamp": "2026-03-10T16:56:22.123Z",
    "durationMs": 45000,
    "sample_rate": 16000,
    "peakDb": -12.5,
    "endReason": "Silence threshold reached"
  }
}
```

---

### 7. `audio_upload_success`

**When:** An audio file is successfully uploaded.

**Source:** `src/network.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"Audio file successfully uploaded"` |
| `filename` | string | Uploaded filename. |
| `size_bytes` | number | File size in bytes. |
| `duration_ms` | number | Recording duration (ms). |
| `peak_db` | number | Peak level (dB). |
| `speed_x` | number | Upload speed multiplier (duration/elapsed). |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "audio_upload_success",
  "event_data": {
    "message": "Audio file successfully uploaded",
    "filename": "2026-03-10_165622_001.wav",
    "size_bytes": 1440000,
    "duration_ms": 45000,
    "peak_db": -12.5,
    "speed_x": 2.3
  }
}
```

---

### 8. `audio_upload_failed`

**When:** An upload attempt fails because the file could not be opened.

**Source:** `src/network.cpp` (multiple call sites).

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `reason` | string | `"file_open"` in current implementation. |
| `path` | string | Path that failed to open. |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "audio_upload_failed",
  "event_data": {
    "reason": "file_open",
    "path": "/sd/recordings/2026-03-10_165622_001.wav"
  }
}
```

---

### 9. `audio_upload_skipped`

**When:** Upload is skipped because the system is busy (e.g. upload mutex held).

**Source:** `src/network.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `reason` | string | `"busy"` |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "audio_upload_skipped",
  "event_data": {
    "reason": "busy"
  }
}
```

---

### 10. `settings_updated`

**When:** Settings are successfully saved to NVS (e.g. after debounced save).

**Source:** `src/settings.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | `"Settings updated"` |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "settings_updated",
  "event_data": {
    "message": "Settings updated"
  }
}
```

---

### 11. `setting_changed`

**When:** A single setting is changed (per-setting events not suppressed). Not sent for WiFi credential keys (e.g. `wifi[0].ssid`, `wifi[0].password`).

**Source:** `src/settings.cpp` — `sendSettingChangeEvent()`.

**event_data (non-sensitive):**

| Field | Type | Description |
|-------|------|-------------|
| `key` | string | Setting key path (e.g. `"audio.audioThreshold"`). |
| `old` | string | Previous value. |
| `new` | string | New value. |
| `sensitive` | bool | `false` |

**event_data (sensitive):** `key`, `sensitive: true`, `old_length`, `new_length` (values not sent).

**Example (non-sensitive):**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "setting_changed",
  "event_data": {
    "key": "audio.audioThreshold",
    "old": "20",
    "new": "25",
    "sensitive": false
  }
}
```

---

### 12. `fatal_error` / `error` / `warning`

**When:** A fatal, error, or warning log entry is written and event logging is enabled. Suppressed for messages that indicate event-send failures (to avoid feedback loops).

**Source:** `src/logger.cpp`.

**event_data:**

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | Log message text. |

**Example:**
```json
{
  "mac_address": "E08CFE63F7B4",
  "event_type": "error",
  "event_data": {
    "message": "SD card mount failed: no card present"
  }
}
```

Same structure for `event_type` `"fatal_error"` or `"warning"`.

---

## Summary Table

| event_type | Source | Trigger |
|------------|--------|---------|
| `online` | network.cpp | WiFi got IP |
| `ping` | network.cpp | Periodic heartbeat (~60 s, when not uploading) |
| `config` | main.cpp | Startup/config (recorder + general) |
| `record_begin` | recorder.cpp | Recording started |
| `record_end` | recorder.cpp | Recording stopped |
| `audio_upload_success` | network.cpp | File uploaded successfully |
| `audio_upload_failed` | network.cpp | File open failed |
| `audio_upload_skipped` | network.cpp | Upload skipped (busy) |
| `settings_updated` | settings.cpp | Settings saved to NVS |
| `setting_changed` | settings.cpp | Single setting changed (no WiFi keys) |
| `fatal_error` | logger.cpp | Fatal log (if event logging enabled) |
| `error` | logger.cpp | Error log (if event logging enabled) |
| `warning` | logger.cpp | Warning log (if event logging enabled) |

---

## Delivery

- Events are queued via `sendEvent(type, message, settings)` and sent asynchronously by the event task.
- Endpoint: `POST /api/v1/events` (path from `config.h`: `DEFAULT_EVENT_PATH`).
- Events are only sent when WiFi is connected; otherwise they are dropped or re-queued as appropriate.
- If the queued `message` is valid JSON, it is used as `event_data`; otherwise `event_data` is `{ "message": "<raw string>" }`.
