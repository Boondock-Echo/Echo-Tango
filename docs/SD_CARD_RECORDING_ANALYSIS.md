# SD Card Recording Reliability Analysis

This document summarizes the analysis of SD card–based recording and identified issues that can cause reliability problems compared to PSRAM recording. The device’s goal is to **keep recording** and **upload audio to the server via API**.

---

## Architecture Summary

- **PSRAM recording**: Audio is written to a pre-allocated buffer in PSRAM; when the recording ends, the buffer is queued and later uploaded from memory. No filesystem I/O during the recording.
- **SD card recording**: Audio is streamed to a file in `/pending/` (`.tmp` while recording, renamed to `.wav` on finalize). The upload task reads files from `/pending` (and an in-memory queue) and uploads them.

---

## Identified Issues

### 1. **Runtime SD failure never switches to PSRAM**

- **Location**: `storage_switchToPsramOnFailure()` in `common.cpp` is **never called** from the recorder (or anywhere else).
- **Effect**: When an SD write fails, only `storage_recordWriteError()` runs. The device keeps trying to record to SD instead of falling back to PSRAM, so recording can keep failing until the next reboot or remount.
- **Intent**: The comment on `storage_recordWriteError()` says it “triggers auto-switch to PSRAM if SD fails,” but that switch is never triggered.

**Recommendation**: Call `storage_switchToPsramOnFailure()` from the recorder when a write fails after all retries (and optionally after the first unrecoverable failure). Ensure that when “runtime SD failure” is set, the system actually uses PSRAM for recording (see issue 2).

---

### 2. **`isStorageModeSdCard()` ignores runtime failure**

- **Location**: `common.cpp`: `isStorageModeSdCard()` returns `g_storageMode == StorageMode::SD_CARD`.
- **Effect**: Even if `storage_switchToPsramOnFailure()` were called and set `g_sdCardRuntimeFailure = true`, `g_storageMode` stays `SD_CARD`, so `isStorageModeSdCard()` remains true and the recorder keeps using SD.
- **Effect**: The runtime-failure path does not actually cause a switch to PSRAM for recording.

**Recommendation**: Treat runtime failure as “SD not usable for recording.” For example, make `isStorageModeSdCard()` return false when `g_sdCardRuntimeFailure` is true (so recording uses PSRAM until the next successful remount).

---

### 3. **Failed write leaves file handle open; reopen path never used**

- **Location**: `recorder.cpp` – `appendAudioSamples()` SD card branch.
- **Behavior**: On write failure or partial write, the code retries up to 3 times. Reopen is only done when `!currentRecordingFile`. After a failed write, the file is **never closed** and `currentRecordingFile` is never cleared, so the “reopen for append” branch is never taken on retry.
- **Effect**: Once a write fails, subsequent chunks keep writing to the same (possibly bad) handle and keep failing until the recording is finalized. No recovery within the same recording.

**Recommendation**: On first write failure (or after final retry), close the file and set `currentRecordingFile = File()`. The next chunk will then see `!currentRecordingFile` and reopen (with `FILE_APPEND`), giving the SD driver a fresh handle and a chance to recover.

---

### 4. **Retry loop can corrupt data on partial write**

- **Location**: `recorder.cpp` – same retry loop.
- **Behavior**: Each attempt does `written = currentRecordingFile.write(samples, byteCount)`. If the first attempt writes only part of the chunk (e.g. 500 of 1024 bytes), the file position advances by 500. The next attempt writes the **same** `samples` and `byteCount` again, so it appends another 1024 bytes from the same buffer instead of the remaining 524.
- **Effect**: Duplicate/corrupt audio and incorrect file length. In addition, `recordedBytes` is only updated with the **last** attempt’s `written`, so the header can undercount actual bytes written (or count the wrong amount if the last attempt wrote a different amount).

**Recommendation**: Do not retry the same chunk with the same buffer. If `write()` returns less than `byteCount`, treat it as failure for this chunk: add the returned `written` to `recordedBytes`, close the file, clear the handle, and return. The next chunk will reopen and continue. Optionally retry once by closing, clearing, and reopening before the next chunk.

---

### 5. **`recordedBytes` only uses last attempt’s `written`**

