# Stability and improvement recommendations

Apply these one by one. Each item has a **Priority**, **File(s)**, and **Action**.

---

## 1. Fix PSRAM buffer size in debug log (recorder) ✅

- **Status:** Fixed
- **Priority:** High (correctness)
- **File:** `src/recorder.cpp`
- **Location:** In `appendAudioSamples`, when allocating PSRAM buffer after mode switch (around line 254).
- **Issue:** The log uses `psramRecordingCapacity` before it is set; it should show the newly allocated size.
- **Action:** In the `logDebugf` call, use `maxBytes` instead of `psramRecordingCapacity` (or assign `psramRecordingCapacity = maxBytes` first, then log `psramRecordingCapacity`).
- **Fix applied:** Assign `psramRecordingCapacity = maxBytes` before the log, then log `psramRecordingCapacity`.

---

## 2. Implement or remove `logResetReason()` ✅

- **Status:** Fixed
- **Priority:** High (dead code / missing behavior)
- **File:** `src/main.cpp`, `src/main.h`, `src/network.cpp`
- **Location:** Function `logResetReason()` in the anonymous namespace (around lines 79–82).
- **Issue:** The function retrieves the reset reason but never logs it.
- **Action:** Either (a) log the reset reason using `resetReasonToString(reason)` and call `logResetReason()` from startup (e.g. after logger is ready), or (b) remove the function and any calls to it.
- **Fix applied:** (a) Implemented: `logResetReason()` now calls `logInfof("[Startup] Reset reason: %s", resetReasonToString(reason))` and is called from `setup()` after `logger_begin()`. Moved `resetReasonToString` to file scope and added `system_getResetReasonString()` in main (declared in main.h) for reuse. (b) When device comes online, the **online** event sent to the server now includes `resetReason` metadata (built with `StaticJsonDocument` in `network_loop()` on gotIp).

---

## 3. Remove duplicate warning for memory-queue upload failure ✅

- **Status:** Fixed
- **Priority:** Medium (log noise)
- **File:** `src/main.cpp`
- **Location:** SD card in-memory queue upload failure handling — one block around lines 371–378, another around 406–411.
- **Issue:** The same warning is logged twice when upload fails (once before the `if (strcmp(...))` and once inside it).
- **Action:** Keep a single log inside the `if (strcmp(request.path, lastWarnedFilePath) != 0)` block; remove the duplicate `logWarnf` that runs every time.
- **Fix applied:** Removed the duplicate `logWarnf` that ran on every failure in both blocks; kept the single `logWarnf` inside the `if (strcmp(request.path, lastWarnedFilePath) != 0)` block so the warning is logged only once per path.

---

## 4. Add mutex for PSRAM and SD card memory queues ✅

- **Status:** Fixed
- **Priority:** High (concurrency / stability)
- **File:** `src/upload_queue.cpp` (no changes to `upload_queue.h`, `main.cpp`, or `recorder.cpp`)
- **Issue:** Record task and upload task access shared queue/pool data without synchronization.
- **Action:**
  - In `upload_queue.cpp`: create a mutex (e.g. `xSemaphoreCreateMutex()`), initialize it in `psramQueue_begin()` and `sdCardMemoryQueue_begin()` (or once at startup).
  - Take the mutex in: `psramQueue_addRecording`, `psramQueue_getNextEntry`, `psramQueue_releaseEntry`, `psramQueue_dropOldestEntry`, `psramPool_take`, `psramPool_return`, and the SD card memory queue equivalents (`sdCardMemoryQueue_addRecording`, `sdCardMemoryQueue_getNextEntry`, `sdCardMemoryQueue_releaseEntry`). Use a short timeout (e.g. 100–500 ms) and handle timeout (e.g. skip or retry) to avoid deadlock.
  - Optionally declare the mutex in `upload_queue.h` or keep it file-static and expose only the locked API.
- **Fix applied:** Two mutexes (`g_psramMutex`, `g_sdCardMemoryQueueMutex`) created in `psramQueue_begin()` and `sdCardMemoryQueue_begin()`. All PSRAM and SD memory queue/pool APIs take the appropriate mutex with 300 ms timeout. Added `psramPool_return_unlocked()` for use inside `releaseEntry`/`dropOldestEntry` to avoid double-lock. On timeout, get/add/count functions return empty/failure; `releaseEntry` retries up to 3 times with 10 ms delay, then logs and returns without modifying state if still no lock. Recorder and upload task call sites unchanged.

