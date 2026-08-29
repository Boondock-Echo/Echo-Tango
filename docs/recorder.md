# Recorder — Audio Capture, Detection & Recording

## Overview

The recorder handles everything from reading I2S audio samples to detecting sound, managing recordings, and finalizing WAV files. It runs inside `monitorAndRecordAudio()` which is called by the RecordTask loop (see [Application.md](Application.md)).

Key mechanisms:
- **ES8388 audio codec** via AudioKit HAL (8kHz, 16-bit, stereo input split to mono)
- **Pre-record ring buffer** (500ms) so you never miss the start of sound
- **dB smoothing** (20-sample sliding window) for noise-resistant threshold detection
- **Dual storage paths** — PSRAM (in-memory, max 30s) or SD card (persistent .wav files)

**ECHO MQTT remote control:** See [MQTT_RECORD_LINE_IN.md](MQTT_RECORD_LINE_IN.md) for the `record_line_in` command (enable/disable VOX recording over MQTT).

---

## Chart 1 — Audio Capture & Sound Detection

```mermaid
flowchart TD
    C1[kit.read — Read 1024<br/>stereo samples from I2S<br/>200ms timeout] --> C2{bytesRead > 0?}
    C2 -- No --> C_RET[Return]
    C2 -- Yes --> C3[splitAudioBuffer<br/>Stereo → Mono<br/>512 mono samples]
    C3 --> C4{monoSamples > 0?}
    C4 -- No --> C_RET
    C4 -- Yes --> M1

    M1[calculateAudioLevel<br/>RMS Level 0–100%] --> M2[calculateDb<br/>Raw dB from samples]
    M2 --> M3[pushDbAndGetAverage<br/>Sliding window of 20 chunks]
    M3 --> M4[calculatePeakSample<br/>+ dynamic range utilization]
    M4 --> M5[Update VU Meter Stats<br/>min / max / avg tracking]
    M5 --> M6[mapSensitivityToDb<br/>Setting 0 → -20dB<br/>Setting 50 → -40dB<br/>Setting 100 → -80dB]
    M6 --> M7{smoothedDb ≥ threshold<br/>AND level > 0?}

    M7 -- "No Sound" --> PS1[pushPreRecordSamples<br/>Feed 500ms ring buffer]
    PS1 --> C_RET

    M7 -- "Sound Detected" --> DEC{Currently<br/>Recording?}
    DEC -- No --> START[startRecording ↓ Chart 2 or 3]
    DEC -- Yes --> APPEND[appendAudioSamples ↓ Chart 4]

    style C1 fill:#1a1a2e,stroke:#533483,color:#fff
    style M1 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M2 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M3 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M4 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M5 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M6 fill:#0f3460,stroke:#53a8b6,color:#fff
    style M7 fill:#1a1a2e,stroke:#e94560,color:#fff
    style PS1 fill:#16213e,stroke:#82ccdd,color:#fff
    style START fill:#38ada9,stroke:#0a3d62,color:#1a1a2e
    style APPEND fill:#b71540,stroke:#e94560,color:#fff
```

---

## Chart 2 — Start Recording (PSRAM Mode)

