# Serial CLI Documentation

This document defines all Serial CLI commands, message formats, response codes, and settings parameter codes for the Boondock firmware. This documentation is intended for tools that monitor and parse Serial port messages.

---

## Table of Contents

1. [CLI Command Format](#cli-command-format)
2. [Settings commands](#settings-commands)
3. [Information commands](#information-commands)
4. [File management commands](#file-management-commands)
5. [Message Types and Codes](#message-types-and-codes)
6. [Settings Parameter Codes](#settings-parameter-codes)

---

## CLI Command Format

### Input Format
- Commands are sent via Serial at 115200 baud
- Commands are case-insensitive (automatically converted to uppercase)
- Commands are terminated by newline (`\n`) or carriage return + newline (`\r\n`)
- Maximum command length: 512 characters
- Command echo: All commands are echoed back with `> ` prefix

### Response format (JSON)

Implementation: `src/settings.cpp`. **Every CLI command prints exactly one JSON line** to Serial (plus the echoed input line with `> ` prefix). This keeps machine parsing deterministic.

- **Success (no extra fields)**: `{"status":"ok"}`
- **Success (with data)**: `{"status":"ok", "<key>": "<value>"}` or a larger object with `"status":"ok"` included (see below).
- **Error**: `{"status":"error","code":"<CODE>","message":"<text>"}`  
  Common `code` values: `UNKNOWN_CMD`, `INVALID_VALUE`, `OUT_OF_RANGE`, `MISSING_PARAM`, `HW_ERROR`, `BUSY`.

Empty or missing parameter values are returned as normal strings (often empty `""`) inside the success object; there is no separate `(empty)` token in JSON mode.

### Command structure
```
<COMMAND> [ARGUMENTS...]
```

---

## Settings commands

### HELP / ?
**Format**: `help` or `?`

**Response**: One JSON object with `status`, `commands` (array of command summary strings), and `shortcuts` (human-readable hints). Example shape:
```json
{"status":"ok","commands":["help","show <setting>", "..."],"shortcuts":["min/max/silence/gain/athr -> audio fields","..."]}
```

---

### SHOW / GET / READ
**Format**: `show <setting>` or `get <setting>` or `read <setting>`

**Response**:
- **Success**: `{"status":"ok","<setting>":"<value>"}` where the first key after `status` is the **exact parameter string** you sent (e.g. `min` → `"min"`).
- **Failure**: `{"status":"error","code":"MISSING_PARAM","message":"usage: show <setting>"}`

---

### SET / CHANGE / UPDATE
**Format**: `set <setting> <value>` (aliases: `change`, `update`)

**Response**:
- **Success**: `{"status":"ok","<setting>":"<accepted_value>"}` (accepted value after validation).
- **Failure**: error JSON with `INVALID_VALUE`, `OUT_OF_RANGE`, etc.

---

### SAVE / STORE
**Format**: `save` or `store`

**Response**: `{"status":"ok"}` or error JSON.

---

### LOAD / RELOAD / REFRESH
**Format**: `load` or `reload` or `refresh`

**Response**: `{"status":"ok"}` or error JSON.

---

### EXPORT / DUMPCONFIG / DUMP / READCONFIG
**Format**: `export` or `dumpconfig` or `dump` or `readconfig` (also `readconfigurations`)

**Response**: Reloads from NVS, then prints one line:
```json
{"status":"ok","data":{ ... full settings document ... }}
```
If nothing is stored, `data` may effectively be empty/`{}` depending on serializer behavior.

---

### IMPORT / WRITECONFIG / APPLY
**Format**: `import <json>` or `writeconfig <json>` or `apply <json>`

The firmware finds the first `{` through last `}` in the remainder of the line and parses that as JSON.

**Response**: `{"status":"ok"}` on success, or error JSON (`MISSING_PARAM` for missing payload, `INVALID_VALUE` for bad JSON or apply failure).

---

### CONFIG (snapshot, no arguments)
**Format**: `config` alone (no `?`, no JSON object)

**Response**: JSON with `host`, `port`, `mac`, `ip`, `minRecordingMs`, `maxRecordingMs`, `silenceThresholdMs`, `audioThreshold`, `codecGainDb`, and when SD is mounted `sdTotalBytes` / `sdFreeBytes`. Primary host/port are `upload.apiHosts[0]` / `upload.apiPorts[0]`. Includes `"status":"ok"`.

---

### CONFIG (with JSON)
**Format**: `config <json>`

Applies settings like `import`, tracks changed keys, then prints a **first** JSON line:
```json
{"status":"ok","action":"config_applied","reboot_pending":true,"changes":[{"key":"...","old":"...","new":"..."}]}
```
Sensitive values appear as `(hidden)` in `changes`. The handler then **blocks** waiting for a line `DONE` (case-insensitive). On `DONE`, it prints `{"status":"ok"}`, waits 500 ms, and reboots. (There is no timed auto-reboot in this path—only `DONE` triggers exit.)

**Failure**: error JSON if JSON is missing/invalid or apply fails.

---

### CONFIG ? (print configuration JSON)
**Format**: `config ?`

**Response**: Sends the periodic **`ty` / `config` message** (see [Message Types](#message-types-and-codes)), then `{"status":"ok"}`.

---

### AUTOCONFIG
**Format**:
- Short: `autoconfig <SSID>,<PASSWORD>`
- Full: `autoconfig <SSID>,<PASSWORD>,<HOST_IP>,<HOST_PORT>,<TIMEZONE_OFFSET>,<AUDIO_THRESHOLD>,<MIN_RECORDING_MS>,<MAX_RECORDING_MS>,<PRE_RECORDING_MS>` (nine comma-separated fields after the first comma)

**Short response**: One line such as:
```json
{"status":"ok","action":"autoconfig","reboot_pending":true,"wifi_ssid":"<SSID>"}
```
Then the firmware waits indefinitely for a line `DONE` to reboot (no `{"status":"ok"}` line before reboot in this path).

**Full response**: After save, one line such as:
```json
{"status":"ok","action":"autoconfig","reboot_pending":true,"settings":{"wifi_ssid":"...","upload_host":"...","upload_port":7001,"timezone_offset":-5,"audio_threshold":50,"min_recording_ms":1000,"max_recording_ms":30000,"pre_record_ms":0}}
```
Then it waits up to **2 seconds** for `DONE` (early reboot) or reboots automatically.

**Failure**: error JSON (wrong arity, bad port/tz/threshold, save failure, etc.).

---

### PUSHSETTINGS
**Format**: `pushsettings`

**Response**: `{"status":"ok"}` if the masked settings JSON was pushed successfully; otherwise error JSON (e.g. `HW_ERROR`).

---

### PULLSETTINGS
**Format**: `pullsettings`

**Response**: `{"status":"ok"}` if settings were pulled and applied; error JSON on network/empty response/apply failure.

---

### TXPOWER / TX_POWER
**Format**: `txpower` or `tx_power` — optional argument `1`–`10`

**Response**:
- No argument: `{"status":"ok","wifiTxPower":<1-10>,"esp32Value":<mapped>}`
- With argument: `{"status":"ok","wifiTxPower":"<value>"}` after save and notify (WiFi TX power applied if connected).
- Invalid range: `OUT_OF_RANGE`

---

### WEBSERVER / WEB_SERVER
**Format**: `webserver` or `web_server` — optional `on|off|true|false|1|0|yes|no`

**Response**:
- No argument: `{"status":"ok","webserverEnabled":"true"|"false"}`
- With argument: updates persisted flag, then same success shape or `HW_ERROR` on save failure.

---

## Information commands

### TIME / CLOCK
**Format**: `time` or `clock`

**Response**: One JSON line, for example:
```json
{"status":"ok","time":"2025-03-24T12:00:00.000Z","epoch":1742817600,"uptime_ms":12345}
```

---

### STATUS / INFO
**Format**: `status` or `info`

**Response**: One JSON object with `"status":"ok"` and comprehensive fields (structure below):
```json
{
  "storage": "SD"|"PS",
  "sd": true|false,
  "sdFree": <percentage>|null,
  "heap": {
    "total": <bytes>,
    "free": <bytes>,
    "used": <bytes>,
    "minFree": <bytes>,
    "FreeBlock": <bytes>
  },
  "psram": {
    "total": <bytes>|null,
    "free": <bytes>|null,
    "used": <bytes>|null
  },
  "record": true|false,
  "upload": true|false,
  "queue": <count>,
  "wifi": {
    "conn": true|false,
    "ssid": "<ssid>"|null,
    "ip": "<ip>"|null,
    "rssi": <rssi>|null
  },
  "uptime": <seconds>,
  "time": {
    "valid": true|false,
    "RTC": true|false,
    "time": "<ISO_time>"|null,
    "epoch": <seconds>|null,
    "uptime": <seconds>
  },
  "recordings": {
    "total": <count>,
    "duration": <ms>,
    "uploaded": <count>,
    "error": <count>,
    "lastEpoch": <seconds>|null,
    "lastTime": "<ISO_time>"|null
  },
  "audio": {
    "currentDb": <dB>,
    "minDb": <dB>,
    "maxDb": <dB>,
    "avgDb": <dB>,
    "currentDynamic": <percentage>
  },
  "api": {
    "dead": true|false,
    "recovery": 0,
    "lastUpload": "<ISO_time>"|null,
    "lastEvent": "<ISO_time>"|null,
    "Events": <count>,
    "Uploads": <count>
  },
  "config": {
    "mac": "<mac>",
    "fw": "<version>",
    "host": "<host>"|null,
    "port": <port>|null,
    "athr": <threshold>,
    "min": <ms>,
    "max": <ms>,
    "silence": <ms>,
    "gain": <dB>,
    "samples": <rate>,
    "pre": <ms>
  }
}
```
See `settings.cpp` (`STATUS` handler) for the authoritative field set.

---

### STATUS_SHORT / STATUSSHORT
**Format**: `status_short` or `statusshort`

**Response**: Compact JSON with `record`, `upload`, `queue`, `uptime`, `recorded`, `uploaded`, nested `audio` (dB), and `"status":"ok"`.

---

### SUMMARY / UPDATESUMMARY
**Format**: `summary` or `updatesummary`

**Response**: Requires SD and valid time: `{"status":"ok","monthly":true|false,"yearly":true|false}`. Errors if no SD or time not synced (`HW_ERROR`).

---

### IP
**Format**: `ip`

**Response**: `{"status":"ok","connected":true|false,"ip":"..."}` (when disconnected, `ip` may be the string `"null"`).

---

### MAC
**Format**: `mac`

**Response**: `{"status":"ok","mac":"<device_id>"}`

---

### RECORDINGS / RECORDINGSSUMMARY
**Format**: `recordings` or `recordingssummary`

**Response**: `{"status":"ok","count":...,"total_duration_ms":...,"uploaded":...,"errors":...}`

---

### AUDIOLEVEL / AUDIOLEVELS / VU
**Format**: `audiolevel` or `audiolevels` or `vu`

**Response**: JSON with `currentLevel`, `currentDb`, `minLevel`, `minDb`, `maxLevel`, `maxDb`, `averageLevel`, `peakSample`, plus `"status":"ok"`.

---

### ERRORS / ERROR
**Format**: `errors` or `error`

**Response**: `lastSequenceId`, `count` (fetched up to 50), `errors` array (up to **10** entries with `level`, `message`, `seq`), and `"status":"ok"`.

---

### SETTIME / SET_TIME
**Format**: `settime <YYYY-MM-DDTHH:MM:SSZ>` or `settime <epoch_seconds>`

**Response**: Success `{"status":"ok","time":"<YYYY-MM-DDTHH:MM:SSZ>"}` or error JSON (`MISSING_PARAM`, `INVALID_VALUE`, `OUT_OF_RANGE`, `HW_ERROR`).

---

### HEALTH ?
**Format**: `health ?` (the `?` argument is required)

**Response**: Emits the **`ty` / `health` JSON** (see [Message Types](#message-types-and-codes)), then `{"status":"ok"}`.

---

### DEBUG
**Format**: `debug` or `debug <true|false|0|1|on|off|yes|no>`

**Response**: Read returns `{"status":"ok","debug":"true"|"false"}` (reflects both `log.serialInfo` and `log.serialDebug`). Write updates those flags, saves NVS, returns the same shape or `HW_ERROR`.

---

### REBOOT / RESET / RESTART
**Format**: `reboot` or `reset` or `restart`

**Response**: `{"status":"ok"}` then immediate reboot.

---

### FACTORYRESET / FACTORY / FACTORY_RESET
**Format**: `factoryreset` or `factory` or `factory_reset`

**Response**: `{"status":"ok","action":"factory_reset","reboot_pending":true}` then delay and reboot.

---

### MAINTENANCE / MAINT
**Format**: `maintenance` or `maint`

**Response**: `{"status":"ok"}` if masked settings push succeeds; else error JSON.

---

### MAKEINDEX / MAKE-INDEX
**Format**: `makeindex` or `make-index`

**Response**: `{"status":"ok"}` (no further output in current firmware).

---

### PUSHLOGS / UPLOADLOGS
**Format**: `pushlogs` or `uploadlogs` or `pushlog` or `uploadlog`

**Response**: Success `{"status":"ok","path":"<log_file_path>"}`; errors for missing SD, invalid time, missing file, or upload failure.

---

### FORMAT / FORMATSD / FORMATSDCARD
**Format**: `format` or `formatsd` or `formatsdcard`

**Response**: `{"status":"ok"}` after clearing SD root files and remounting, or error JSON.

---

### RECOVER / RECOVERY
**Format**: `recover` or `recovery`

**Response**: JSON with `endpoints_reset`, `all_dead`, `queue_size` (may attempt WiFi connect first).

---

### RECONNECT / RECONNECTWIFI / WIFI_RECONNECT
**Format**: `reconnect` or `reconnectwifi` or `wifi_reconnect`

**Response**: `{"status":"ok","connected":true|false,"ip":"..."}` when connected.

---

### SAMPLE
**Format**: `sample`

**Response**: `{"status":"ok"}` on success; `BUSY` if already recording; `HW_ERROR` if start fails.

---

## File management commands (SD card)

### CD / CHDIR
**Format**: `cd` or `chdir` — optional path

**Response**: `{"status":"ok","directory":"<path>"}` or error JSON.

---

### DIR / LS / LIST
**Format**: `dir` or `ls` or `list`

**Response**: JSON with `directory`, `entries` (name, type, optional `size`), `files`, `dirs`, `"status":"ok"`.

---

### RM / DELETE / DEL
**Format**: `rm <file>` or `rm *`

**Response**: Single file: `{"status":"ok","deleted":"<path>"}`. Wildcard: `{"status":"ok","deleted":<n>,"failed":<n>}`. Errors: `HW_ERROR`, `INVALID_VALUE` (cannot delete directory), etc.

---

## Message Types and Codes

**CLI command replies** are JSON lines with a `status` field (`ok` or `error`). **Telemetry** lines (logs, config broadcast, health, short status) use a `ty` (type) field. In AP mode when WiFi is not configured, some log output may be plain text.

### Message Type: `log`
**Code**: `ty: "log"`

**Format**:
```json
{
  "ty": "log",
  "lv": "info"|"warn"|"error"|"fatal"|"debug"|"event",
  "ms": "<message_text>",
  "mc": "<device_id>",
  "si": "<session_id>"
}
```

**Levels**:
- `info`: Informational messages
- `warn`: Warning messages
- `error`: Error messages
- `fatal`: Fatal error messages
- `debug`: Debug messages
- `event`: Event-style messages (state transitions)

**Example**:
```json
{"ty":"log","lv":"info","ms":"INIT - Reset reason: Power On (1)","rr":"Power On","mc":"AA:BB:CC:DD:EE:FF","si":"abc123"}
```

---

### Message Type: `config`
**Code**: `ty: "config"`

**Format**:
```json
{
  "ty": "config",
  "fw": "<firmware_version>",
  "ss": "<wifi_ssid>"|"",
  "sie": true|false,
  "rte": true|false,
  "usc": true|false,
  "rsc": true|false,
  "oh": <timezone_offset_hours>,
  "wtp": <wifi_tx_power>,
  "ath": <audio_threshold>,
  "mi": <min_recording_ms>,
  "mx": <max_recording_ms>,
  "sth": <silence_threshold_ms>,
  "pr": <pre_record_ms>,
  "gn": <codec_gain_db>,
  "is": <sample_rate>,
  "mc": "<device_id>",
  "si": "<session_id>"
}
```

**When sent**:
- On startup (if WiFi credentials configured)
- After settings changes (5 second debounce)
- Periodically every 15 minutes
- In response to `config ?` command

---

### Message Type: `health`
**Code**: `ty: "health"`

**Format**:
```json
{
  "ty": "health",
  "st": "SD",
  "sd": true|false,
  "rc": <recording_count>,
  "uc": <uploaded_count>,
  "pq": <pending_queue_count>,
  "td": <total_duration_seconds>,
  "ht": "<heap_total>"|"<size>K"|"<size>M",
  "hf": "<heap_free>"|"<size>K"|"<size>M",
  "tm": true|false,
  "wi": true|false,
  "ip": "<ip_address>"|"",
  "ri": <rssi>|0,
  "qe": <queue_count>,
  "ut": <uptime_seconds>,
  "yr": <year>|null,
  "yf": <total_files>|null,
  "ys": "<size>"|null,
  "yh": <total_hours>|null,
  "ym": <months_with_recordings>|null,
  "yd": <days_with_recordings>|null,
  "am": <api_min_response_ms>|null,
  "ax": <api_max_response_ms>|null,
  "aa": <api_avg_response_ms>|null,
  "mc": "<device_id>",
  "si": "<session_id>"
}
```

**When sent**:
- In response to `health ?` command

---

### Message Type: `short`
**Code**: `ty: "short"`

**Format**:
```json
{
  "ty": "short",
  "rg": true|false,
  "ug": true|false,
  "cd": <current_db>,
  "mi": <min_db>,
  "mx": <max_db>,
  "mc": "<device_id>",
  "si": "<session_id>"
}
```

**When sent**:
- Periodically every 30 seconds (if WiFi credentials configured)

---

## Settings Parameter Codes

### Top-Level Keys

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `fw` | firmware | string | Firmware version (read-only) |
| `cv` | configVersion | integer | Configuration schema version (read-only) |
| `w` | wifi | array | WiFi credential array (max 3 entries) |
| `a` | audio | object | Audio settings |
| `u` | upload | object | Upload settings |
| `r` | rtc | object | External RTC settings |
| `s` | sdCard | object | SD card settings |
| `t` | timezone | object | Timezone and maintenance settings |
| `l` | log | object | Logging configuration |
| `c` | cli | object | CLI settings (reserved, unused) |
| `wtp` | wifiTxPower | integer | WiFi transmit power (1-10) |
| `wse` | webserverEnabled | boolean | Enable built-in HTTP server (port 80) |
| `mac` | mac | string | Device ID / MAC address (read-only) |
| `cws` | connectedWiFiSsid | string | Connected WiFi SSID (read-only) |
| `cwi` | connectedWiFiIndex | integer | Connected WiFi index (read-only) |
| `rt` | runtime | object | Runtime metadata (read-only) |
| `cip` | currentIp | object | Current IP configuration (read-only) |

---

### WiFi Credential Fields (`w` array)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `ss` | ssid | string | WiFi network name |
| `pw` | password | string | WiFi password (WPA/WPA2 PSK) |
| `ctm` | connectTimeoutMs | integer | Connection timeout in milliseconds |
| `sie` | staticIpEnabled | boolean | Enable static IP configuration |
| `sip` | staticIp | string | Static IPv4 address |
| `ssn` | staticSubnet | string | Subnet mask |
| `sgt` | staticGateway | string | Default gateway |
| `sd1` | staticDns1 | string | Primary DNS server |
| `sd2` | staticDns2 | string | Secondary DNS server |

**CLI Shortcuts**:
- `ssid0`, `ssid1`, `ssid2` → `wifi[0].ssid`, `wifi[1].ssid`, `wifi[2].ssid`
- `pass0`, `pass1`, `pass2` → `wifi[0].password`, `wifi[1].password`, `wifi[2].password`
- `static0`, `static1`, `static2` → `wifi[0].staticIpEnabled`, etc.
- `ip0`, `ip1`, `ip2` → `wifi[0].staticIp`, etc.
- `subnet0`, `subnet1`, `subnet2` → `wifi[0].staticSubnet`, etc.
- `gateway0`, `gateway1`, `gateway2` → `wifi[0].staticGateway`, etc.
- `dns10`, `dns11`, `dns12` → `wifi[0].staticDns1`, etc.
- `dns20`, `dns21`, `dns22` → `wifi[0].staticDns2`, etc.

---

### Audio Settings (`a` object)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `sr` | sampleRate | integer | Audio sampling rate (Hz), currently only 8000 |
| `bs` | bufferSamples | integer | Internal audio buffer size (512-4096 samples) |
| `ath` | audioThreshold | integer | Amplitude threshold (0-255) |
| `prm` | preRecordMs | integer | Pre-record window (0-500 ms) |
| `mrm` | minRecordingMs | integer | Minimum recording duration (ms) |
| `xrm` | maxRecordingMs | integer | Maximum recording duration (ms) |
| `stm` | silenceThresholdMs | integer | Silence duration before auto-stop (ms) |
| `dsf` | discardSmallFilesEnabled | boolean | Enable discarding small files |
| `dmm` | discardSmallFilesMinMs | integer | Minimum duration for small file discard (ms) |
| `cg` | codecGain | integer | Codec input gain (-3, 0, 3, 6, 9, 12, 15, 18, 21, 24 dB) |

**CLI Shortcuts**:
- `min` → `audio.minRecordingMs`
- `max` → `audio.maxRecordingMs`
- `pre` / `preroll` / `prerecord` → `audio.preRecordMs`
- `silence` → `audio.silenceThresholdMs`
- `gain` → `audio.codecGain`
- `athr` → `audio.audioThreshold`

---

### Upload Settings (`u` object)

Four endpoints (`kApiEndpointCount` = 4): **0** Ohio, **1** Oregon, **2** Virginia, **3** Custom.

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `qd` | queueDepth | integer | Upload queue depth (4-32, legacy) |
| `ctm` | convertToMp3 | boolean | Convert to MP3 before upload (legacy) |
| `ah` | apiHosts | array | API hostnames/IPs (indices 0–3) |
| `ap` | apiPorts | array | API TCP ports (indices 0–3) |
| `en` | enabled | array | Per-endpoint enable flags (indices 0–3) |
| `uch` | useCustomHost | boolean | Legacy: maps to `upload.enabled[3]` |
| `ch` | customHost | string | Legacy: maps to `upload.apiHosts[3]` |
| `cp` | customPort | integer | Legacy: maps to `upload.apiPorts[3]` |

**CLI Shortcuts**:
- `host0` … `host3` → `upload.apiHosts[0]` … `[3]`
- `region0` / `ohio`, `region1` / `oregon`, `region2` / `virginia`, `region3` / `custom` → `upload.enabled[0]` … `[3]`
- `customhost`, `customport`, `usecustomhost` → Custom host fields / `upload.useCustomHost`

---

### RTC Settings (`r` object)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `en` | enabled | boolean | Enable external RTC |
| `sda` | sdaPin | integer | I²C SDA pin number |
| `scl` | sclPin | integer | I²C SCL pin number |

---

### SD Card Settings (`s` object)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `usc` | useSdCard | boolean | Enable SD card usage |
| `rsc` | recordToSdCard | boolean | Record audio to SD card |
| `m1b` | mode1bit | boolean | Use 1-bit SD bus mode |
| `frq` | frequency | integer | SD card bus clock frequency (1-20 MHz) |
| `fmf` | formatIfMountFailed | boolean | Format SD card if mount fails |

---

### Timezone Settings (`t` object)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `oh` | offsetHours | integer | Timezone offset from UTC (-12 to +14 hours) |
| `mh` | maintenanceHour | integer | Maintenance hour (0-23) |
| `mm` | maintenanceMinute | integer | Maintenance minute (0-59) |

---

### Log Settings (`l` object)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `sf` | serialFatal | boolean | Enable fatal messages on serial |
| `se` | serialError | boolean | Enable error messages on serial |
| `sw` | serialWarning | boolean | Enable warning messages on serial |
| `si` | serialInfo | boolean | Enable info messages on serial |
| `sd` | serialDebug | boolean | Enable debug messages on serial |
| `sev` | serialEvent | boolean | Enable event messages on serial |
| `ff` | fileFatal | boolean | Enable fatal messages in files |
| `fe` | fileError | boolean | Enable error messages in files |
| `fw` | fileWarning | boolean | Enable warning messages in files |
| `fi` | fileInfo | boolean | Enable info messages in files |
| `fd` | fileDebug | boolean | Enable debug messages in files |
| `fev` | fileEvent | boolean | Enable event messages in files |

---

### Runtime Metadata (`rt` object, read-only)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `mac` | mac | string | Device ID / MAC address |
| `wc` | wifiConnected | boolean | WiFi connection status |
| `css` | connectedSsid | string | Connected WiFi SSID |
| `rss` | rssi | integer | Current RSSI (dBm) |
| `ci` | connectedIndex | integer | WiFi credentials array index |

---

### Current IP Block (`cip` object, read-only)

| Code | Full Name | Type | Description |
|------|-----------|------|-------------|
| `ip` | ip | string | Current IPv4 address |
| `sub` | subnet | string | Current subnet mask |
| `gw` | gateway | string | Current default gateway |
| `d1` | dns1 | string | Primary DNS server |
| `d2` | dns2 | string | Secondary DNS server |

---

### Config Message Field Codes

In `config` type messages, these codes are used:

| Code | Full Name | Maps To |
|------|-----------|---------|
| `ss` | ssid | `wifi[0].ssid` |
| `sie` | staticIpEnabled | `wifi[0].staticIpEnabled` |
| `rte` | rtcEnabled | `rtc.enabled` |
| `usc` | useSdCard | `sdCard.useSdCard` |
| `rsc` | recordToSdCard | `sdCard.recordToSdCard` |
| `oh` | offsetHours | `timezone.offsetHours` |
| `wtp` | wifiTxPower | `wifiTxPower` |
| `ath` | audioThreshold | `audio.audioThreshold` |
| `mi` | min | `audio.minRecordingMs` |
| `mx` | max | `audio.maxRecordingMs` |
| `sth` | silenceThreshold | `audio.silenceThresholdMs` |
| `pr` | preRecord | `audio.preRecordMs` |
| `gn` | gain | `audio.codecGainDb` |
| `is` | sampleRate | `audio.sampleRate` |

---

### Health Message Field Codes

In `health` type messages, these codes are used:

| Code | Full Name | Description |
|------|-----------|-------------|
| `st` | storage | Storage type ("SD") |
| `sd` | sdCard | SD card available |
| `rc` | recordingCount | Audio files recorded this session |
| `uc` | uploadedCount | Audio files uploaded this session |
| `pq` | pendingQueue | Files in queue pending upload |
| `td` | totalDuration | Total duration recorded (seconds) |
| `ht` | heapTotal | Total heap size (formatted string) |
| `hf` | heapFree | Free heap size (formatted string) |
| `tm` | timeValid | Time is valid |
| `wi` | wifi | WiFi connected |
| `ip` | ip | IP address |
| `ri` | rssi | RSSI value |
| `qe` | queue | Upload queue count |
| `ut` | uptime | Uptime in seconds |
| `yr` | year | Year for yearly summary |
| `yf` | yearlyFiles | Total files on SD |
| `ys` | yearlySize | Total size (formatted string) |
| `yh` | yearlyHours | Total hours |
| `ym` | yearlyMonths | Months with recordings |
| `yd` | yearlyDays | Days with recordings |
| `am` | apiMin | API minimum response time (ms) |
| `ax` | apiMax | API maximum response time (ms) |
| `aa` | apiAvg | API average response time (ms) |

---

### Short Message Field Codes

In `short` type messages, these codes are used:

| Code | Full Name | Description |
|------|-----------|-------------|
| `rg` | recording | Currently recording |
| `ug` | uploading | Currently uploading |
| `cd` | currentDb | Current audio level (dB) |
| `mi` | minDb | Minimum audio level (dB) |
| `mx` | maxDb | Maximum audio level (dB) |

---

## Common Field Codes (All Message Types)

| Code | Full Name | Description |
|------|-----------|-------------|
| `ty` | type | Message type: "log", "config", "health", "short" |
| `lv` | level | Log level: "info", "warn", "error", "fatal", "debug", "event" |
| `ms` | message | Message text |
| `mc` | mac | Device ID / MAC address |
| `si` | sessionId | Session identifier |
| `rr` | resetReason | Reset reason (in INIT log message) |

---

## Notes

1. **Case sensitivity**: CLI **commands** are normalized to uppercase for dispatch; `show`/`get`/`set` parameter names use lowercase matching in `settings_getParam` / `settings_setParam`.

2. **JSON Format**: CLI responses and periodic telemetry are single-line JSON. Pretty printing is not used.

3. **AP Mode**: When WiFi credentials are not configured (AP mode), some log output may be plain text.
4. **Mutex Protection**: Serial output is protected by a mutex to prevent interleaving of CLI responses and periodic messages.

5. **Session ID**: Log/config/health/short telemetry include a session ID (`si`) that changes on each device boot.

6. **Device ID**: Telemetry includes a device ID (`mc`) derived from the MAC address.

7. **Parameter Aliases**: Many settings have CLI shortcuts (e.g., `min` → `audio.minRecordingMs`). See the shortcuts tables and `Help` output.

8. **Array Indices**: WiFi credentials use indices `0`–`2`. Upload endpoints use `0`–`3` (Ohio, Oregon, Virginia, Custom).

9. **Read-Only Fields**: Some fields (like `fw`, `cv`, `mac`, `rt`, `cip`) are read-only and cannot be set via CLI commands.

10. **Validation**: Settings are validated before being saved. Invalid values may be clamped to valid ranges or rejected with a JSON error line.

