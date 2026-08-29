# Boondock Cloud API Reference

This document describes the outbound HTTP calls the device firmware makes to
the Boondock cloud servers. All requests originate from the ESP32 device.

---

## Common Infrastructure

### Default Endpoints

| Slot | Region | Domain | IP | Port |
|------|--------|--------|----|------|
| 0 | Ohio | `api.oh.boondock.cloud` | `3.128.235.120` | 7001 |
| 1 | Oregon | `api.or.boondock.cloud` | `35.85.105.6` | 7001 |
| 2 | Virginia | `api.vi.boondock.cloud` | `52.1.103.236` | 7001 |
| 3 | Custom | `apitest.boondock.cloud` | `52.38.112.84` | 7001 |

- **Protocol:** Plain HTTP (no TLS)
- **Default port:** `7001`
- The device cycles through all enabled slots on failure.

### User-Agent

```
Boondock-TANGO V-TANGO-2026-03-13
```

Format: `Boondock-<PREFIX> V-<FIRMWARE>`  
The prefix is derived from the firmware string up to the first `-` (e.g. `TANGO`, `ECHO`).

### Authentication

No tokens or API keys. Device identity is always the MAC address embedded in
each request (as a form field, JSON field, or URL path segment).

---

## 1. Audio Upload

**`POST /api/v2/audio/s3`**

Uploads a recorded WAV file for transcription and storage.

### Headers

| Header | Value |
|--------|-------|
| `User-Agent` | `Boondock-TANGO V-<FIRMWARE>` |
| `Accept` | `application/json` |
| `Content-Type` | `multipart/form-data; boundary=----BoondockBoundary` |
| `Content-Length` | calculated |
| `Connection` | `close` |

### Multipart Form Fields

| Field | Always sent | Description |
|-------|-------------|-------------|
| `convert_to_mp3` | Yes | Always `"true"` |
| `mac_address` | Yes | Device MAC address |
| `tags` | Conditional | JSON metadata object (see below) |
| `audio_file` | Yes | Binary WAV data (`Content-Type: audio/wav`) |

### Full Request Example

```http
POST /api/v2/audio/s3 HTTP/1.1
Host: api.oh.boondock.cloud:7001
User-Agent: Boondock-TANGO V-TANGO-2026-03-13
Accept: application/json
Content-Type: multipart/form-data; boundary=----BoondockBoundary
Content-Length: <calculated>
Connection: close

------BoondockBoundary
Content-Disposition: form-data; name="convert_to_mp3"

true
------BoondockBoundary
Content-Disposition: form-data; name="mac_address"

AABBCCDDEEFF
------BoondockBoundary
Content-Disposition: form-data; name="tags"

{"recorder":{"id":"AABBCCDDEEFF","trigger":1,"endReason":"silence","duration":12,"durationMs":12345,"size":218412,"dataBytes":217600,"timestamp":"2026-03-13T14:30:00Z","path":"AA_BB_CC_DD_EE_FF_20260313_143000.wav","decibel":-34.7},"dock":{"id":"AABBCCDDEEFF"},"user":{"name":"AABBCCDDEEFF","ip":"192.168.1.42"}}
------BoondockBoundary
Content-Disposition: form-data; name="audio_file"; filename="AA_BB_CC_DD_EE_FF_20260313_143000.wav"
Content-Type: audio/wav

<binary WAV data>
------BoondockBoundary--
```

### `tags` Field — Full JSON Schema

```json
{
  "recorder": {
    "id": "AABBCCDDEEFF",
    "trigger": 1,
    "endReason": "silence",
    "duration": 12,
    "durationMs": 12345,
    "size": 218412,
    "dataBytes": 217600,
    "timestamp": "2026-03-13T14:30:00Z",
    "path": "AA_BB_CC_DD_EE_FF_20260313_143000.wav",
    "decibel": -34.7
  },
  "dock": {
    "id": "AABBCCDDEEFF"
  },
  "user": {
    "name": "AABBCCDDEEFF",
    "ip": "192.168.1.42"
  }
}
```

| Field | Type | Optional | Description |
|-------|------|----------|-------------|
| `recorder.id` | string | No | Device MAC address |
| `recorder.trigger` | int | No | Always `1` |
| `recorder.endReason` | string | Yes | Why recording stopped (e.g. `"silence"`, `"maxDuration"`) — omitted if empty |
| `recorder.duration` | uint32 | No | Recording length in whole seconds |
| `recorder.durationMs` | uint32 | No | Recording length in milliseconds |
| `recorder.size` | uint32 | No | Total WAV file size in bytes (including header) |
| `recorder.dataBytes` | uint32 | No | Raw PCM audio data size in bytes (excluding header) |
| `recorder.timestamp` | string | No | ISO 8601 recording start time |
| `recorder.path` | string | No | WAV filename |
| `recorder.decibel` | float | Yes | Peak dB level — omitted when ≤ -120.0 |
| `dock.id` | string | No | Device MAC address |
| `user.name` | string | No | Device MAC address |
| `user.ip` | string | No | Device local WiFi IP address |