---

## 5. Fix `sendHealthMessage` default argument declaration

- **Priority:** Medium (correctness / ODR)
- **File:** `src/main.cpp` (and possibly `src/health.cpp`)
- **Location:** Local `extern void sendHealthMessage(bool mutexAlreadyHeld = false);` inside `serialTask` and inside `maintenanceTask` (in `health.cpp`).
- **Issue:** Default arguments should appear only in one declaration (the canonical one), not in local externs.
- **Action:** Remove the default from the local `extern` declarations. Ensure the default `false` is specified only in the canonical declaration (e.g. in `main.h` or at the function definition in `main.cpp`).

---

## 6. Revisit upload when time is not valid ✅

- **Status:** Fixed
- **Priority:** Medium (behavior / data loss risk)
- **File:** `src/network.cpp`
- **Location:** `uploadAudioFile`, `uploadLogFile`, and `network_pushSettingsToServer` — early return when `!timeKeeper().timeIsValid()`.
- **Issue:** If NTP never syncs, uploads never run; PSRAM or SD can fill and recordings can be dropped.
- **Action:** Consider one or more of: (a) allow upload with best-effort time (e.g. RTC or boot + millis) and document “time not synced” in metadata; (b) add a periodic warning or event when time is invalid and uploads are skipped; (c) cap queue size or age when time is invalid so the device degrades gracefully instead of filling up silently.
- **Fix applied:** Time is no longer a gate for uploads. Removed the `timeIsValid()` check from `uploadAudioFile`, `uploadLogFile`, and `network_pushSettingsToServer`. Clock is synced from event responses (e.g. heartbeat, online): when a successful event HTTP response has a body, `syncClockFromApiResponse(body)` is called so server timestamp/current_time sets the device clock. When epoch is invalid, `formatIsoTimestamp` already returns fallback `ms-<recordedAtMs>` so uploads proceed with a best-effort timestamp.

---

## 7. Reduce cost of full heap integrity check

- **Priority:** Medium (performance / jitter)
- **File:** `src/main.cpp`
- **Location:** `checkMemorySafety()` — `heap_caps_check_integrity_all(true)` (around line 147).
- **Issue:** Full heap check every 5 seconds can cause noticeable freezes.
- **Action:** Call `heap_caps_check_integrity_all(true)` less often (e.g. only when free heap is below a threshold, or every N minutes), or use `false` for the frequent check and reserve `true` for critical paths or right before a safety reboot.

---

## 8. Serialize SD card pending directory access ✅

- **Status:** Fixed
- **Priority:** Medium (stability on SD)
- **File:** `src/upload_queue.cpp`, `src/upload_queue.h`, `src/recorder.cpp`
- **Issue:** `uploadQueue_getNextFile()` and `uploadQueue_getPendingCount()` iterate `/pending` while the record task may be creating/renaming files; SD_MMC is not necessarily thread-safe.
- **Action:** Introduce a mutex (or reuse an existing one) for “pending directory” operations. Have both the upload task (when scanning/reading from `/pending`) and the record task (when creating/renaming files in `/pending`) take the same mutex around those operations.
- **Fix applied:** Added `g_pendingDirMutex` in `upload_queue.cpp`, created in `uploadQueue_begin()`. Added `uploadQueue_lockPendingDir(timeoutMs)` and `uploadQueue_unlockPendingDir()`. Wrapped `uploadQueue_getNextFile()`, `uploadQueue_getPendingCount()`, and `moveOneTempFileToTrash()` with take/give. In recorder, lock before mkdir+open in SD branch, unlock after open; lock before close+rename+remove in finalize, unlock after. Recorder uses 500 ms timeout.

---

## 9. Logger behavior when in PSRAM-only mode

- **Priority:** Medium (correctness)
- **File:** `src/logger.cpp`
- **Issue:** When `!isStorageModeSdCard()`, file logging is skipped; ensure no code path assumes an open `g_logFile` or tries to use it when in PSRAM-only mode.
- **Action:** Audit all uses of `g_logFile` and related state; ensure they are no-ops or safe when `!isStorageModeSdCard()` (e.g. `ensureLogFileLocked()` returns false). Add guards if any path could touch a stale or invalid file handle.

