# Boondock TANGO/ECHO – Architecture

## Overview

**Boondock TANGO-ECHO** is an ESP32-based **voice-activated audio recorder** that records sound above a threshold, stores recordings on an SD card (or PSRAM as fallback), and uploads them to cloud API endpoints. It targets IoT-style deployment for continuous monitoring and remote access.

The system runs two firmware variants:
- **TANGO** – Firmware with TANGO-specific configuration
- **ECHO** – Firmware with ECHO-specific configuration  

Both use the same codebase with different build flags.

---

## What the System Does

1. **Voice-Activated Recording**  
   Monitors microphone input; starts recording when audio exceeds a configurable threshold and stops after silence or max duration.

2. **Storage**  
   - **SD card mode**: WAV files in date-organized folders (`/inbox/YYYY/MM/DD/`, `/pending/` for upload queue).  
   - **PSRAM mode**: In-memory buffer when SD card is unavailable (max ~6 recordings, ~30 sec each).

3. **Cloud Upload**  
   Uploads recordings over HTTP to configured API endpoints (e.g. `/api/v2/audio/s3`), with retry logic and multiple endpoint support.

4. **Configuration & Monitoring**  
   - Serial CLI for configuration and status  
   - Optional web server for configuration and live audio  
   - AP mode for initial WiFi setup when no credentials exist

5. **Health & Maintenance**  
   Memory monitoring, task watchdog, storage cleanup, nightly summaries, and settings sync with the API.

---

## Main Logical Components

### 1. Recorder (`recorder.cpp` / `recorder.h`)

Handles voice-activated recording logic.

- **Threshold detection** – Audio level vs. configurable sensitivity (dB mapping 0–100)
- **Pre-record buffer** – ~500 ms ring buffer so recording starts slightly before trigger
- **Recording limits** – Min/max duration, silence timeout
- **Output** – WAV files to SD card or PSRAM buffer
- **Codec** – ES8388 via AudioKit HAL, configurable gain

**Notable APIs:** `recorder_isRecording()`, `recorder_getAudioLevelStats()`, `recorder_stopActiveRecording()`, `recorder_setLiveAudioCallback()` for live streaming.

---

### 2. Storage & Upload Queue (`upload_queue.cpp` / `common.cpp`)

**Storage modes:**
- **SD_CARD** – SD_MMC, date-based directory layout (`/inbox/`, `/pending/`, `/trash/`)
- **PSRAM** – Fallback when SD is unavailable; recordings held in memory

**Upload queue:**
- **Filesystem-based** – `/pending/` is the queue; files move to `/inbox/` after successful upload
- **In-memory queues** – `sdCardMemoryQueue` for recent recordings (priority), `psramQueue` for PSRAM mode

**Storage helpers:** `ensureStorage()`, `getStorageMode()`, `isStorageModeSdCard()`, `isStorageModePsram()`, `storage_updateHealthMetrics()`, cleanup and summary helpers.

---

### 3. Network (`network.cpp` / `network.h`)

WiFi and HTTP communication layer.

- **WiFi** – Connection, reconnect, multi-SSID credentials
- **Time sync** – NTP, API response timestamps
- **Upload** – HTTP POST of WAV files, events, logs
- **Endpoint selection** – Multiple hosts; random healthy endpoint selection
- **Reliability** – Circuit breaker, adaptive backoff, RSSI tracking, endpoint health metrics

**Notable APIs:** `uploadAudioFile()`, `sendEvent()`, `network_getRandomHealthyEndpoint()`, `network_pushSettingsToServer()`, `network_pullSettingsFromServer()`.

---

### 4. Settings (`settings.cpp` / `settings.h`)

Configuration management backed by NVS.

- **Stored settings** – WiFi, audio (threshold, duration, gain), upload hosts, RTC, SD card, timezone, logging
- **Serial CLI** – Text commands (`SET param value`, `REBOOT`, etc.) and AUTOCONFIG JSON
- **Server sync** – Push/pull JSON from API with masking for sensitive fields

**Notable APIs:** `settings_begin()`, `settings_processSerial()`, `settings_setParam()`, `settings_updateAllFromJson()`, `settings_getSerialMutex()`.

---

### 5. Web Server (`boondock_server.cpp` / `websocket_handler.cpp`)

HTTP + WebSocket server for management and live streaming.

- **AP mode** – Captive portal when no WiFi credentials are configured
- **Main mode** – Web UI for configuration and monitoring when WiFi is configured
- **WebSocket** – Push of home data, audio stats, network config, live audio
- **SPA assets** – Embedded HTML/CSS/JS via `web_*.h` and `app_js_*.h`

**Notable APIs:** `boondock_server_startAPMode()`, `boondock_server_loop()`, `boondock_server_pushLiveAudio()`.

---

### 6. Time Keeper (`timekeeper.cpp` / `timekeeper.h`)

Central time synchronization.

- **Sources** – RTC (DS3231), NTP, API response timestamps
- **Timezone** – Offset and maintenance window (e.g. 3 AM local)
- **Boot-time** – Tracks `millis()` at boot for time reconstruction when sync is delayed