### Timeouts

| Phase | Timeout |
|-------|---------|
| TCP connect | 3 s |
| Body send | 60 s |
| Response wait | 5 s |
| Total (all slots) | 60 s |

### Success Response

```json
{
  "message": "Audio uploaded successfully",
  "local_path": "/inbox/AA_BB_CC_DD_EE_FF_20260313_143000.wav",
  "timestamp": "2026-03-13T14:30:05Z"
}
```

The firmware accepts a `timestamp` or `current_time` field in the response to sync the device clock.

---

## 2. Log File Upload

**`POST /api/v1/upload/logs`**

> Note the capital **V** in `v1`.

Uploads the device's daily `.LOG` file.

### Headers

Same as Audio Upload (multipart, `----BoondockBoundary`).

### Multipart Form Fields

| Field | Always sent | Description |
|-------|-------------|-------------|
| `mac_address` | Yes | Device MAC address |
| `filename` | Yes | Log filename (converted to `.LOG` extension) |
| `file` | Yes | Plain text log content (`Content-Type: text/plain`) |

Filename conversion: `2026-03-13-LOG.txt` → `2026-03-13.LOG`

### Full Request Example

```http
POST /api/v1/upload/logs HTTP/1.1
Host: api.oh.boondock.cloud:7001
User-Agent: Boondock-TANGO V-TANGO-2026-03-13
Accept: application/json
Content-Type: multipart/form-data; boundary=----BoondockBoundary
Content-Length: <calculated>
Connection: close

------BoondockBoundary
Content-Disposition: form-data; name="mac_address"

AABBCCDDEEFF
------BoondockBoundary
Content-Disposition: form-data; name="filename"

2026-03-13.LOG
------BoondockBoundary
Content-Disposition: form-data; name="file"; filename="2026-03-13.LOG"
Content-Type: text/plain

<raw log file text>
------BoondockBoundary--
```

### Response

HTTP status code only — `200–299` = success. No JSON body parsed.

---

## 3. Events

**`POST /api/v1/events`**

Posts a device lifecycle or status event. Uses a plain JSON body (not multipart).

### Headers

| Header | Value |
|--------|-------|
| `User-Agent` | `Boondock-TANGO V-<FIRMWARE>` |
| `Accept` | `application/json` |
| `Content-Type` | `application/json` |
| `Content-Length` | calculated |
| `Connection` | `close` |

### Request Body

```json
{
  "event_type": "online",
  "mac_address": "AABBCCDDEEFF",
  "event_data": {
    "message": "Device came online",
    "reset_reason": "power_on",
    "firmware": "TANGO-2026-03-13"
  },
  "settings": {}
}
```

| Field | Optional | Description |
|-------|----------|-------------|
| `event_type` | No | Event identifier string (see table below) |
| `mac_address` | No | Device MAC address |
| `event_data` | No | Event payload — parsed from event message JSON, or `{"message":"..."}` if plain text |
| `settings` | Yes | Full settings object, only included when a settings payload is attached to the event |

### Known Event Types

| `event_type` | When sent |
|--------------|-----------|
| `"online"` | Device boots and connects to WiFi |
| `"ping"` | Periodic heartbeat |
| `"audio_upload_success"` | Audio file uploaded successfully |
| `"audio_upload_failed"` | Upload attempt failed |
| `"audio_upload_skipped"` | File skipped (e.g. too small, below threshold) |

### Timeouts

| Phase | Timeout |
|-------|---------|
| TCP connect | 2 s |
| Response wait | 2 s |
| Total (all slots) | 10 s |
| Pre-connect delay | 100 ms (rate-limit spacing) |
| WiFi reconnect wait | 3 s before first event |

### Success Response

```json
{ "message": "Event received", "timestamp": "2026-03-13T14:30:01Z" }
```

The firmware accepts `"timestamp"` or `"current_time"` in the response to sync the device clock. An HTTP 200 with an empty body is also treated as success.

---

## 4. Firmware Check

**`GET /api/v1/firmware/check`**

Checks whether a newer firmware version is available.

### Headers

| Header | Value |
|--------|-------|
| `User-Agent` | `Boondock-TANGO V-<FIRMWARE>` |
| `Accept` | `application/json` |
| `Connection` | `close` |

