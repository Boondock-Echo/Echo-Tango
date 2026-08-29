# SD Card I/O — Optimization Recommendations

This document suggests how to **reduce SD reads and writes** (wear, latency, and contention with recording/uploads). It complements [SD_CARD_RECORDING_RECOMMENDATIONS.md](./SD_CARD_RECORDING_RECOMMENDATIONS.md) (queue/upload architecture) and [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md) (recorder write-path reliability).

**Goal:** fewer filesystem operations for the same functionality, with explicit tradeoffs where durability or freshness would change.

---

## 1. Recording path (largest write amplification)

### Implemented

| Item | Behavior |
|------|------------|
| **Coalesced `flush`** | In SD mode, audio writes call `flush()` only when **≥ 400 ms** since the last flush **or** **≥ 32 KiB** written since the last flush. The placeholder WAV header write still flushes once at session start; **finalize** always seeks, writes the real header, **flush**, and **close**. Fewer FAT/metadata updates than flushing every codec chunk; **tradeoff:** more unsynced audio after an abrupt power cut between flushes. |
| **Summary `sizeBytes` without `open`** | After finalize, the recordings catalog line uses **`sizeof(WaveHeader) + recordedBytes`** (PCM bytes only in `recordedBytes`), matching the closed `.wav` file—no `SD_MMC.open(..., FILE_READ)` just to read `size()`. |
| **Pending path session cache** | **`/pending`:** first successful recording pass ensures the root exists, then **`SD_MMC.exists` / `mkdir`** for `/pending` is skipped until **`recorder_invalidatePendingDirectoryCache()`** (invoked from **`storage_switchToPsramOnFailure`** and **`storage_permanentlyDisableSdCard`**). **Day folder:** if the parent path (e.g. `/pending/YYYY/MM/DD`) matches the last ensured path, **`storage_ensureDirectoryPath`** is skipped. A new calendar day or a new boot naturally uses a different path. |

**Code:** [`src/recorder.cpp`](../src/recorder.cpp) (`appendAudioSamples`, `startRecording`, `finalizeRecording`), [`src/common.cpp`](../src/common.cpp) (storage hooks calling `recorder_invalidatePendingDirectoryCache`).

---

## 2. Upload queue and pending counts

### Implemented

| Item | Behavior |
|------|------------|
| **Hot path vs filesystem fallback** | Upload task still drains the **SPIRAM basename queue** first. **Filesystem** `uploadQueue_getNextFile()` runs only when the memory queue is empty. **Rate limit:** if that scan finds **no** `.wav`, the next full-tree scan is deferred **5 s** (`kFsFallbackEmptyScanMinIntervalMs` in [`main.cpp`](../src/main.cpp)); finding a file or successfully dequeuing from the memory queue clears the deferral so backlogs drain quickly. |
| **Pending `.wav` count cache** | `uploadQueue_getPendingCount()` returns a **cached** recursive count for **2 s** (`kFsPendingWavCountCacheTtlMs` in [`upload_queue.cpp`](../src/upload_queue.cpp)) unless **`uploadQueue_invalidateFilesystemPendingCountCache()`** runs. Invalidate on **upload complete** (`markUploaded` / `markUploadedWithRecord`), **`uploadQueue_begin`**, and **recorder finalize** when a `/pending` **`.wav`** is created or a small-file discard removes one. Memory-queue slot count (`sdCardMemoryQueue_getPendingCount`) is always computed live in `system_getUploadQueueSize()`. |

**API:** `void uploadQueue_invalidateFilesystemPendingCountCache()` in [`upload_queue.h`](../src/upload_queue.h).

---

## 3. Web UI and playback

### Implemented

| Item | Behavior |
|------|------------|
| **Folders API TTL cache** | `GET /api/recordings/folders` caches the JSON response per canonical path for **5 s** (`kRecordingsFoldersCacheTtlMs` in [`src/boondock_server.cpp`](../src/boondock_server.cpp)). Rapid UI refreshes reuse the last response without re-enumerating the SD directory. **Tradeoff:** new subfolders may not appear until the cache expires. |
| **Summary / list data sources** | Day-folder file lists use **`summary.json`** (see `handleMainRecordingsList` / `handleMainRecordingsSummary`); the SPA should prefer **`/api/recordings/summary`** (NDJSON) over heavier patterns. |
| **WAV streaming** | At most **2** concurrent `GET /api/recordings/stream` handlers (`kMaxConcurrentRecordingsStreams`); extra clients get **503** + `API_ERR_BUSY`. Read chunk size **512** bytes (`kRecordingsStreamReadChunkBytes`); yield every **4** chunks (`kRecordingsStreamChunksPerYield`) so playback shares the SD with recorder/uploader. |

**Code:** [`src/boondock_server.cpp`](../src/boondock_server.cpp).

---

## 4. Logging

### Implemented

| Item | Behavior |
|------|------------|
| **PSRAM log buffer** | In SD mode, log text is batched in a **32 KiB** PSRAM buffer when allocation succeeds (`heap_caps_malloc`, `MALLOC_CAP_SPIRAM`); falls back to a **2 KiB** DRAM buffer if PSRAM is unavailable. |
| **Append vs FAT flush** | Buffered lines are appended to the log file on a **10 s** timer (`kLogRamToFileIntervalMs`); `File::flush()` (FAT/metadata sync) runs at most every **15 s** when dirty (`kLogFileFatFlushIntervalMs`). **Tradeoff:** more log loss on abrupt power loss between syncs. |
| **Deployment** | — | **Verbose logging** still increases SD traffic; tune **log level** in the field when needed. |

**Code:** [`src/logger.cpp`](../src/logger.cpp).

---

## 5. Inbox index and summaries

| Issue | Effect | Recommendation |
|--------|--------|----------------|
| **Per-upload append to `index.json`** | Many small writes | **Batch** lines in RAM and flush periodically, or append in larger chunks. |
| **Summary regeneration** | Reads/writes across months | Ensure **monthly/yearly** summary updates run on a **schedule**, not on hot paths (e.g. avoid redundant `storage_updateYearlySummary` if called too often). |

**Primary code:** [`src/upload_queue.cpp`](../src/upload_queue.cpp) (`uploadQueue_appendToIndex`), [`src/common.cpp`](../src/common.cpp) (summary helpers).

---

## 6. Maintenance and cleanup

| Issue | Effect | Recommendation |
|--------|--------|----------------|
| **Full-card walks** | Long read passes | **Scope** cleanup (e.g. date range, depth limit), align with any **reconciliation** policy (today/yesterday only, etc.). |

---

## 7. What to optimize last

- **Single** `SD_MMC.open` + sequential read for **upload** or **one** web stream is inherent; focus on **duplicate** opens and **enumeration** (directory scans), not on removing necessary data reads.

---

## 8. Related files (quick reference)

| Area | Files |
|------|--------|
| Recorder writes / flush | `src/recorder.cpp` |
| Pending / inbox / index | `src/upload_queue.cpp` |
| Upload task opens | `src/main.cpp`, `src/network.cpp` |
| Logs | `src/logger.cpp` |
| Summaries / prune / inbox | `src/common.cpp` |
| Web recordings API | `src/boondock_server.cpp` |

---

## 9. Summary

Reduce I/O by **coalescing writes** (recorder flush batching and log batching), **avoiding redundant opens** (summary size from finalize math), **minimizing tree scans** (RAM queue first, **throttled** empty fallback, **TTL-cached** `uploadQueue_getPendingCount`), **caching** repeated pending-dir work during a session, and **scoping** maintenance. Every batching decision trades **durability or freshness** for fewer SD operations—tune for field power stability and product requirements.
