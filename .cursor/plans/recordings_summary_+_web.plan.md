# Recordings folder + web UI from summary.json (updated)

## Design decisions

- **Path layout:** `/recordings/YYYY/MM/DD/summary.json` — one JSON object per line (JSONL), same pattern as [`upload_queue_appendToIndex`](d:/Github/Boondock-TANGO-ECHO-2026/src/upload_queue.cpp) for `index.json`.
- **Inbox playback path:** For standard pending filenames [`/pending/YYYY-MM-DD-HH-MM-SS.wav`](d:/Github/Boondock-TANGO-ECHO-2026/src/recorder.cpp), the future inbox path matches [`moveFileToInbox`](d:/Github/Boondock-TANGO-ECHO-2026/src/upload_queue.cpp) logic: `/inbox/YYYY/MM/DD/<same filename>`. Store this as `inboxPath` (or `playPath`) in each summary line so the SPA can call existing [`/api/recordings/stream?path=...`](d:/Github/Boondock-TANGO-ECHO-2026/src/boondock_server.cpp) unchanged.
- **When to append (no duplicates):**
  - **SD recording:** After finalize in [`finalizeRecording`](d:/Github/Boondock-TANGO-ECHO-2026/src/recorder.cpp) — only when the file remains on SD (not PSRAM), not discarded as “small file”, and final path is a `.wav` under `/pending`. Include `pendingPath`, computed `inboxPath`, `durationMs`, `endReason`, `peakDb`, `sizeBytes` (stat file after close), `sampleRate`, device id, etc.
  - **PSRAM recording:** Append **when the file reaches inbox** — in [`uploadQueue_markUploadedWithRecord`](d:/Github/Boondock-TANGO-ECHO-2026/src/upload_queue.cpp) after a successful `moveFileToInbox`, **only for PSRAM uploads**. Pass `request.isPsramMode` from [`uploadAudioFile`](d:/Github/Boondock-TANGO-ECHO-2026/src/network.cpp) so SD uploads do not append twice (SD is already covered at finalize).
- **Fallback filenames** (`/pending/rec_<millis>.tmp` when time invalid): Document one behavior (e.g. store `pendingPath` + optional empty `inboxPath` until known).

## Recordings summary cleanup when inbox days are removed

When an **inbox day folder** `/inbox/YYYY/MM/DD` is permanently deleted, the matching **`/recordings/YYYY/MM/DD/summary.json`** (and optionally empty parent dirs under `/recordings`) must be removed or rewritten so the web UI does not list recordings for days that no longer exist on disk.

**Where inbox deletion happens today:** [`storage_deleteOldestFolderIfNeeded`](d:/Github/Boondock-TANGO-ECHO-2026/src/common.cpp) (recursive delete of oldest `/inbox/YYYY/MM/DD` when utilization rules apply). After a successful delete it already calls [`storage_updateMonthlySummary`](d:/Github/Boondock-TANGO-ECHO-2026/src/common.cpp) / yearly for inbox aggregates.

**Planned behavior (two layers):**

1. **Immediate:** In `storage_deleteOldestFolderIfNeeded`, after parsing `delYear`, `delMonth`, `delDay` from `oldestFolderPath`, also delete `/recordings/<Y>/<M>/<D>/summary.json` if it exists (and remove empty `/recordings/Y/M/D` directories if desired). Keeps recordings metadata in sync for the primary automatic deletion path.

2. **Maintenance task hook:** Add a function such as `storage_pruneRecordingsSummariesWithoutInbox()` (name TBD) in [`common.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/common.cpp) that:
   - Walks `/recordings/YYYY/MM/DD` (or only checks paths where `summary.json` exists),
   - For each day, if **`/inbox/YYYY/MM/DD` does not exist**, delete that day’s `summary.json` (and prune empty directories upward as needed).
   - Call it from [`maintenanceTask`](d:/Github/Boondock-TANGO-ECHO-2026/src/health.cpp) on the same schedule as other periodic storage work (e.g. alongside the hourly block that runs [`storage_cleanupOldUploadedFiles`](d:/Github/Boondock-TANGO-ECHO-2026/src/health.cpp) / [`storage_checkCapacityAlerts`](d:/Github/Boondock-TANGO-ECHO-2026/src/health.cpp)), with watchdog feeds around SD walks.

This covers **manual** SD edits, **future** call sites that delete inbox days, and any edge case where immediate deletion was missed.

## Implementation outline

### 1. Storage helpers ([`common.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/common.cpp) / [`common.h`](d:/Github/Boondock-TANGO-ECHO-2026/src/common.h))

- Add `storage_ensureDirectoryPath` (shared mkdir chain; reuse logic from static `createDirectoryPath` in [`upload_queue.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/upload_queue.cpp)).
- Add `recordings_appendSummaryLine(...)` — append one JSONL line to `/recordings/YYYY/MM/DD/summary.json`.
- Add `pendingWavToPredictedInboxPath(...)` mirroring `moveFileToInbox` filename logic.
- Add **`storage_pruneRecordingsSummariesWithoutInbox()`** (or equivalent) for maintenance reconciliation.
- In **`storage_deleteOldestFolderIfNeeded`**, after successful delete of `/inbox/Y/M/D`, remove matching recordings `summary.json` for that day.

### 2. Recorder hook ([`recorder.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/recorder.cpp))

- SD `finalizeRecording`: append summary line when appropriate (see above).

### 3. Upload hook for PSRAM ([`upload_queue.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/upload_queue.h) / [`network.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/network.cpp))

- Extend `markUploadedWithRecord` + pass `isPsramMode` to append only for PSRAM after move.

### 4. Web API ([`boondock_server.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/boondock_server.cpp))

- `recordings_canonicalizePath`, default `/recordings`, list from `summary.json`, stream unchanged (inbox path).

### 5. SPA ([`app_js_spa.h`](d:/Github/Boondock-TANGO-ECHO-2026/src/app_js_spa.h))

- Tree/list base `/recordings`; playback uses inbox path from items.

### 6. Maintenance ([`health.cpp`](d:/Github/Boondock-TANGO-ECHO-2026/src/health.cpp))

- Invoke `storage_pruneRecordingsSummariesWithoutInbox()` periodically (e.g. hourly with other storage cleanup), with `esp_task_wdt_reset()` during long walks.

## Todos

- storage-api: mkdir helper + append + pending→inbox + **pruneRecordingsSummariesWithoutInbox** + **deleteOldest hook for recordings file**
- recorder-hook: finalize append (SD)
- upload-psram: markUploadedWithRecord PSRAM-only append
- web-api: recordings paths + list from summary.json
- spa: `/recordings` UI + stream from inbox path
- **maintenance: call prune in maintenanceTask (hourly or aligned with storage cleanup)**