### Query Parameters

| Parameter | Description |
|-----------|-------------|
| `current_version` | Current firmware string (e.g. `TANGO-2026-03-13`) |

### Full Request Example

```http
GET /api/v1/firmware/check?current_version=TANGO-2026-03-13 HTTP/1.1
Host: api.oh.boondock.cloud:7001
User-Agent: Boondock-TANGO V-TANGO-2026-03-13
Accept: application/json
Connection: close
```

### Timeouts

| Phase | Timeout |
|-------|---------|
| TCP connect | 3 s (1 retry) |
| Response wait | 10 s |

### Response

Full JSON body returned transparently to the web UI. The firmware does not extract
specific fields — it validates the response is parseable JSON and passes it on.

---

## 5. Settings Upload (Push)

**`POST /api/v1/settings`**

Pushes the full device configuration to the cloud for backup and remote management.

### Headers

| Header | Value |
|--------|-------|
| `Content-Type` | `multipart/form-data; boundary=----BoondockSettingsBoundary` |

### Multipart Form Fields

| Field | Description |
|-------|-------------|
| `mac_address` | Device MAC address |
| `settings` | Full settings JSON object (see schema below) |

### Full Request Example

```http
POST /api/v1/settings HTTP/1.1
Host: api.oh.boondock.cloud:7001
User-Agent: Boondock-TANGO V-TANGO-2026-03-13
Accept: application/json
Content-Type: multipart/form-data; boundary=----BoondockSettingsBoundary
Content-Length: <calculated>
Connection: close

------BoondockSettingsBoundary
Content-Disposition: form-data; name="mac_address"

AABBCCDDEEFF
------BoondockSettingsBoundary
Content-Disposition: form-data; name="settings"

<settings JSON — see schema below>
------BoondockSettingsBoundary--
```

### Timeouts

| Phase | Timeout |
|-------|---------|
| TCP connect | 2 s |
| Response wait | 2 s |
| Total (all slots) | 5 s |

### Response

HTTP status code only — `200–299` = success. Tries all 4 endpoint slots.

---

## 6. Settings Download (Pull)

**`GET /api/v1/settings/{mac_address}`**

Downloads the stored device configuration from the cloud.

### Headers

| Header | Value |
|--------|-------|
| `User-Agent` | `Boondock-TANGO V-<FIRMWARE>` |
| `Accept` | `application/json` |
| `Connection` | `close` |

### Full Request Example

```http
GET /api/v1/settings/AABBCCDDEEFF HTTP/1.1
Host: api.oh.boondock.cloud:7001
User-Agent: Boondock-TANGO V-TANGO-2026-03-13
Accept: application/json
Connection: close
```

### Timeouts

| Phase | Timeout |
|-------|---------|
| TCP connect | 2 s |
| Response wait | 2 s |
| Total (all slots) | 5 s |
| Pre-connect delay | 100 ms |

### Response

Full settings JSON body (same schema as the `settings` field in the Push endpoint).
The firmware applies the body directly to the device configuration.

---

## Settings JSON Schema

Used as the `settings` form field in **Settings Upload**, and returned as the
response body in **Settings Download**.

> All keys use the compact short-key format as sent on the wire.

```json
{
  "fw":  "TANGO-2026-03-13",
  "cv":  "1.0.0",
  "mac": "AABBCCDDEEFF",
  "wtp": 78,
  "wse": true,

  "w": [
    {
      "ss":  "MyWiFiNetwork",
      "pw":  "mypassword",
      "ctm": 10000,
      "sie": false,
      "sip": "",
      "ssn": "",
      "sgt": "",
      "sd1": "",
      "sd2": ""
    }
  ],

  "a": {
    "sr":  8000,
    "bs":  512,
    "ath": 50,
    "prm": 500,
    "mrm": 1000,
    "xrm": 300000,
    "stm": 2000,
    "dsf": false,
    "dmm": 1000,
    "cg":  0
  },

  "u": {
    "qd":  10,
    "ctm": true,
    "ah":  ["api.oh.boondock.cloud", "api.or.boondock.cloud", "api.vi.boondock.cloud", "apitest.boondock.cloud"],
    "ap":  [7001, 7001, 7001, 7001],
    "en":  [true, true, true, false]
  },

  "r": {
    "en":  false,
    "sda": 21,
    "scl": 22
  },

  "s": {
    "usc": true,
    "rsc": true,
    "m1b": false,
    "frq": 20000000,
    "fmf": false
  },

  "t": {
    "oh": -5,
    "mh": 3,
    "mm": 0
  },

  "l": {
    "sf":  true,
    "se":  true,
    "sw":  true,
    "si":  false,
    "sd":  false,
    "sev": true,
    "ff":  true,
    "fe":  true,
    "fw":  true,
    "fi":  true,
    "fd":  false,
    "fev": true
  }
}
```

