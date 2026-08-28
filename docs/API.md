# Boondock Device API Reference

All CLI responses and the unified web endpoint (`POST /api/cmd`) share the same
code path through `settings_getParam()` / `settings_setParam()`, so behaviour is
identical regardless of interface.

---

## Response Format

Every response is a single JSON line.

**Success:**

```json
{"status":"ok"}
{"status":"ok","param":"audio.audioThreshold","value":"50"}
```

**Error:**

```json
{"status":"error","code":"INVALID_VALUE","message":"audio.audioThreshold expects a number"}
```

### Error Codes

| Code | Meaning |
|------|---------|
| `UNKNOWN_CMD` | Command or endpoint not recognised |
| `INVALID_VALUE` | Value is the wrong type (e.g. string where number expected) |
| `OUT_OF_RANGE` | Value is outside acceptable bounds |
| `MISSING_PARAM` | Required parameter not provided |
| `HW_ERROR` | Hardware operation failed (SD card, WiFi, NVS, etc.) |
| `BUSY` | Device is busy (e.g. recording in progress) |
| `NOT_FOUND` | Unknown API endpoint (HTTP 404) |
| `INTERNAL` | Internal device error (HTTP 500) |

These codes are used identically by both CLI and web API.

---

## Unified Web Endpoint

### `POST /api/cmd`

The primary endpoint for test automation and programmatic access. Accepts one or
many get/set operations in a single request, all routed through the CLI settings
engine.

**Single command:**

```json
POST /api/cmd
Content-Type: application/json

{"action":"get","param":"audio.audioThreshold"}
```

```json
{"status":"ok","results":[{"param":"audio.audioThreshold","status":"ok","value":"50"}]}
```

**Batch (up to 50 commands):**

```json
POST /api/cmd
Content-Type: application/json

{"commands":[
  {"action":"get","param":"audio.minRecordingMs"},
  {"action":"get","param":"audio.maxRecordingMs"},
  {"action":"set","param":"audio.audioThreshold","value":"25"}
]}
```

```json
{"status":"ok","results":[
  {"param":"audio.minRecordingMs","status":"ok","value":"1000"},
  {"param":"audio.maxRecordingMs","status":"ok","value":"30000"},
  {"param":"audio.audioThreshold","status":"ok","value":"25"}
]}
```

**Mixed success/error:**

```json
{"status":"ok","results":[
  {"param":"audio.minRecordingMs","status":"ok","value":"1000"},
  {"param":"audio.audioThreshold","status":"error","code":"OUT_OF_RANGE","message":"audioThreshold must be 0-100"}
]}
```

SET operations in a batch are saved to NVS once at the end (single write),
not per-command.

---

## CLI Commands

All commands are case-insensitive. Send over serial (115200 baud).
Each command produces exactly one JSON line on stdout.

### Settings — Get / Set

#### `GET <param>` (aliases: `SHOW`, `READ`)

Read a single setting.

```
> GET audio.audioThreshold
{"status":"ok","audio.audioThreshold":"50"}
```

#### `SET <param> <value>` (aliases: `CHANGE`, `UPDATE`)

Write a single setting. Echoes back the accepted value.

```
> SET audio.audioThreshold 25
{"status":"ok","audio.audioThreshold":"25"}

> SET audio.audioThreshold abc
{"status":"error","code":"INVALID_VALUE","message":"audioThreshold expects a number"}

> SET audio.audioThreshold 999
{"status":"error","code":"OUT_OF_RANGE","message":"audioThreshold must be 0-100"}
```

### Settings — Bulk

| Command | Aliases | Description |
|---------|---------|-------------|
| `SAVE` | `STORE` | Persist current in-memory settings to NVS |
| `LOAD` | `RELOAD`, `REFRESH` | Reload settings from NVS into memory |
| `EXPORT` | `READCONFIG`, `DUMPCONFIG`, `DUMP` | Print all settings as `{"status":"ok","data":{...}}` |
| `IMPORT <json>` | `WRITECONFIG`, `APPLY` | Apply a JSON settings payload |
| `CONFIG <json>` | — | Apply JSON, report changes, wait for `DONE` to reboot |
| `CONFIG ?` | — | Print device configuration JSON |
| `AUTOCONFIG` | — | Quick setup (see below) |

#### `AUTOCONFIG`

Short form (WiFi only):

```
> AUTOCONFIG MySSID,MyPassword
{"status":"ok","action":"autoconfig","reboot_pending":true,"wifi_ssid":"MySSID"}
```