- **Location**: `recorder.cpp` – after the retry loop, `recordedBytes += written`.
- **Effect**: `written` is overwritten each attempt, so only the last attempt’s count is added. If attempt 0 wrote 1024 and attempt 1–2 wrote 0, we add 0 and undercount. The WAV header in `finalizeRecording()` is then wrong (data length mismatch).

**Recommendation**: Once retries are fixed (no duplicate writes), add only the actual bytes written for this chunk (e.g. the first attempt’s `written`, or the only attempt if we stop retrying the same chunk).

---

### 6. **Flush on every chunk can hurt SD reliability and latency**

- **Location**: `recorder.cpp` – after each successful write, `currentRecordingFile.flush()` is called.
- **Effect**: Frequent flush increases SD wear and can block the recording task. Under load (e.g. upload + logging + recording), this can make SD writes slower and more prone to timeouts or errors.

**Recommendation**: Flush periodically (e.g. every N bytes or every M ms) or only on finalize, unless you need stronger durability per chunk. Balance between “keep recording” and “minimize SD stress.”

---

### 7. **Memory queue can hold `.tmp` path if rename fails**

- **Location**: `recorder.cpp` – `finalizeRecording()`; after rename, `finishedRecordingPath` is added to `sdCardMemoryQueue_addRecording()`.
- **Behavior**: If `SD_MMC.rename(.tmp → .wav)` fails, `finishedRecordingPath` stays as `.tmp` and is still added to the in-memory queue. The upload task then tries to open that path.
- **Effect**: Upload can open and send a `.tmp` file. Content is valid WAV, but the filename is inconsistent with the filesystem scan (which only lists `.wav`). Minor; mainly a consistency issue.

**Recommendation**: Either add only the final `.wav` path to the memory queue (if rename succeeded), or ensure the upload logic explicitly allows `.tmp` when it’s the path from the memory queue.

---

### 8. **Concurrent SD access**

- **Behavior**: The recorder holds one file open for writing (current recording) and does not hold `uploadQueue_lockPendingDir()` during the whole recording. The upload task scans `/pending` and opens other files for read. So recorder and upload access the SD concurrently.
- **Effect**: Some SD cards or drivers can be sensitive to concurrent access, leading to transient errors or timeouts. This can contribute to write failures even when the card is healthy.

**Recommendation**: Already mitigated by using different files (recorder writes `.tmp`, upload reads `.wav`). If problems persist, consider batching SD writes or reducing flush frequency to shorten write windows.

---

## Summary Table

| Issue | Severity | Effect on “keep recording” |
|-------|----------|------------------------------|
| 1. Switch to PSRAM never triggered | High | SD failures never cause fallback to PSRAM; recording can keep failing. |
| 2. Runtime failure not reflected in mode | High | Even if switch were triggered, recorder would still use SD. |
| 3. File handle never closed on failure | High | No recovery within a recording; repeated failures on same handle. |
| 4. Retry corrupts on partial write | High | Corrupt/duplicate audio and wrong length. |
| 5. `recordedBytes` from last attempt only | Medium | Wrong WAV header / duration. |
| 6. Flush every chunk | Medium | Extra SD load and blocking. |
| 7. `.tmp` in memory queue | Low | Naming inconsistency. |
| 8. Concurrent SD access | Low–Medium | Possible extra transient errors. |

---

## Recommended Fix Order

1. **Fix retry and handle (4, 3, 5)**  
   On partial or failed write: do not retry the same chunk; add actual `written` to `recordedBytes`; close file and clear `currentRecordingFile` so the next chunk reopens.

2. **Wire runtime failure to PSRAM (1, 2)**  
   On unrecoverable SD write failure in the recorder, call `storage_switchToPsramOnFailure()`. Make `isStorageModeSdCard()` return false when `g_sdCardRuntimeFailure` is true so recording uses PSRAM until SD is remounted successfully.

3. **Optional: reduce flush frequency (6)**  
   Flush every N chunks or every M ms instead of every chunk, if acceptable for durability.

4. **Optional: memory queue path (7)**  
   Add only the final `.wav` path to the memory queue when rename succeeds; or document that upload may receive `.tmp` from the queue.

---

## Files Touched

- `src/recorder.cpp` – `appendAudioSamples()` SD branch (retry, close, reopen, `recordedBytes`, optional flush).
- `src/common.cpp` – `isStorageModeSdCard()` to consider `g_sdCardRuntimeFailure`; ensure `storage_switchToPsramOnFailure()` is called from recorder on persistent write failure.