### Key Reference

#### Top-Level

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `fw` | firmware | string | Firmware version string |
| `cv` | configVersion | string | Config schema version |
| `mac` | macAddress | string | Device MAC address |
| `wtp` | wifiTxPower | int (1–10) | WiFi transmit power level |
| `wse` | webserverEnabled | bool | Enable built-in web server |

#### `w` — WiFi Array (up to 3 entries)

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `ss` | ssid | string | Network name |
| `pw` | password | string | Network password |
| `ctm` | connectTimeoutMs | int | Connection timeout in ms |
| `sie` | staticIpEnabled | bool | Use static IP |
| `sip` | staticIp | string | Static IP address |
| `ssn` | staticSubnet | string | Subnet mask |
| `sgt` | staticGateway | string | Gateway address |
| `sd1` | staticDns1 | string | Primary DNS |
| `sd2` | staticDns2 | string | Secondary DNS |

#### `a` — Audio

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `sr` | sampleRate | int | Sample rate in Hz (default: 8000) |
| `bs` | bufferSamples | int | DMA buffer size in samples |
| `ath` | audioThreshold | int (0–100) | Voice activity detection threshold |
| `prm` | preRecordMs | int | Pre-record buffer duration in ms |
| `mrm` | minRecordingMs | int | Minimum recording duration in ms |
| `xrm` | maxRecordingMs | int | Maximum recording duration in ms |
| `stm` | silenceThresholdMs | int | Silence gap before ending recording in ms |
| `dsf` | discardSmallFilesEnabled | bool | Discard recordings below minimum size |
| `dmm` | discardSmallFilesMinMs | int | Minimum duration to keep a recording in ms |
| `cg` | codecGainDb | int | Codec gain in dB (discrete: -3, 0, 3 … 24) |

#### `u` — Upload

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `qd` | queueDepth | int (4–32) | Max files queued for upload |
| `ctm` | convertToMp3 | bool | Convert WAV to MP3 before upload |
| `ah` | apiHosts | string[4] | API endpoint hostnames/IPs |
| `ap` | apiPorts | int[4] | API endpoint ports |
| `en` | enabled | bool[4] | Enable/disable each endpoint slot |

#### `r` — RTC

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `en` | enabled | bool | Enable hardware RTC |
| `sda` | sdaPin | int | I2C SDA GPIO pin |
| `scl` | sclPin | int | I2C SCL GPIO pin |

#### `s` — SD Card

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `usc` | useSdCard | bool | Enable SD card |
| `rsc` | recordToSdCard | bool | Save recordings to SD card |
| `m1b` | mode1bit | bool | Use 1-bit SD bus mode |
| `frq` | frequency | int (1M–20M) | SD bus frequency in Hz |
| `fmf` | formatIfMountFailed | bool | Auto-format SD on mount failure |

#### `t` — Timezone

| Key | Full name | Type | Description |
|-----|-----------|------|-------------|
| `oh` | offsetHours | int (-12 to +14) | UTC offset in hours |
| `mh` | maintenanceHour | int (0–23) | Hour to run daily maintenance |
| `mm` | maintenanceMinute | int (0–59) | Minute to run daily maintenance |

#### `l` — Log Levels

| Key | Full name | Destination |
|-----|-----------|-------------|
| `sf` | serialFatal | Serial |
| `se` | serialError | Serial |
| `sw` | serialWarning | Serial |
| `si` | serialInfo | Serial |
| `sd` | serialDebug | Serial |
| `sev` | serialEvent | Serial |
| `ff` | fileFatal | SD log file |
| `fe` | fileError | SD log file |
| `fw` | fileWarning | SD log file |
| `fi` | fileInfo | SD log file |
| `fd` | fileDebug | SD log file |
| `fev` | fileEvent | SD log file |

---

## Endpoint Summary

| # | Method | Path | Body format | Device identity |
|---|--------|------|-------------|-----------------|
| 1 | POST | `/api/v2/audio/s3` | `multipart/form-data` | `mac_address` field |
| 2 | POST | `/api/v1/upload/logs` | `multipart/form-data` | `mac_address` field |
| 3 | POST | `/api/v1/events` | `application/json` | `mac_address` JSON field |
| 4 | GET | `/api/v1/firmware/check?current_version=...` | none | query parameter |
| 5 | POST | `/api/v1/settings` | `multipart/form-data` | `mac_address` field |
| 6 | GET | `/api/v1/settings/{mac_address}` | none | MAC in URL path |