Full form (9 comma-separated values):

```
> AUTOCONFIG SSID,PASS,192.168.1.10,8080,5,50,1000,30000,200
{"status":"ok","action":"autoconfig","reboot_pending":true,"settings":{...}}
```

After either form, send `DONE` to trigger reboot.

### Device Info

| Command | Aliases | Response Fields |
|---------|---------|-----------------|
| `STATUS` | `INFO` | `storage`, `sd`, `sdFree`, `heap`, `psram`, `record`, `upload`, `queue`, `wifi`, `uptime`, `time`, `recordings`, `audio`, `api`, `config` |
| `STATUS_SHORT` | `STATUSSHORT` | `record`, `upload`, `queue`, `uptime`, `recorded`, `uploaded`, `audio` |
| `TIME` | `CLOCK` | `time` (ISO 8601), `epoch`, `uptime_ms` |
| `IP` | — | `connected`, `ip` |
| `MAC` | — | `mac` |
| `RECORDINGS` | `RECORDINGSSUMMARY` | `count`, `total_duration_ms`, `uploaded`, `errors` |
| `AUDIOLEVEL` | `AUDIOLEVELS`, `VU` | `currentLevel`, `currentDb`, `minLevel`, `minDb`, `maxLevel`, `maxDb`, `averageLevel`, `peakSample` |
| `ERRORS` | `ERROR` | `lastSequenceId`, `count`, `errors` (array of `{level, message, seq}`) |
| `HEALTH ?` | — | Prints health metrics JSON, then `{"status":"ok"}` |

### Device Actions

| Command | Aliases | Description |
|---------|---------|-------------|
| `REBOOT` | `RESET`, `RESTART` | Reboot the device |
| `FACTORYRESET` | `FACTORY`, `FACTORY_RESET` | Erase NVS and reboot. Returns `{"status":"ok","action":"factory_reset","reboot_pending":true}` |
| `MAINTENANCE` | `MAINT` | Run maintenance (pushes settings to server) |
| `PUSHSETTINGS` | — | Push settings JSON to API server |
| `PULLSETTINGS` | — | Pull settings from API server and apply |
| `PUSHLOGS` | `UPLOADLOGS`, `PUSHLOG`, `UPLOADLOG` | Upload latest log file to server |
| `SUMMARY` | `UPDATESUMMARY` | Update monthly/yearly recording summaries |
| `MAKEINDEX` | `MAKE-INDEX` | Reconcile recordings index |
| `FORMAT` | `FORMATSD`, `FORMATSDCARD` | Delete all files on SD card |
| `SAMPLE` | — | Start a sample recording (bypasses threshold) |
| `RECONNECT` | `RECONNECTWIFI`, `WIFI_RECONNECT` | Reconnect WiFi. Returns `{"status":"ok","connected":true,"ip":"..."}` |
| `RECOVER` | `RECOVERY` | Reset dead API endpoints. Returns `{"status":"ok","endpoints_reset":true,"all_dead":false,"queue_size":5}` |

### Direct-Set Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `TXPOWER` | `[1-10]` | Get or set WiFi TX power |
| `WEBSERVER` | `[on/off]` | Get or set webserver enabled state |
| `SETTIME` | `<ISO8601>` or `<epoch>` | Set device clock |
| `DEBUG` | `[true/false]` | Get or set debug log output |

### File Management (SD card)

| Command | Aliases | Arguments | Description |
|---------|---------|-----------|-------------|
| `CD` | `CHDIR` | `[path]` | Change/show current directory |
| `DIR` | `LS`, `LIST` | — | List files. Returns `{"status":"ok","directory":"/","entries":[...],"files":3,"dirs":1}` |
| `RM` | `DELETE`, `DEL` | `<file>` or `*` | Delete file(s). Returns `{"status":"ok","deleted":"path"}` or `{"status":"ok","deleted":5,"failed":0}` |

---

## Settings Parameters

All parameter names are case-insensitive.

### Audio

| Parameter | Type | Range | Default |
|-----------|------|-------|---------|
| `audio.sampleRate` | int | — | 8000 |
| `audio.bufferSamples` | int | — | 512 |
| `audio.audioThreshold` | int | 0–100 | 50 |
| `audio.preRecordMs` | int | ≥ 0 | 200 |
| `audio.minRecordingMs` | int | > 0 | 1000 |
| `audio.maxRecordingMs` | int | > 0 | 30000 |
| `audio.silenceThresholdMs` | int | > 0 | 1000 |
| `audio.codecGain` | int | — | 0 |

