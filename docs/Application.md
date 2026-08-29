# Application — System Startup & Task Architecture

## Overview

The application is an ESP32-based audio capture system running on FreeRTOS with two cores. On boot it initializes the ES8388 audio codec, creates all FreeRTOS tasks, and registers them with the watchdog timer. The system supports two storage modes: **SD card** (persistent files) and **PSRAM** (in-memory buffer).

| Task | Core | Priority | Stack | Role |
|------|------|----------|-------|------|
| RecordTask | 1 | 2 | 8 KB | Audio I/O and recording — see [Recorder.md](Recorder.md) |
| UploadTask | 0 | 1 | 12 KB | File uploads to API — see [Uploader.md](Uploader.md) |
| SerialTask | 0 | 3 | 8 KB | CLI and serial communication |
| MaintenanceTask | 0 | 1 | 8 KB | Health checks, cleanup, summaries |
| WebServerTask | 0 | 2 | 8 KB | Web UI and WebSocket events |

---

## Chart 1 — System Startup & Task Creation

```mermaid
flowchart TD
    A1[Boot ESP32] --> A2[startAudioCodec]
    A2 --> A2a[Configure ES8388<br/>ADC Line1 Input<br/>8kHz Sample Rate]
    A2a --> A2b[kit.begin — Initialize I2S]
    A2b --> A2c[kit.setMute true]
    A2c --> A3[updateCodecGainFromSettings<br/>Apply mic gain -1dB to +24dB]
    A3 --> A4[Create RecordTask<br/>xTaskCreatePinnedToCore<br/>Core 1 · Priority 2 · 8KB Stack]
    A4 --> A5[Create UploadTask<br/>xTaskCreatePinnedToCore<br/>Core 0 · Priority 1 · 12KB Stack]
    A5 --> A6[Create SerialTask + MaintenanceTask + WebServerTask]
    A6 --> A7[Register all tasks<br/>with Watchdog Timer]
    A7 --> A8[System Running]

    style A1 fill:#1a1a2e,stroke:#e94560,color:#fff
    style A2 fill:#0f3460,stroke:#53a8b6,color:#fff
    style A2a fill:#0f3460,stroke:#53a8b6,color:#fff
    style A2b fill:#0f3460,stroke:#53a8b6,color:#fff
    style A2c fill:#0f3460,stroke:#53a8b6,color:#fff
    style A3 fill:#0f3460,stroke:#53a8b6,color:#fff
    style A4 fill:#0a3d62,stroke:#38ada9,color:#fff
    style A5 fill:#0a3d62,stroke:#38ada9,color:#fff
    style A6 fill:#0a3d62,stroke:#38ada9,color:#fff
    style A7 fill:#16213e,stroke:#82ccdd,color:#fff
    style A8 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
```

---

## Chart 2 — RecordTask Main Loop

The RecordTask is the entry point into the recording pipeline. It gates on WiFi credentials — when none are configured (AP mode), the task idles and feeds the watchdog. Once credentials exist, it calls `monitorAndRecordAudio()` on every iteration (see [Recorder.md](Recorder.md) for that flow).

```mermaid
flowchart TD
    B1[RecordTask Entry] --> B2{WiFi Credentials<br/>Configured?}
    B2 -- No --> B3[AP Mode — No Recording]
    B3 --> B4[Feed Watchdog]
    B4 --> B5[Sleep 100ms]
    B5 --> B2

    B2 -- Yes --> B6[monitorAndRecordAudio<br/>→ See Recorder.md]
    B6 --> B7[Feed Watchdog<br/>every 20 iterations]
    B7 --> B8[vTaskDelay 1ms]
    B8 --> B2

    style B1 fill:#16213e,stroke:#0f3460,color:#fff
    style B2 fill:#1a1a2e,stroke:#e94560,color:#fff
    style B3 fill:#b71540,stroke:#e94560,color:#fff
    style B6 fill:#0a3d62,stroke:#38ada9,color:#fff
```

---

## Default Audio Settings

| Setting | Default | Purpose |
|---------|---------|---------|
| Sample rate | 8000 Hz | Low bandwidth mono voice capture |
| Threshold | 50 (maps to -40 dB) | Sound detection sensitivity |
| Pre-record | 200 ms | Captures audio before trigger |
| Min recording | 1000 ms | Prevents ultra-short clips |
| Max recording | 30000 ms | Hard recording length cap |
| Silence threshold | 1000 ms | How long silence before stopping |
| Codec gain | 0 dB | ES8388 mic amplification |
| dB smoothing | 20-sample window | Noise-resistant threshold detection |
