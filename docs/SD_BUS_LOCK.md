# SD bus lock

ESP32 `SD_MMC` is **not safe across FreeRTOS tasks**. RecordTask writes WAV data while NetworkTask reads pending files for upload. Logger, maintenance, MQTT downloads, the serial CLI, and **WebServer** (Recordings UI) also touch the card. Overlapping host commands stall the MMC bus (often logged as error 257). A `read()`/`write()` that never returns means **NetworkTask or RecordTask miss the 30 s task watchdog and the device reboots**.

All firmware SD I/O in `src/` goes through **`sd_bus`** so only one task talks to the card at a time. `sd_bus.cpp` is the only translation unit that should call `SD_MMC.*`.

## API

| Piece | Role |
|--------|------|
| `sd_bus::init()` | Create the recursive mutex. Called from `setup()` before `logger_begin()`. Also lazy-inits on first lock. |
| `sd_bus::Guard` | RAII take/give. Used inside the wrappers; use directly only for a tight block of raw `SD_MMC` calls (there should be none left in `src/`). |
| `sd_bus::exists/mkdir/rename/remove/rmdir/open` | Locked filesystem ops. |
| `sd_bus::cardType/totalBytes/usedBytes` | Locked card queries. |
| `sd_bus::mount/unmount` | Locked `SD_MMC.begin` / `SD_MMC.end`. |
| `sd_bus::SdFile` | `Stream` wrapper around `File`. Every `read`/`write`/`seek`/`close`/`flush`/`size`/`openNextFile` takes the bus lock for that call only. |

Timeouts (in `config.h`):

- `SD_BUS_LOCK_TIMEOUT_MS` (8 s) — normal I/O
- `SD_BUS_LOCK_WAIT_SLICE_MS` (50 ms) — wait slice; `esp_task_wdt_reset()` each slice
- `SD_BUS_MOUNT_LOCK_TIMEOUT_MS` (20 s) — mount/unmount

If the lock is not acquired, wrappers fail safely (`false`, `0`, invalid `SdFile`) instead of calling into `SD_MMC`.

## Lock order (required)

1. Domain mutexes first: logger mutex, `/pending` mutex, upload mutex, queue mutexes.
2. **SD bus last.**
3. Never take a domain mutex while holding the SD bus.
4. Never hold the SD bus across Wi-Fi, `delay()`, or logging. Log **after** the wrapper returns.

`/pending` still serializes directory create/rename/scan. That lock does **not** cover file content. Upload reads and recorder writes each take the **SD bus** for one syscall, then release, so recording can proceed between upload chunks.

Same-task nesting is OK (recursive mutex). Example: `markUploaded` holds `/pending`, then `sd_bus::rename` takes the SD bus.

## How to add new SD code

```cpp
#include "sd_bus.h"

sd_bus::SdFile f = sd_bus::open("/pending/foo.wav", FILE_READ);
if (!f) { /* fail */ }
uint8_t buf[512];
size_t n = f.read(buf, sizeof(buf));  // lock held only during read
f.close();
// send buf over Wi-Fi here — do not hold the bus
```

Do **not** call `SD_MMC.*` or use a raw `File` for card I/O from `src/`. `sd_bus.cpp` is the only translation unit that should call the driver.

Copy `name()` into a `String` immediately; do not keep the `const char*` after another SD call.

## Tasks that use the bus

| Task | Typical SD work |
|------|------------------|
| RecordTask | Open/write/flush/close WAV, rename `.tmp` → `.wav` |
| NetworkTask | Open/read pending WAV for upload, move to `/inbox` |
| Serial / logger | Append log files |
| Maintenance | Summaries, cleanup, capacity, remount |
| MQTT (ECHO) | Firmware/asset file writes |
| CLI | `SDLS`, format, delete, remount |
| WebServer | SPA files cached on SD, recordings folder/list/summary, **browser playback** (`GET /api/recordings/stream`), home/SD status capacity queries |

## Browser playback (web UI)

This is **not** onboard speaker output and **not** Live Audio (mic PCM over WebSocket from RAM).

**Browser playback** is: a laptop/phone opens the device SPA → Recordings → clicks a `.wav` → the `<audio>` element GETs `/api/recordings/stream?path=...`. WebServer reads `/inbox/...` (or `/pending/...` if not yet uploaded) and streams bytes over Wi‑Fi. The file is always on the **SD card**.

`recordToSdCard = false` (PSRAM **recording** mode) only changes where the **live take** is captured. If the card is still mounted (`useSdCard` / `isStorageModeSdCard()`), the Recordings page still streams from SD. Playback must use `sd_bus` the same as the recorder.

Handlers (`src/boondock_server.cpp`):

| HTTP | SD work |
|------|---------|
| SPA HTML/CSS/JS | Optional cached files via `sd_bus::open` + `streamFile` (`SdFile` is a `Stream`; each `read`/`available` takes the bus only for that call) |
| `/api/recordings/folders` | Directory enumerate (`open` / `openNextFile`) |
| `/api/recordings/list` | `summary.json` JSONL read |
| `/api/recordings/summary` | Stream `summary.json` |
| `/api/recordings/stream` | WAV range/body read in 512-byte chunks, then `sendContent` **without** holding the bus |
| Home / WS home / SD mount status | `totalBytes` / `usedBytes` / `cardType` |

Do **not** hold `sd_bus::Guard` across `sendContent`, `streamFile`’s Wi‑Fi write, or `delay()`. The WAV loop already yields between chunks so RecordTask can take the bus.

The browser often opens **two** Range connections; firmware allows two concurrent streams. Each stream locks only per syscall, so they interleave with recording instead of stacking raw `SD_MMC` calls.

## Related

- [SD_CARD_IO_OPTIMIZATION.md](./SD_CARD_IO_OPTIMIZATION.md) — fewer I/Os (flush batching, scan cache). This lock is about **safety**, not fewer operations.
- [SD_CARD_RECORDING_ANALYSIS.md](./SD_CARD_RECORDING_ANALYSIS.md) — recorder write path.
- Code: [`src/sd_bus.h`](../src/sd_bus.h), [`src/sd_bus.cpp`](../src/sd_bus.cpp)
