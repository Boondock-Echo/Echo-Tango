# Uploader — Upload Pipeline & Queue Management

## Overview

The UploadTask runs on **Core 0** (priority 1) and is responsible for taking completed recordings and uploading them to the cloud API via HTTP POST. It supports two storage modes with a prioritized queue system:

- **PSRAM mode** — polls the in-memory PSRAM queue, uploads, and frees the buffer
- **SD card mode** — pending names live on the card in each day's append-only `upload_list`, with progress in `upload_list.idx` (see [PENDING_UPLOAD_LIST.md](./PENDING_UPLOAD_LIST.md)); the upload task reads one name at a time. There is no in-RAM backlog of filenames.

After successful upload, SD card files are moved from `/pending` to `/inbox` and an entry is appended to the daily `index.json`.

---

## Chart 1 — Upload Pipeline (PSRAM Mode)

```mermaid
flowchart TD
    U0[UploadTask Entry<br/>Core 0 · Priority 1] --> U1{Storage Mode<br/>= PSRAM?}
    U1 -- No --> SD_LINK[→ See Chart 2 — SD Card]

    U1 -- Yes --> Q1[Poll PSRAM Queue<br/>psramQueue_getNextEntry]
    Q1 --> Q2{Entry Available?}
    Q2 -- No --> Q3[Sleep & Retry]
    Q3 --> Q1

    Q2 -- Yes --> Q4[Build upload request<br/>from PSRAM buffer<br/>data · size · duration · peakDb]
    Q4 --> Q5[uploadAudioFile<br/>HTTP POST to API endpoint]
    Q5 --> Q6{Upload OK?}

    Q6 -- Yes --> Q7[psramQueue_releaseEntry<br/>Free PSRAM buffer]
    Q7 --> Q8[recorder_incrementUploadedCount]
    Q8 --> Q1

    Q6 -- No --> Q9[recorder_incrementErrorCount]
    Q9 --> Q10{Sustained failure?<br/>No success for extended time}
    Q10 -- Yes --> Q11[Log sustained failure error]
    Q11 --> Q12[Backoff delay]
    Q10 -- No --> Q12[Backoff delay]
    Q12 --> Q1

    style U0 fill:#16213e,stroke:#82ccdd,color:#fff
    style U1 fill:#1a1a2e,stroke:#e94560,color:#fff
    style Q4 fill:#0f3460,stroke:#53a8b6,color:#fff
    style Q5 fill:#0a3d62,stroke:#38ada9,color:#fff
    style Q7 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style Q8 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style Q9 fill:#b71540,stroke:#e94560,color:#fff
    style SD_LINK fill:#16213e,stroke:#0f3460,color:#fff
```

---

## Chart 2 — Upload Pipeline (SD Card Mode)

```mermaid
flowchart TD
    U0[UploadTask Entry<br/>Core 0 · Priority 1] --> U1{Storage Mode<br/>= SD Card?}
    U1 -- No --> PSRAM_LINK[→ See Chart 1 — PSRAM]

    U1 -- Yes --> SD0{Startup Delay<br/>Active?}

    SD0 -- Yes --> SD1b[Wait for startup<br/>delay to complete]
    SD1b --> SD0

    SD0 -- No --> SD4[Read today upload_list by UTC path<br/>fallback: tree walk for older days]
    SD4 --> SD5{Name Found?}
    SD5 -- No --> SD6[Sleep & Poll]
    SD6 --> SD0
    SD5 -- Yes --> SD7[Open file<br/>Build upload request]

    SD7 --> SD8[uploadAudioFile<br/>HTTP POST to API endpoint]

    SD8 --> SD9{Upload OK?}

    SD9 -- Yes --> SD10[uploadQueue_markUploaded<br/>Move file /pending → /inbox]
    SD10 --> SD11[uploadQueue_appendToIndex<br/>Add record to daily index.json]
    SD11 --> SD12[recorder_incrementUploadedCount]
    SD12 --> SD13[500ms cooldown; defer events]
    SD13 --> SD0

    SD9 -- No --> SD14[File stays in /pending<br/>Available for retry]
    SD14 --> SD15[recorder_incrementErrorCount]
    SD15 --> SD16[Backoff delay]
    SD16 --> SD0

    style U0 fill:#16213e,stroke:#82ccdd,color:#fff
    style U1 fill:#1a1a2e,stroke:#e94560,color:#fff
    style SD4 fill:#0f3460,stroke:#53a8b6,color:#fff
    style SD7 fill:#0a3d62,stroke:#38ada9,color:#fff
    style SD8 fill:#0a3d62,stroke:#38ada9,color:#fff
    style SD10 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style SD11 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style SD12 fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style SD14 fill:#b71540,stroke:#e94560,color:#fff
    style SD15 fill:#b71540,stroke:#e94560,color:#fff
    style PSRAM_LINK fill:#16213e,stroke:#0f3460,color:#fff
```