---

## 10. Protect settings with a mutex (or atomic updates)

- **Priority:** Medium (correctness under concurrency)
- **File:** `src/settings.cpp`, `src/settings.h`, and all readers of `appSettings`.
- **Issue:** Multiple tasks read `appSettings` while settings can be updated (e.g. `settings_applyJsonFromServer`, `settings_updateAllFromJson`); risk of torn reads.
- **Action:** Add a settings mutex. Take it for any write (apply/update) and for any read that must be consistent (or use a single “current settings” pointer and swap after a full copy). Ensure NVS and JSON apply paths hold the mutex for the duration of the update.

---

## 11. Clarify or fix upload queue size reporting ✅

- **Status:** Fixed
- **Priority:** Low (UX / consistency)
- **File:** `src/main.cpp` — `system_getUploadQueueSize()`; health message uses it for `pq`.
- **Issue:** `system_getUploadQueueSize()` returns only files in `/pending`, not items in the SD card in-memory queue, so the value can be 0 while the memory queue has items.
- **Action:** Either document that “upload queue size” means “pending files on SD” only, or extend the implementation to include the in-memory queue count (e.g. `uploadQueue_getPendingCount()` + `sdCardMemoryQueue_getPendingCount()` when in SD mode) and document the combined meaning.
- **Fix applied:** Extended `system_getUploadQueueSize()`: in PSRAM mode returns `psramQueue_getPendingCount()`; in SD mode returns `uploadQueue_getPendingCount()` + `sdCardMemoryQueue_getPendingCount()`. Health message `pq` now uses `system_getUploadQueueSize()` for consistency.

---

## 12. Ensure `request.path` is null-terminated

- **Priority:** Low (safety)
- **File:** `src/main.cpp` (upload task when building `UploadRequest`), and any other place that fills `request.path`.
- **Issue:** If `request.path` is ever not null-terminated, `strcmp`/`strncpy` with `lastWarnedFilePath` could be unsafe.
- **Action:** After every place that fills `request.path`, set `request.path[kMaxUploadPathLength - 1] = '\0'` (or use a helper that always null-terminates).

---

## 13. Watchdog feeding in new or long-running code

- **Priority:** Low (maintainability)
- **File:** Any new or modified code that runs in a task and can block for more than a few seconds.
- **Action:** When adding or changing long-running or blocking operations (e.g. new network calls, DNS, TLS), ensure the task feeds the watchdog at least every few seconds (e.g. in loops and before/after blocking calls) so the task watchdog does not trigger.

---

## 14. Verify log upload API path case

- **Priority:** Low (correctness)
- **File:** `src/config.h`
- **Location:** `DEFAULT_LOG_UPLOAD_PATH` — currently `"/api/V1/upload/logs"` (capital V).
- **Action:** Confirm with the server that the path is correct (e.g. `V1` vs `v1`). If the server expects `/api/v1/...`, change the constant to match.

---

## 15. Fix `sendHealthMessage` declaration in maintenance task

- **Priority:** Low (same as item 5, different location)
- **File:** `src/health.cpp`
- **Location:** Local `extern void sendHealthMessage(bool mutexAlreadyHeld = false);` inside `maintenanceTask`.
- **Action:** Remove the default argument from this local extern; keep the default only in the canonical declaration.

---

## Summary checklist

- [x] 1. Recorder PSRAM log variable
- [x] 2. logResetReason
- [x] 3. Duplicate upload-failure warning
- [x] 4. Queue/pool mutex
- [ ] 5. sendHealthMessage default (main.cpp)
- [ ] 6. Upload when time invalid
- [ ] 7. Heap check frequency/cost
- [x] 8. SD pending directory mutex
- [ ] 9. Logger PSRAM-only mode
- [ ] 10. Settings mutex
- [x] 11. Upload queue size reporting
- [ ] 12. request.path null termination
- [ ] 13. Watchdog in new code (guideline)
- [ ] 14. Log upload API path
- [ ] 15. sendHealthMessage default (health.cpp)