**Notable APIs:** `timeKeeper().ensureTimeFromNtp()`, `timeKeeper().timeIsValid()`, `timeKeeper().syncFromApiResponse()`.

---

### 7. Logger (`logger.cpp` / `logger.h`)

Unified logging to Serial and SD card.

- **Levels** – FATAL, ERROR, WARNING, INFO, DEBUG, EVENT
- **Destinations** – Configurable per level for serial vs. file
- **Recent errors** – Buffered for web UI (`logger_getRecentErrors()`)

---

### 8. Health & Maintenance (`health.cpp` / `health.h`)

System health and background maintenance.

- **Task health** – Stack usage, restart count, running state
- **Mutex metrics** – Timeout tracking
- **Network metrics** – RSSI, packet loss, endpoint health
- **Storage metrics** – Utilization, errors, mount status
- **Maintenance behavior** – SD retry, storage cleanup, nightly summaries, settings push/pull, `.tmp` file cleanup

**Maintenance task** runs every 30 seconds for health checks and periodically for cleanup and nightly jobs.

---

### 9. Common Utilities (`common.cpp` / `common.h`)

Shared helpers and types.

- **Device/session** – `getDeviceId()` (MAC), `getSessionId()`
- **Audio** – `calculateDb()`, `calculatePeakSample()`, WAV header creation
- **Time** – `parseIsoTimestampToEpoch()`, `formatIsoTimestamp()`, `getFormattedTimeWithTimezone()`
- **Storage** – Storage mode queries, path helpers, cleanup and summary calls
- **Serial** – `serialWriteJsonAtomic()` for JSON output

---

### 10. Live Audio (`live_audio.cpp` / `live_audio.h`)

Streaming buffer for live audio over WebSocket.

- **PSRAM circular buffer** – ~1 second of mono 16-bit PCM
- **Drop on overrun** – Keeps newest samples
- **Client management** – Register/unregister WebSocket clients

---

### 11. NVS (`nvs.cpp` / `nvs.h`)

Thread-safe access to NVS (64 KB partition).

- **Namespaces** – `uploadq`, `logs`, `system`
- **Operations** – Read/write bytes, ints, strings with mutex protection
- **Health** – Basic metrics for diagnostics

---

## Task Architecture (FreeRTOS)

| Task             | Core | Priority | Purpose                                              |
|-----------------|------|----------|------------------------------------------------------|
| RecordTask      | 1    | 2        | Voice-activated recording, codec I/O                 |
| UploadTask      | 0    | 1        | Upload queue processing (PSRAM or SD)                |
| SerialTask      | 0    | 3        | Serial CLI, periodic config and short status         |
| MaintenanceTask | 0    | 1        | Health checks, cleanup, summaries, settings sync     |
| WebServer       | 0    | 2        | HTTP/WebSocket serving                               |
| loop()          | 1    | –        | `network_loop()`, `logger_tick()`, memory checks     |

Tasks are added to the task watchdog and monitored by MaintenanceTask; stopped tasks can be restarted.

---

## Data Flow

```
Microphone → Codec (ES8388) → Recorder
                                    ↓
                    ┌───────────────┴───────────────┐
                    │                              │
              SD card mode                   PSRAM mode
                    │                              │
                    ↓                              ↓
              /pending/*.wav              psramQueue (6 slots)
                    │                              │
                    └──────────────┬───────────────┘
                                   ↓
                          UploadTask
                                   ↓
                    HTTP POST → API /api/v2/audio/s3
                                   ↓
              Success: move to /inbox or release PSRAM
```

---

## Key Configuration (from `main.h` / `config.h`)

- **WiFi** – Up to 3 credential sets
- **Audio** – Sample rate (8 kHz), threshold (0–100), pre-record, min/max duration, codec gain
- **Upload** – Multiple API hosts/ports, per-endpoint enable
- **Storage** – SD card vs PSRAM, `recordToSdCard` flag
- **Timezone** – Offset (hours), maintenance hour/minute
- **Logging** – Per-level filters for serial and file

---

## Build Environment

- **Platform** – ESP32 (PlatformIO + Arduino framework)
- **Board** – `esp32dev` with PSRAM
- **Libraries** – AudioKit, RTClib, ESPAsyncWebServer, ArduinoJson, SD_MMC

---

## File Layout (Main Sources)

| File              | Component                         |
|-------------------|-----------------------------------|
| `main.cpp`        | Startup, loop, task creation      |
| `main.h`          | AppSettings, health metrics       |
| `recorder.cpp/h`  | Recorder                          |
| `upload_queue.cpp/h` | Upload queue                   |
| `network.cpp/h`   | Network                           |
| `settings.cpp/h`  | Settings                          |
| `common.cpp/h`    | Shared utilities                  |
| `health.cpp/h`    | Health & maintenance              |
| `timekeeper.cpp/h`| Time sync                         |
| `logger.cpp/h`    | Logging                           |
| `boondock_server.cpp/h` | Web server                 |
| `live_audio.cpp/h`    | Live streaming buffer        |
| `nvs.cpp/h`       | NVS persistence                   |
| `config.h`        | Build-time defaults               |