```mermaid
flowchart TD
    SR0[startRecording called] --> SR1[ensureStorage]
    SR1 --> SR2{Storage Mode<br/>= PSRAM?}
    SR2 -- No --> SD_LINK[→ See Chart 3 — SD Card]

    SR2 -- Yes --> P1[Log: Starting PSRAM recording<br/>max_bytes · queue_slots]
    P1 --> P2{psramQueue<br/>_getAvailableSlots<br/>> 0?}
    P2 -- No --> P3[psramQueue_dropOldestEntry<br/>Free oldest completed recording<br/>to make room]
    P3 --> P4
    P2 -- Yes --> P4[Calculate max buffer size<br/>min of maxRecordingMs and 30s cap<br/>× sampleRate × 2 bytes]
    P4 --> P5[Cap at 480,000 bytes<br/>kPsramRecordingMaxBytes]
    P5 --> P6[heap_caps_malloc SPIRAM<br/>Allocate WAV header 44B<br/>+ audio buffer up to 480KB]
    P6 --> P7{Allocation<br/>Succeeded?}
    P7 -- No --> FAIL[Log error<br/>Return — Cannot Record]
    P7 -- Yes --> P8[psramRecordingCapacity = maxBytes<br/>psramRecordingOffset = 44<br/>Leave space for WAV header]
    P8 --> P9[currentRecordingPath = PSRAM]
    P9 --> COMMON

    COMMON[isRecording = true<br/>recordingStartMs = now<br/>lastSoundMs = now<br/>time &recordingStartEpoch] --> PR{prependPreRecord<br/>enabled?}
    PR -- Yes --> PR1[appendPreRecordAudio]
    PR1 --> PR2[copyRecentPreRecordSamples<br/>from 500ms ring buffer]
    PR2 --> PR3[appendAudioSamples<br/>Write pre-record to PSRAM buffer]
    PR3 --> PR4[Adjust recordingStartMs<br/>backwards by applied ms]
    PR4 --> EVT
    PR -- No --> EVT

    EVT[Send record_begin<br/>WebSocket event<br/>path · timestamp · sample_rate] --> DONE[Recording Active<br/>→ Chart 4 continues]

    style SR0 fill:#0a3d62,stroke:#38ada9,color:#fff
    style SR2 fill:#1a1a2e,stroke:#e94560,color:#fff
    style P3 fill:#b71540,stroke:#e94560,color:#fff
    style P6 fill:#16213e,stroke:#82ccdd,color:#fff
    style P8 fill:#16213e,stroke:#82ccdd,color:#fff
    style COMMON fill:#0f3460,stroke:#53a8b6,color:#fff
    style PR1 fill:#533483,stroke:#b8e994,color:#fff
    style PR2 fill:#533483,stroke:#b8e994,color:#fff
    style EVT fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style FAIL fill:#b71540,stroke:#e94560,color:#fff
    style SD_LINK fill:#16213e,stroke:#0f3460,color:#fff
    style DONE fill:#78e08f,stroke:#38ada9,color:#1a1a2e
```

---

## Chart 3 — Start Recording (SD Card Mode)

```mermaid
flowchart TD
    SR0[startRecording called] --> SR1[ensureStorage]
    SR1 --> SR2{Storage Mode<br/>= SD Card?}
    SR2 -- No --> PSRAM_LINK[→ See Chart 2 — PSRAM]

    SR2 -- Yes --> S1{/pending<br/>directory exists?}
    S1 -- No --> S1a[SD_MMC.mkdir /pending]
    S1a --> S1b{mkdir OK?}
    S1b -- No --> FAIL[Log error<br/>Return — Cannot Record]
    S1b -- Yes --> S2
    S1 -- Yes --> S2[createPendingRecordingPath]

    S2 --> S3{Epoch time<br/>valid?}
    S3 -- Yes --> S4[Generate timestamp path<br/>/pending/YYYY-MM-DD-HH-MM-SS.tmp]
    S3 -- No --> S5[Fallback to millis path<br/>/pending/rec_123456.tmp]
    S4 --> S6
    S5 --> S6[SD_MMC.open path FILE_WRITE]

    S6 --> S7[Retry Loop<br/>up to 3 attempts]
    S7 --> S8{File Opened?}
    S8 -- "No — retry" --> S9[delay 100ms × attempt<br/>Record write error]
    S9 --> S7
    S8 -- "No — all failed" --> FAIL
    S8 -- Yes --> S10[Write placeholder WAV header<br/>makeWaveHeader 0 bytes, sampleRate<br/>44 bytes to file]
    S10 --> S11[file.flush]
    S11 --> S12{Header write<br/>= 44 bytes?}
    S12 -- No --> S13[Close file<br/>Record write error]
    S13 --> FAIL
    S12 -- Yes --> COMMON

    COMMON[isRecording = true<br/>recordingStartMs = now<br/>lastSoundMs = now<br/>time &recordingStartEpoch] --> PR{prependPreRecord<br/>enabled?}
    PR -- Yes --> PR1[appendPreRecordAudio]
    PR1 --> PR2[copyRecentPreRecordSamples<br/>from 500ms ring buffer]
    PR2 --> PR3[appendAudioSamples<br/>Write pre-record to .tmp file]
    PR3 --> PR4[Adjust recordingStartMs<br/>backwards by applied ms]
    PR4 --> EVT
    PR -- No --> EVT

    EVT[Send record_begin<br/>WebSocket event<br/>path · timestamp · sample_rate] --> DONE[Recording Active<br/>→ Chart 4 continues]

    style SR0 fill:#0a3d62,stroke:#38ada9,color:#fff
    style SR2 fill:#1a1a2e,stroke:#e94560,color:#fff
    style S2 fill:#16213e,stroke:#82ccdd,color:#fff
    style S6 fill:#16213e,stroke:#82ccdd,color:#fff
    style S7 fill:#1a1a2e,stroke:#e58e26,color:#fff
    style S10 fill:#16213e,stroke:#82ccdd,color:#fff
    style COMMON fill:#0f3460,stroke:#53a8b6,color:#fff
    style PR1 fill:#533483,stroke:#b8e994,color:#fff
    style PR2 fill:#533483,stroke:#b8e994,color:#fff
    style EVT fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style FAIL fill:#b71540,stroke:#e94560,color:#fff
    style PSRAM_LINK fill:#16213e,stroke:#0f3460,color:#fff
    style DONE fill:#78e08f,stroke:#38ada9,color:#1a1a2e
```