---

## Upload Queue Architecture

| Queue | Storage | Capacity | Source | Priority |
|-------|---------|----------|--------|----------|
| PSRAM Queue | PSRAM heap | 6 entries × ~480 KB each | `finalizeRecording()` in PSRAM mode | Only queue in PSRAM mode |
| Per-day `upload_list` | SD `/pending/YYYY/MM/DD/upload_list` | Unlimited (disk space) | `uploadList_addPending()` after finalize | Only SD pending-name source |

## File Lifecycle (SD Card Mode)

```mermaid
flowchart LR
    A[Recording starts<br/>/pending/YYYY-MM-DD-HH-MM-SS.tmp] --> B[Recording ends<br/>Rename .tmp → .wav]
    B --> C[Basename appended to<br/>that day's upload_list]
    C --> D[UploadTask picks up file]
    D --> E{Upload OK?}
    E -- Yes --> F[Move to /inbox/YYYY/MM/DD/<br/>Append to index.json]
    E -- No --> G[File stays in /pending<br/>Retry on next pass]

    style A fill:#1a1a2e,stroke:#e58e26,color:#fff
    style B fill:#16213e,stroke:#82ccdd,color:#fff
    style C fill:#0f3460,stroke:#53a8b6,color:#fff
    style D fill:#0a3d62,stroke:#38ada9,color:#fff
    style F fill:#78e08f,stroke:#38ada9,color:#1a1a2e
    style G fill:#b71540,stroke:#e94560,color:#fff
```

---

## Network-friendly upload behavior

Under continuous recording the ESP32 WiFi stack can run out of TCP send buffers (`tcp_write 0/4096`, then `errno 119`). Mitigations:

| Setting | Value | Purpose |
|---------|-------|---------|
| `UPLOAD_TCP_CHUNK_SIZE` | 1024 bytes | Smaller `WiFiClient::write()` calls; less pressure per syscall |
| Chunk yield | every 2 chunks | Brief `vTaskDelay(1)` so LwIP/WiFi can drain |
| `UPLOAD_INTER_FILE_COOLDOWN_MS` | 500 ms | Pause after each successful upload before the next file |
| Event drain | idle only | While `handleUploadOne()` uploaded a file, **no** event TCP — avoids competing sockets during backlog drain |
| Cloud-path WiFi reconnect | blocked while uploading | `network_reconnectWiFi()` and `cloudPathMaybeRecover()` skip if `networkHandler_isUploading()` — prevents reason-8 disconnect mid-POST (see log `06:44:14`) |

Cloud-path recovery still runs from `network_loop()` when uploads are idle; deferred reconnects wait until the current POST finishes.

---

## API Endpoints

| Endpoint | Path | Purpose |
|----------|------|---------|
| Audio Upload | `/api/v2/audio/s3` | Upload WAV file via HTTP POST |
| Events | `/api/v1/events` | Send system events (record_begin, record_end, etc.) |
| Log Upload | `/api/V1/upload/logs` | Upload log files |
| Firmware Check | `/api/v1/firmware/check` | OTA firmware update check |

Default API hosts (3 endpoints with failover):
- `3.128.235.120:7001`
- `35.85.105.6:7001`
- `52.1.103.236:7001`
