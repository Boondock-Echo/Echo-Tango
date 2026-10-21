# SD Card Recording & Upload — Recommendations

This document captures **holistic recommendations** for managing recordings when using **SD card storage**, the **upload pipeline**, and the **web UI**. It complements the write-path focus of [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md).

**Goals:** keep recording reliable, drain uploads to the server, and avoid misleading “queue full” behavior when the device is otherwise healthy.

---

## Current implementation (per-day SD `upload_list`)

- **Pending names:** Each day's folder holds append-only `upload_list` plus `upload_list.idx` (line cursor). The WAV files stay on SD. Firmware does not keep the backlog in RAM. See [PENDING_UPLOAD_LIST.md](./PENDING_UPLOAD_LIST.md).
- **Hot path:** After the 30 s boot delay, the upload task reads the newest day's list one name at a time.
- **Overflow:** There is no in-memory slot cap; a large backlog only grows the on-card lists.
- **Legacy cards:** A day folder with WAVs but no list is seeded into `upload_list` on first visit.
- **`markUploadedWithRecord`:** Uses the same **`g_pendingDirMutex`** serialization as `markUploaded`.
- **Observability:** `[UploadTask] skip reason=…` (debug) for `wifi`, `no_credentials`, `sd_unavailable`, `startup_delay`, `queue_empty`, `mutex_timeout`, `open_failed`. Upload failures log `upload failed reason=<network_getLastUploadFailureReason()> path=…` (see `network.cpp`).
- **SD recorder write path:** Audio writes **batch `flush()`** (400 ms or 32 KiB) with **flush on finalize**; **`recordings` JSONL** gets **`sizeBytes`** from `sizeof(WaveHeader)+recordedBytes` (no extra read-open); **session cache** skips repeat **`/pending`** and same-day **`storage_ensureDirectoryPath`** until SD failure hooks clear it. Details: [SD_CARD_IO_OPTIMIZATION.md §1](./SD_CARD_IO_OPTIMIZATION.md#1-recording-path-largest-write-amplification).
- **Upload queue / metrics:** SPIRAM basename queue first; **empty** filesystem fallback scans throttled to **≥ 5 s** apart; **`uploadQueue_getPendingCount()`** uses a **2 s TTL** cache with explicit invalidation. [SD_CARD_IO_OPTIMIZATION.md §2](./SD_CARD_IO_OPTIMIZATION.md#2-upload-queue-and-pending-counts).

---

## 1. Why PSRAM mode feels solid but SD + UI does not

| Aspect | PSRAM recording | SD card recording |
|--------|-----------------|-------------------|
| During capture | Audio stays in RAM; no filesystem I/O | Continuous writes to `/pending/.../*.tmp` |
| Queue | Small in-RAM queue with **bounded** slots; can **drop oldest** under pressure | SPIRAM **basename** queue (50) + **filesystem** scan as fallback; `uploadQueue_getPendingCount()` still walks `/pending` for disk backlog |
| Upload source | Memory buffers | Known path from basename queue, or scan-selected file |
| Contention | Upload task vs recorder is mostly separate | **Recorder, upload task, and web playback** all use the SD card |

Under frequent or back-to-back recordings, SD mode can accumulate files faster than the upload scheduler drains them, especially when the **UI** adds concurrent reads. That can look like “upload queue full” and “uploads stopped” even though recording continues.

---

## 2. Root causes (remaining / historical)

### 2.1 Filesystem fallback and startup delay

The **full-tree** fallback and **30 s** startup window (SPIRAM-only) can still delay draining **orphan** files on disk. The SPIRAM hot path no longer uses the old **recording** or **5 s inter-upload** gates.

### 2.2 SPIRAM queue (50 entries) and disk backlog

`sdCardMemoryQueue_addRecording()` drops the **oldest** slot when full; files remain on SD for fallback scan. **`uploadQueue_getPendingCount()`** still reflects disk backlog (can exceed the 50-slot SPIRAM list) but uses a **2 s TTL cache** with invalidation on recording/upload changes—see [SD_CARD_IO_OPTIMIZATION.md §2](./SD_CARD_IO_OPTIMIZATION.md#2-upload-queue-and-pending-counts).

### 2.3 Full-tree scans and a single pending-directory mutex

`uploadQueue_getNextFile()` and `uploadQueue_getPendingCount()` still recurse `/pending` when they run. **Mitigations in firmware:** memory-queue-first upload; **5 s minimum** between **empty** fallback scans; **cached** pending count for UI/health. Contention with recorder **rename** under `g_pendingDirMutex` remains; short mutex timeouts can still yield **undercounted** pending counts.

**Recommendation:** Long term, replace **O(n) full scans** with a **cursor** or **append-only journal** for “next to upload.” Short term: **split metrics** (disk vs memory) and log **mutex timeout** explicitly.

### 2.4 Web UI playback vs recorder / uploader

Streaming WAVs from SD (`/api/recordings/stream` in `boondock_server.cpp`) interleaves **long reads** with **writes** (recording) and **reads** (upload). All of those paths take **`sd_bus` per syscall** (see [SD_BUS_LOCK.md](./SD_BUS_LOCK.md)); do not add raw `SD_MMC` in the web server.

**Recommendation:** Keep streaming chunked and unlocked across Wi‑Fi. Optional: **rate-limit** or **pause** streaming when upload/recorder pressure is high.

### 2.5 Concurrency on successful upload

**Done:** `uploadQueue_markUploadedWithRecord()` now takes **`g_pendingDirMutex`** around move + index append, matching `markUploaded`.

---

## 3. Prioritized recommendations

### Priority A — Scheduling and throughput

1. **Done:** completed `.wav` files may upload while another recording is in progress. `getNextFile()` is no longer gated on `recorder_isRecording()`; only the live `.tmp` is excluded (it is never on `upload_list`).
2. **Revisit** `kSdCardFileUploadDelayMs` — a fixed multi-second gap **caps** drain rate; tune for SD fairness vs backlog.
3. After each **record_end**, optionally **trigger a burst** of one or more upload attempts before the next idle delay.

### Priority B — Queue semantics and UX

1. **Expose two metrics** (or subfields): **pending on disk** vs **in-memory priority slots**, instead of a single `uploadQueue` number when the two behave differently.
2. On **SdCardMemoryQueue full**, **drop oldest in-memory** reference (file stays on disk) so new clips always get a slot if that matches product expectations.

### Priority C — Scalability

1. Replace **recursive full-tree** “next file” / “count” with a **persistent cursor** or **daily index** under `/pending` to shorten lock hold time.
2. Optional **cap** on total `/pending` size or file count with **oldest-first** move to `/trash` or policy-driven deletion (with clear logging).

### Priority D — Recorder write path (see existing doc)

Follow [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md) for **partial writes**, **flush** rate, **runtime SD failure → PSRAM**, and **file handle** recovery — these reduce corrupt files and unnecessary load on the card.

### Priority E — Observability

**Partially implemented:** skip reasons and `network_getLastUploadFailureReason()` after failed `uploadAudioFile()`. Server/API error strings propagate via the existing upload loop (`lastAttemptError`).

---

## 4. Summary

SD uploads now prioritize a **SPIRAM basename queue** so the steady path avoids **full-tree scans**; recording and uploads can proceed **concurrently**, and **`markUploadedWithRecord`** is mutex-aligned with pending-dir operations. Remaining pressure points: **filesystem fallback** cost, **30 s** startup behavior, **UI** streaming vs SD, and **recorder write-path** hardening (see [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md)).

---

## 5. Related files (implementation reference)

| Area | Primary files |
|------|----------------|
| Upload task, SD gating | `src/main.cpp` |
| SPIRAM basename queue, `buildFullPath`, pending mutex | `src/upload_queue.cpp`, `src/upload_queue.h` |
| Upload failure reason string | `network_getLastUploadFailureReason()` in `src/network.cpp` |
| Finalize, enqueue | `src/recorder.cpp` |
| Queue size for UI | `system_getUploadQueueSize()` in `src/main.cpp` |
| Web playback | `src/boondock_server.cpp` (recordings stream handler) |
| Write-path analysis | [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md) |