---

## Chart 4 — Active Recording & Stop Conditions

```mermaid
flowchart TD
    A[appendAudioSamples] --> MODE{Storage Mode?}

    MODE -- PSRAM --> PM1[memcpy to PSRAM buffer]
    PM1 --> PM2{Buffer Full?}
    PM2 -- Yes --> PM3[Write remaining<br/>space only — truncate]
    PM3 --> SND
    PM2 -- No --> SND

    MODE -- SD Card --> SD1[currentRecordingFile.write<br/>3 retries + flush]
    SD1 --> SND

    SND{Sound in<br/>this chunk?} -- Yes --> SND1[lastSoundMs = now]
    SND1 --> CHK
    SND -- No --> CHK

    CHK[Calculate elapsed time] --> SC1{elapsed ≥ minRecordingMs<br/>AND silence ≥<br/>silenceThresholdMs?}

    SC1 -- "Yes & NOT sample recording" --> STOP1[finalizeRecording<br/>reason: silence ↓ Chart 5]

    SC1 -- No --> SC2{elapsed ≥<br/>maxRecordingMs?}
    SC2 -- Yes --> STOP2[finalizeRecording<br/>reason: max_duration ↓ Chart 5]
    STOP2 --> CHAIN{Sound still active<br/>AND NOT sample?}
    CHAIN -- Yes --> RESTART[startRecording<br/>Chain new recording<br/>without pre-record]
    CHAIN -- No --> PRE

    SC2 -- No --> PRE[pushPreRecordSamples<br/>Keep ring buffer fed]
    STOP1 --> PRE

    PRE --> LIVE{liveAudioCallback set?}
    LIVE -- Yes --> FEED[Feed Live Audio<br/>WebSocket stream]
    FEED --> DONE[Return]
    LIVE -- No --> DONE

    style A fill:#b71540,stroke:#e94560,color:#fff
    style MODE fill:#1a1a2e,stroke:#e94560,color:#fff
    style PM1 fill:#16213e,stroke:#82ccdd,color:#fff
    style SD1 fill:#16213e,stroke:#82ccdd,color:#fff
    style SC1 fill:#1a1a2e,stroke:#e58e26,color:#fff
    style SC2 fill:#1a1a2e,stroke:#e58e26,color:#fff
    style STOP1 fill:#0a3d62,stroke:#78e08f,color:#fff
    style STOP2 fill:#0a3d62,stroke:#78e08f,color:#fff
    style RESTART fill:#38ada9,stroke:#0a3d62,color:#1a1a2e
    style FEED fill:#533483,stroke:#b8e994,color:#fff
```

---

## Chart 5 — Finalize Recording

```mermaid
flowchart TD
    F0[finalizeRecording called] --> F1[Calculate duration<br/>from recordedBytes]
    F1 --> F2{Storage Mode?}

    F2 -- PSRAM --> PA{Discard small files<br/>enabled AND<br/>duration < minMs?}
    PA -- Yes --> PD[heap_caps_free<br/>Discard recording]
    PD --> STATS
    PA -- No --> PB[Write WAV header<br/>at buffer offset 0]
    PB --> PC[psramQueue_addRecording<br/>Max 6 queue entries]
    PC --> PF{Queue Full?}
    PF -- Yes --> PG[heap_caps_free<br/>Drop — log error]
    PG --> STATS
    PF -- No --> STATS

    F2 -- SD Card --> SA[Seek to file start<br/>Write final WAV header]
    SA --> SB[Close file]
    SB --> SC[Rename .tmp → .wav]
    SC --> SD{Discard small files<br/>enabled AND<br/>duration < minMs?}
    SD -- Yes --> SE[SD_MMC.remove<br/>Delete file]
    SE --> STATS
    SD -- No --> SF[sdCardMemoryQueue_addRecording<br/>In-memory priority queue<br/>max 20 entries]
    SF --> STATS

    STATS[Update Session Stats<br/>recordingCount++<br/>totalDurationMs += duration] --> EVT[Send record_end event<br/>via WebSocket<br/>path · duration · peakDb · reason]
    EVT --> RESET[Reset State<br/>isRecording = false<br/>recordedBytes = 0<br/>resetSoundDetection]

    style F0 fill:#0a3d62,stroke:#78e08f,color:#fff
    style F2 fill:#1a1a2e,stroke:#e94560,color:#fff
    style PB fill:#16213e,stroke:#82ccdd,color:#fff
    style SA fill:#16213e,stroke:#82ccdd,color:#fff
    style SC fill:#16213e,stroke:#82ccdd,color:#fff
    style SF fill:#0f3460,stroke:#53a8b6,color:#fff
    style STATS fill:#0f3460,stroke:#53a8b6,color:#fff
    style EVT fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style RESET fill:#1a1a2e,stroke:#533483,color:#fff
    style PD fill:#b71540,stroke:#e94560,color:#fff
    style SE fill:#b71540,stroke:#e94560,color:#fff
    style PG fill:#b71540,stroke:#e94560,color:#fff
```

