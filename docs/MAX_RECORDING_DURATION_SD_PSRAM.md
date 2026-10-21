# Max Recording Duration — SD Card vs PSRAM Cap

**Date:** 2026-07-29  
**Related files:** `common.cpp` / `common.h`, `settings.cpp` / `settings.h`, `recorder.cpp`, `main.cpp`, `boondock_server.cpp`, `app_js_spa.h`, `config.h`

---

## Summary

Maximum recording length is now mode-aware:

| Condition | Max recording setting / clip length |
|-----------|-------------------------------------|
| SD card **present** and recording mode is **SD card** | **180 seconds** (3 minutes) |
| SD card **not present**, or recording mode is **not SD** (PSRAM) | **30 seconds** |

Previously, the stored setting `audio.maxRecordingMs` could remain **180000 ms** even when the device was recording to PSRAM. Runtime recording in PSRAM was already capped near 30s, but the setting, API, CLI, and UI could still show or accept **180s**, which was confusing and could overwrite a valid SD value via a UI race.

---

## Constants (unchanged absolute limits)

```111:111:src/config.h
#define DEFAULT_AUDIO_MAX_RECORDING_MS 180000
```

```36:36:src/common.h
constexpr uint32_t kPsramMaxRecordingMs = 30000; // 30 seconds max in PSRAM mode
```

- **180000 ms** — hard ceiling when recording to an available SD card  
- **30000 ms** — ceiling for PSRAM / non-SD recording mode  

---

## Previous behavior (before this change)

### 1. Settings always allowed up to 180s

`settings.cpp` validated `maxRecordingMs` only against `DEFAULT_AUDIO_MAX_RECORDING_MS` (180s). It did **not** check whether the device was in SD recording mode or whether an SD card was mounted.

```text
1s ≤ audio.maxRecordingMs ≤ 180s   (always)
```

So API / CLI / MQTT / JSON could save **180s** while the device was in PSRAM mode.

### 2. Recorder: runtime PSRAM cap only

In PSRAM mode the recorder already did:

```cpp
std::min(appSettings.audio.maxRecordingMs, kPsramMaxRecordingMs)  // → ≤ 30s
```

In SD mode it used the raw setting:

```cpp
appSettings.audio.maxRecordingMs  // → up to 180s
```

**Effect:** actual PSRAM clips stopped around 30s, but the **setting** could still say 180s.

### 3. UI slider race could force 30s even in SD mode

`updateMaxRecordingLimit()` in `app_js_spa.h`:

- Set slider max to **180** only if both SD checkboxes were checked  
- Set slider max to **30** otherwise  
- If the current value was above the new max, it **clamped and saved** immediately  

Audio settings often loaded **before** SD checkboxes were populated. Both checkboxes defaulted to unchecked → limit became 30 → **saved 30s back to the device**, even when SD recording was enabled and 180s was intended.

### 4. UI did not consider physical SD presence

The old UI only looked at checkbox state (`useSdCard && recordToSdCard`), not whether the card was actually mounted. A device with “Record to SD” enabled but **no card** could still show a **180s** slider max.

---

## What changed

### 1. Shared ceiling: `getMaxAllowedRecordingMs()`

```1630:1638:src/common.cpp
uint32_t getMaxAllowedRecordingMs()
{
    // 180s only when recording to an available SD card; otherwise PSRAM limit (30s)
    if (isRecordingModeSdCard())
    {
        return static_cast<uint32_t>(DEFAULT_AUDIO_MAX_RECORDING_MS);
    }
    return kPsramMaxRecordingMs;
}
```

`isRecordingModeSdCard()` is true only when:

- `appSettings.sdCard.recordToSdCard` is enabled, **and**
- storage mode is SD (`isStorageModeSdCard()` — card mounted / available)

Otherwise the ceiling is **30s**.

Declared in `common.h` as `uint32_t getMaxAllowedRecordingMs();`.

---

### 2. Settings enforcement: `settings_enforceMaxRecordingLimit()`

```2335:2363:src/settings.cpp
bool settings_enforceMaxRecordingLimit(bool saveIfChanged)
{
    const uint32_t allowed = getMaxAllowedRecordingMs();
    bool changed = false;

    if (appSettings.audio.maxRecordingMs > allowed)
    {
        // ... clamp maxRecordingMs to allowed ...
        changed = true;
    }

    if (appSettings.audio.minRecordingMs > appSettings.audio.maxRecordingMs)
    {
        // ... clamp minRecordingMs down if needed ...
        changed = true;
    }

    if (changed && saveIfChanged)
    {
        settings_save();
    }
    return changed;
}
```

**Called when:**

| Trigger | Where |
|---------|--------|
| After storage init at boot | `main.cpp` (after `ensureStorage()`) |
| SD settings changed (`useSdCard` / `recordToSdCard`) | `settings_setParam` + JSON apply |
| JSON load when SD recording is not configured | End of `jsonToAppSettings` (settings-flag clamp to 30s; safe before mount) |

**Boot-safe JSON load:** while applying NVS JSON, max is first capped only to the absolute **180s** ceiling (storage may not be mounted yet). After SD keys are applied, if SD recording is **not** configured, max is capped to **30s**. Physical presence is enforced later via `settings_enforceMaxRecordingLimit()` after `ensureStorage()`.

