# Release notes — `KC-Fixing-SD-Card-Issues` vs `KC-Add-Web-Audio-Playback`

**Baseline:** `KC-Add-Web-Audio-Playback`  
**Release branch:** `KC-Fixing-SD-Card-Issues`  
**Delta:** 2 commits (2026-03-24), +1,309 / −426 lines across tracked files (local `.pio/` build artifacts excluded from product meaning; regenerate clean builds from `platformio.ini`).

---

## Highlights

This release focuses on **SD/pending upload reliability and I/O reduction**, **clearer upload diagnostics**, **recordings UI/API behavior**, and **build-time product branding** (Tango / Echo / Edge), plus a new **EDGE** PlatformIO environment.

---

## Build & firmware identity

- **`platformio.ini`:** Adds **`[env:EDGE]`** (aligned with TANGO/ECHO: `-DFIRMWARE_PREFIX=\"EDGE\"`, `-DEDGE`).
- **`config.h`:** Firmware string **`FIRMWARE`** is **`FIRMWARE_PREFIX "-2026-03-24"`**. Introduces **`PRODUCT_BROWSER_TITLE`** per build (`Boondock Tango` / `Boondock Echo` / `Boondock Edge`).

---

## Web UI & API — product branding

- **SPA HTML** uses the build-time product name in the shell (title / header).
- **`applyProductBranding()`** applies the name from API payloads (`data.product`).
- **Device/info JSON** includes a **`product`** field (`PRODUCT_BROWSER_TITLE`) so the browser does not hardcode the product name.

---

## SD card, `/pending`, and upload queue

- **Pending-queue metadata:** The in-memory SD queue stores **basenames** and rebuilds **`/pending/YYYY/MM/DD/...`** paths via **`sdCardMemoryQueue_buildFullPath`**, with the slot table allocated in **SPIRAM** (DRAM fallback).
- **Queue sizing / behavior:** Up to **50** basename slots; **drop-oldest** when full; clearer logging on mutex timeouts.
- **Less SD churn:** **Cached** recursive count of `.wav` files under `/pending` (short TTL, ~2s) plus **`uploadQueue_invalidateFilesystemPendingCountCache()`** when the tree changes.
- **Concurrency:** **`uploadQueue_lockPendingDir` / `uploadQueue_unlockPendingDir`** serialize directory work on the pending tree (recorder vs upload task). The lock is not held across full file read/upload.
- **Upload task:** When the memory queue is empty and a filesystem scan finds nothing, **full-tree scans are rate-limited** (defer interval) to avoid excessive SD access.
- **Recorder path:** Documented behaviors include **coalesced `flush`** on SD, **cached pending-root / day-folder** work, and **size** derived without an extra `open()` where applicable. See [SD_CARD_IO_OPTIMIZATION.md](./SD_CARD_IO_OPTIMIZATION.md).

---

## Upload diagnostics

- **`network_getLastUploadFailureReason()`** returns a short reason for the last failed **`uploadAudioFile`** (e.g. WiFi down, mutex timeout, SD unavailable, open failures, HTTP/endpoint errors, or a generic “all endpoints failed” when nothing more specific was set).

---

## Recordings browser & server

- **TTL cache** for **`GET /api/recordings/folders`** to reduce repeated SD directory enumeration on UI refresh.
- **Yielding** during long SD walks so other work can run.
- UI copy clarifies **UTC day-folder** boundaries for `/recordings/...` layout.
- **Clock / refresh** for the recordings view uses a **dedicated interval** so listing time stays aligned with device time.

---

## Logging

- **`logger.cpp`:** Refactored for **lighter, more reliable** file logging under load.

---

## Supporting code changes

- **`main.cpp`:** Coordination updates (upload task, intervals, SD/PSRAM paths) aligned with queue and recorder changes.
- **`settings.cpp`:** Adjustments to session/stats JSON (recordings-related fields) to match new behavior.

---

## Documentation

- [SD_CARD_IO_OPTIMIZATION.md](./SD_CARD_IO_OPTIMIZATION.md) — I/O reduction and implemented behavior.
- [SD_CARD_RECORDING_RECOMMENDATIONS.md](./SD_CARD_RECORDING_RECOMMENDATIONS.md) — recording/queue alignment with firmware.

---

## Git reference

| Commit    | Date       | Summary |
|-----------|------------|---------|
| `cde64d3` | 2026-03-24 | Recordings functionality & UI; SD/upload queue, logger, network failure reasons, docs |
| `5eecb81` | 2026-03-24 | EDGE env, branding in UI/API, `platformio.ini` / `config.h` / SPA follow-ups |

---

## Notes for packagers / CI

- Do not treat **`.pio/`** checksum or `libdeps` churn as functional product changes; reproduce release binaries from **`platformio.ini`** on a clean build environment.