### Upload

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `upload.queueDepth` | int | — | Max upload queue size |
| `upload.convertToMp3` | bool | — | Convert recordings to MP3 |
| `upload.apiHosts[N]` | string | N: 0–2 | API host address |
| `upload.apiPorts[N]` | int | N: 0–2 | API host port |
| `upload.enabled[N]` | bool | N: 0–2 | Enable/disable endpoint |

### WiFi (indexed, up to 3 networks)

| Parameter | Type | Description |
|-----------|------|-------------|
| `wifi[N].ssid` | string | Network name |
| `wifi[N].password` | string | Network password |
| `wifi[N].connectTimeoutMs` | int | Connection timeout |
| `wifi[N].staticIpEnabled` | bool | Use static IP |
| `wifi[N].staticIp` | string | Static IP address |
| `wifi[N].staticSubnet` | string | Subnet mask |
| `wifi[N].staticGateway` | string | Gateway address |
| `wifi[N].staticDns1` | string | Primary DNS |
| `wifi[N].staticDns2` | string | Secondary DNS |

### SD Card

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `sdCard.useSdCard` | bool | — | Enable SD card |
| `sdCard.recordToSdCard` | bool | — | Record audio to SD |
| `sdCard.mode1bit` | bool | — | Use 1-bit SD mode |
| `sdCard.frequency` | int | 1000000–20000000 | SD bus frequency |
| `sdCard.formatIfMountFailed` | bool | — | Auto-format on mount failure |

### Timezone

| Parameter | Type | Range |
|-----------|------|-------|
| `timezone.offsetHours` | int | −12 to +14 |

### RTC

| Parameter | Type | Description |
|-----------|------|-------------|
| `rtc.enabled` | bool | Enable RTC |
| `rtc.sdaPin` | int | I2C SDA pin |
| `rtc.sclPin` | int | I2C SCL pin |

### Other

| Parameter | Type | Range |
|-----------|------|-------|
| `txpower` / `wtp` / `wifitxpower` | int | 1–10 |
| `webserverEnabled` / `webserver` / `wse` | bool | — |

### Shortcuts

These short aliases resolve to their full parameter names:

| Shortcut | Resolves To |
|----------|-------------|
| `min` | `audio.minRecordingMs` |
| `max` | `audio.maxRecordingMs` |
| `pre` / `preroll` / `prerecord` | `audio.preRecordMs` |
| `silence` | `audio.silenceThresholdMs` |
| `gain` | `audio.codecGain` |
| `athr` | `audio.audioThreshold` |
| `hostN` | `upload.apiHosts[N]` |
| `ssidN` | `wifi[N].ssid` |
| `passN` / `pwdN` | `wifi[N].password` |
| `ipN` | `wifi[N].staticIp` |
| `subnetN` / `maskN` | `wifi[N].staticSubnet` |
| `gatewayN` / `gwN` | `wifi[N].staticGateway` |
| `dns1N` | `wifi[N].staticDns1` |
| `dns2N` | `wifi[N].staticDns2` |
| `staticN` | `wifi[N].staticIpEnabled` |

---

## Settings validation and out-of-range behaviour

All settings that accept numeric values from CLI or `POST /api/cmd` go through the same validation in `settings_setParam()`. Invalid or out-of-range values are handled in one of two ways: **reject** or **clamp**.

### Type validation

For numeric parameters, the value must be a valid integer (optional leading `+` or `-`, then digits). Non-numeric input (e.g. `SET athr abc`) is **rejected**:

- **CLI:** `{"status":"error","code":"INVALID_VALUE","message":"audio.audioThreshold expects a number"}`
- **API:** HTTP 422, same JSON body.

### Reject (value not stored)

If the value is the wrong type or outside the allowed range, the setting is **not** changed. The call returns failure and the client gets an error with `code` and `message`.

| Parameter | Validation | When out of range |
|-----------|------------|-------------------|
| `audio.audioThreshold` (athr) | Integer, 0–100 | `ERR_OUT_OF_RANGE`, "audio.audioThreshold must be 0-100" |
| `timezone.offsetHours` | Integer, −12 to +14 | `ERR_OUT_OF_RANGE`, "timezone.offsetHours must be -12 to +14" |
| `sdCard.frequency` | Integer, 1 000 000–20 000 000 | `ERR_OUT_OF_RANGE`, "sdCard.frequency must be 1000000-20000000" |
| `txpower` / `wifitxpower` | Integer, 1–10 | `ERR_OUT_OF_RANGE`, "wifiTxPower must be 1-10" |
| Index parameters | e.g. `wifi[N]`, `upload.apiHosts[N]` | `ERR_OUT_OF_RANGE` if index &gt; max (e.g. "wifi index out of range") |

