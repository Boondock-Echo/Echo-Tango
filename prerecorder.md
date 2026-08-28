# Pre-record buffer (5 seconds) changes

This document captures the changes made to support longer **pre-recording** (audio captured *before* the trigger) on Boondock ECHO/TANGO firmware.

## Summary

- **Max pre-record**: **5000 ms (5 seconds)**
- **Default pre-record**: **3000 ms (3 seconds)**
- **UI slider step**: **500 ms**
- **Implementation detail**: The pre-record ring + scratch buffers were moved out of static DRAM (`.bss`) and are now allocated dynamically (prefer PSRAM) to prevent linker DRAM overflow.

## Why this change was required

The original pre-record implementation used fixed-size static arrays sized for a small window (previously 500 ms). Expanding that to 5 seconds increases the buffer size by 10x and caused the firmware to fail to link with:

- `section '.dram0.bss' will not fit in region 'dram0_0_seg'`

To make 5 seconds feasible, the buffers had to be removed from `.dram0.bss`.

## Firmware changes

### 1) Increase pre-record window to 5 seconds

File: `src/recorder.cpp`

- `kPreRecordBufferWindowMs` updated to **5000**.
- Ring capacity is still computed using `kPreRecordMaxSampleRate` (currently 8000 Hz):
  - samples = \(8000 * 5000 / 1000 = 40000\) samples

### 2) Allocate pre-record buffers dynamically (prefer PSRAM)

File: `src/recorder.cpp`

- Replaced static arrays:
  - `int16_t preRecordRing[...]`
  - `int16_t preRecordScratch[...]`
- With pointers:
  - `int16_t* preRecordRing`
  - `int16_t* preRecordScratch`
- Added `ensurePreRecordBuffers()` which:
  - Allocates using `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` when available
  - Falls back to `malloc()` if PSRAM allocation fails
  - If allocation fails, logs an error and effectively disables pre-record for that run (no crash).

This eliminates large `.bss` growth and keeps DRAM usage within limits.

### 3) Settings validation: allow up to 5000 ms

File: `src/settings.cpp`

- `sanitizePreRecordMs()` clamp changed from **500** to **5000**

### 4) Default pre-record increased to 3000 ms

File: `src/config.h`

- `DEFAULT_AUDIO_PRE_RECORD_MS` changed to **3000**

This affects:
- boot defaults
- the API “reset to defaults” endpoint (`/api/audio/defaults`)

## Web UI changes

File: `src/app_js_spa.h`

- Slider range updated:
  - `min=0`
  - `max=5000`
  - default `value=3000`
- Slider step updated:
  - `step=500`
- Help text updated to mention tuning up to **5000 ms (5 seconds)**.

## How to test

1. Flash the updated firmware.
2. Open the web UI → Audio settings.
3. Confirm:
   - pre-record defaults to **3000 ms**
   - you can increase it to **5000 ms**
4. Record speech that previously clipped the first words.
5. Compare captures at:
   - `3000 ms`
   - `5000 ms`

## Notes / constraints

- The pre-record buffer is sized assuming up to **8 kHz** sample rate (`kPreRecordMaxSampleRate=8000`).
  - If the device is configured to sample higher than 8 kHz, the effective pre-record duration may be less than the configured ms (because the ring is capped by sample count).
- Dynamic allocation relies on PSRAM when present; without PSRAM, it will attempt heap allocation, which may reduce available memory for other features.