**`settings_setParam("audio.maxRecordingMs", …)`** and CLI / JSON paths now clamp with `getMaxAllowedRecordingMs()` so a PSRAM / no-card device cannot keep or set values above 30s.

---

### 3. Recorder uses the effective max everywhere

```2019:2064:src/recorder.cpp
        size_t peakRecordingBytes;
        const uint32_t effectiveMaxRecordingMs =
            std::min(appSettings.audio.maxRecordingMs, getMaxAllowedRecordingMs());
        if (isRecordingModePsram())
        {
            // ... peak bytes from effectiveMaxRecordingMs ...
        }
        else
        {
            // ... peak bytes from effectiveMaxRecordingMs ...
        }
        // ...
        if (isRecordingModePsram())
        {
            maxDurationReached = liveBytes >= peakRecordingBytes;
        }
        else
        {
            maxDurationReached = elapsed >= effectiveMaxRecordingMs;
        }
```

PSRAM buffer allocation and start-recording logs also use `getMaxAllowedRecordingMs()` instead of only `kPsramMaxRecordingMs` / raw setting.

**Effect:** even if a stale setting of 180s remains briefly, runtime recording still stops at the correct ceiling for the current mode.

---

### 4. API exposes the ceiling

`/api/audio/settings` and `/api/advanced/sd-card-settings` now include:

- `maxAllowedRecordingMs` — current ceiling (30000 or 180000)  
- `sdCardMounted` — whether SD storage is active  
- Audio settings also return `useSdCard` / `recordToSdCard` so the UI can sync checkboxes without waiting on a separate call  

Audio “Set Defaults” uses `getMaxAllowedRecordingMs()` instead of always writing 180000.

---

### 5. UI: mode-aware slider without destructive load race

```3808:3841:src/app_js_spa.h
function updateMaxRecordingLimit(options) {
    // ...
    // Prefer server-reported ceiling when available; otherwise:
    // maxLimit = (useSdCard && recordToSdCard && sdMounted) ? 180 : 30
    //
    // Only persist a clamp when options.saveClamp === true
}
```

| Call site | Behavior |
|-----------|----------|
| Page init / `loadAudioSettings` | Update slider max; **do not** auto-save |
| User toggles SD checkboxes | Clamp + **save** if value exceeds new max |
| `loadSdCardSettings` | Use server `maxAllowedRecordingMs`; may save clamp |

This fixes the old race where loading audio settings before SD checkboxes were ready wrote **30s** into NVS while SD mode was actually enabled.

Slider HTML max updated from `300` to `180` to match the real product limit.

---

## Problems solved

| Problem | Solution |
|---------|----------|
| Setting could be **180s** in PSRAM / no-SD mode | Settings + enforce clamp to **30s** when not in SD recording mode |
| UI showed / accepted 180s without a card | Ceiling uses `isRecordingModeSdCard()` + UI uses `sdCardMounted` |
| UI could **overwrite** a valid 180s SD setting with 30s on page load | `saveClamp: false` on load; only save when user changes SD options or server reports a clamp after SD load |
| Recorder SD path ignored mode ceiling | Always uses `effectiveMaxRecordingMs = min(setting, getMaxAllowedRecordingMs())` |
| Defaults / API / CLI inconsistent with mode | Shared `getMaxAllowedRecordingMs()` used across layers |

---

## Decision flow

```text
                    ┌─────────────────────────────┐
                    │ getMaxAllowedRecordingMs()  │
                    └──────────────┬──────────────┘
                                   │
                    isRecordingModeSdCard()?
                     /                    \
                   yes                     no
                    │                       │
            recordToSdCard &&         return 30000
            SD mounted                (30 seconds)
                    │
            return 180000
            (180 seconds)
```

```text
User / API sets audio.maxRecordingMs
        │
        ▼
Clamp to getMaxAllowedRecordingMs()
        │
        ▼
Store in appSettings (+ NVS)
        │
        ▼
Recorder stop condition uses
min(stored setting, getMaxAllowedRecordingMs())
```

---

## Files touched

| File | Change |
|------|--------|
| `src/common.h` / `src/common.cpp` | Added `getMaxAllowedRecordingMs()` |
| `src/settings.h` / `src/settings.cpp` | Added `settings_enforceMaxRecordingLimit()`; clamp on set/JSON/CLI/SD toggle |
| `src/recorder.cpp` | Effective max for buffer size and max-duration stop |
| `src/main.cpp` | Enforce after `ensureStorage()` at boot |
| `src/boondock_server.cpp` | API fields + mode-aware audio defaults |
| `src/app_js_spa.h` | Safer `updateMaxRecordingLimit()`; slider max 180 |

---

## Manual test checklist

1. **No SD / PSRAM mode** — set max recording via UI/API above 30s → stored value and slider max stay ≤ **30**.  
2. **SD present + Record to SD** — slider max becomes **180**; can set and keep values up to 180s.  
3. **Disable Record to SD** (or disable SD) while max was 120s → value clamps to **30** and is saved.  
4. **Reload web UI** with SD recording enabled and max = 120s → value remains **120** (no accidental save of 30 on load).  
5. **PSRAM recording** — long continuous sound stops around **30s** even if a stale higher setting existed before enforce.  
6. **SD recording** — long continuous sound can run up to the configured max (≤ **180s**).