Example: `SET athr 150` → error, athr stays at previous value.

### Clamp (value adjusted, then stored)

Some parameters **sanitize** the value to the nearest allowed value and then store that. The **stored** (clamped) value is what is echoed back.

| Parameter | Allowed range / set | Clamp behaviour |
|-----------|----------------------|------------------|
| `audio.sampleRate` | 8000 only | Any other value → 8000 |
| `audio.bufferSamples` | 512–4096 | Below 512 → 512; above 4096 → 4096 |
| `audio.preRecordMs` (pre) | 0–500 | Negative → 0; above 500 → 500 |
| `audio.codecGain` (gain) | Discrete: −3, 0, 3, 6, 9, 12, 15, 18, 21, 24 dB | Rounded to **nearest** allowed value |
| `upload.queueDepth` | 4–32 | Below 4 → 4; above 32 → 32 |

Example: `SET pre 1000` → accepted, stored value is 500; response shows `"value":"500"`.

### No range check (SET path only)

These parameters only require a valid integer; there is no min/max check in `settings_setParam()`:

- `audio.minRecordingMs` (min)
- `audio.maxRecordingMs` (max)
- `audio.silenceThresholdMs` (silence)

So values like `0` or negative can be stored. The **AUTOCONFIG** command does enforce min &gt; 0, max &gt; 0, and max ≥ min for its arguments.

### Summary

| Behaviour | Client sees | Setting changed? |
|-----------|-------------|-------------------|
| **Reject** (wrong type or out of range) | `{"status":"error","code":"INVALID_VALUE" or "OUT_OF_RANGE","message":"..."}` | No |
| **Clamp** | `{"status":"ok","param":"...","value":"<clamped_value>"}` | Yes, to clamped value |

---

## Legacy Web API Endpoints

These endpoints are used by the built-in web UI. For new integrations and
test automation, prefer `POST /api/cmd`.

### Read-Only (GET)

| Path | Description |
|------|-------------|
| `/api/device-info` | Device identity (MAC, firmware, etc.) |
| `/api/home/summary` | Dashboard summary (storage, memory, WiFi, audio, recordings) |
| `/api/audio/stats` | Real-time audio level statistics |
| `/api/audio/settings` | Current audio configuration |
| `/api/advanced/sd-card-settings` | SD card configuration |
| `/api/advanced/wifi-tx-power-settings` | WiFi TX power setting |
| `/api/network/config` | WiFi network configuration (passwords masked) |
| `/api/advanced/export-settings` | Full settings JSON export |
| `/api/firmware/check` | Check for firmware updates |

### Write (POST)

| Path | Description | HTTP Error Codes |
|------|-------------|-----------------|
| `/api/cmd` | **Unified get/set endpoint** (see above) | 400, 422 |
| `/api/audio/settings` | Set a single audio parameter | 400, 422 |
| `/api/audio/defaults` | Reset audio settings to factory defaults | 500 |
| `/api/audio/save` | Save + push audio settings | 500 |
| `/api/network/save` | Save WiFi network configuration | 400, 422, 500 |
| `/api/advanced/reboot` | Reboot the device | — |
| `/api/advanced/set-default` | Reset all settings to defaults | 500 |
| `/api/advanced/factory-reset` | Factory reset (erase NVS + reboot) | 500 |
| `/api/advanced/import-settings` | Import full settings JSON | 400, 500 |
| `/api/advanced/push-settings` | Push settings to cloud API | 500 |
| `/api/advanced/pull-settings` | Pull settings from cloud API | 500 |
| `/api/advanced/update-firmware` | Upload firmware binary | 400, 500 |
| `/api/firmware/apply` | Apply firmware from URL | 400, 503 |
| `/api/advanced/sd-card-test` | Test SD card read/write | — |

All POST endpoints return JSON with the standard `{"status":"ok",...}` or
`{"status":"error","code":"...","message":"..."}` envelope.

### HTTP Status Codes

| Code | Meaning |
|------|---------|
| 200 | Success |
| 400 | Malformed request (bad JSON, missing fields) |
| 404 | Unknown route |
| 422 | Valid JSON but values out of range or wrong type |
| 500 | Internal device error |
| 503 | Service unavailable (e.g. WiFi not connected) |