---

## Chart 6 — Live Audio Streaming

Firmware streams mic PCM over the websocket **independent of whether a WAV recording is active**. After each `kit.read` + mono split + level smoothing, **`liveAudioCallback`** may run (**not** gated on `isSound`—silence and sub-threshold audio are included). The recorder only invokes that callback while **`recorder_setLiveAudioFeedEnabled(true)`** (set by the websocket layer when **any** client is subscribed to the `live-audio` page). The lambda registered in **`boondock_server.cpp`** copies samples into a PSRAM ring; **`boondock_server_pushLiveAudio()`** (main loop timing) sends base64-encoded chunks to subscribed clients.

```mermaid
flowchart TD
    L1[I2S read + split to mono chunk] --> L2{"liveAudioFeedEnabled<br/>AND callback set?"}
    L2 -- No --> L_END[Skip ring write]
    L2 -- Yes --> L3[liveAudioCallback<br/>writes PSRAM ring in boondock_server]
    L3 --> L4[Circular Buffer<br/>1 sec at 8 kHz mono 16‑bit<br/>overwrite on overflow]
    L4 --> L5["boondock_server_loop<br/>~1 Hz: pushLiveAudio"]
    L5 --> L6{"Any WS subscriber<br/>page live-audio?"}
    L6 -- No --> L7[Narrowcast idle —<br/>recorder ring not fed]
    L6 -- Yes --> L8["JSON WS message<br/>type live-audio · base64 PCM"]

    style L1 fill:#1a1a2e,stroke:#b8e994,color:#fff
    style L3 fill:#533483,stroke:#b8e994,color:#fff
    style L4 fill:#16213e,stroke:#82ccdd,color:#fff
    style L5 fill:#0a3d62,stroke:#38ada9,color:#fff
    style L8 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
```

---

## Key Flow Summary

| Phase | Function | What Happens |
|-------|----------|-------------|
| **Read** | `kit.read()` | 1024 stereo samples read from I2S into `audioBuffer` |
| **Split** | `splitAudioBufferForRecording()` | Stereo demuxed to 512 mono samples in `recordingBuffer` |
| **Analyze** | `calculateDb()` + `pushDbAndGetAverage()` | Raw dB computed, then smoothed over 20-sample window |
| **Detect** | Threshold comparison | Smoothed dB vs `mapSensitivityToDb(threshold)` where setting 50 = -40 dB |
| **Pre-buffer** | `pushPreRecordSamples()` | 500ms ring buffer always fed so recording starts *before* sound was detected |
| **Live stream** | `liveAudioCallback` (+ `liveAudioFeedEnabled`) | Every mono chunk after analysis (silence included); ring + `pushLiveAudio` when WS clients subscribe to `live-audio` |
| **Start** | `startRecording()` | PSRAM buffer allocated or SD .tmp file opened; pre-record audio prepended |
| **Capture** | `appendAudioSamples()` | PSRAM: memcpy; SD: file write with 3 retries + flush |
| **Stop** | Silence or max duration | Silence: `minRecordingMs` elapsed + `silenceThresholdMs` of quiet; Max: `maxRecordingMs` hard cap (default 30s) |
| **Finalize** | `finalizeRecording()` | WAV header written, file renamed .tmp → .wav, queued for upload |
| **Chain** | Auto-restart on max duration | If sound still active when max reached, immediately starts a new recording (no pre-record) |
