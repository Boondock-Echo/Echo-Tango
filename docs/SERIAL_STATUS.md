# Serial telemetry (automated messages)

This document describes **JSON lines printed to UART automatically** by the firmware: boot, periodic status, recording/upload activity, and application logging. It does **not** cover interactive [CLI](CLI.md) command responses (those use `{"status":"ok"}` / `{"status":"error",...}`).

- **Baud rate**: 115200 (see `Serial.begin` in `main.cpp`).
- **Format**: One JSON object per line; messages are written atomically where possible (mutex + `Serial.write` + flush).
- **Common fields** (when present):
  - **`tm`**: Local time string from `getFormattedTimeWithTimezone()` (not UTC unless configured that way).
  - **`mc`**: Device ID (MAC-style string from `getDeviceId()`).
  - **`si`**: Session ID (`getSessionId()`), new each boot.

Non-JSON lines (ROM bootloader, plain-text in AP mode) are described under [Non-JSON output](#non-json-output).

---

## When messages are sent (summary)

| Source | Condition | Interval / trigger |
|--------|-----------|-------------------|
| Boot INIT | Always after logger init | Once per boot |
| Device ready | End of `setup()` | Once per boot |
| `ty: "config"` | WiFi credentials configured (`network_hasAnyWiFiCredentials()`) | Once shortly after boot, again **5 s** after a settings change (debounced), then **every 60 s** (`ONE_MINUTE_MS` in `config.h`) |
| `ty: "short"` | WiFi credentials configured | **Every 30 s** (`THIRTY_SECONDS_MS`) |
| `ty: "health"` | Maintenance task | First time **≥ 30 s** after boot, then **every 60 s** |
| `ty: "event"` | Recording / upload lifecycle | On record start/stop and successful upload |
| Level-based logs | WiFi credentials configured | Whenever the logger emits a completed line (info/warning/error/…) |

If **no WiFi credentials** are stored, periodic **`config`** and **`short`** messages are **not** sent; the logger prints **plain text** instead of JSON for log lines (see below).

---

## Message shapes by `ty`

### `ty: "log"` — boot INIT only

Emitted in `setup()` after reset-reason logging. Uses the structured log shape with **`lv`** (level string).

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | Always `"log"` |
| `lv` | `"info"` |
| `ms` | Text including reset reason, e.g. `INIT - Reset reason: Software Reset (4)` |
| `rr` | Short reset reason string (e.g. `"Software Reset"`) |
| `mc`, `si` | Device and session ID |

---

### `ty: "ready"` — setup complete

Emitted once at the end of `setup()`.

| Field | Type | Meaning |
|-------|------|---------|
| `ty` | string | `"ready"` |
| `tm` | string | Time string |
| `ms` | string | `"Device setup complete"` |
| `dv` | bool | Device valid (always `true` here) |
| `nw` | bool | WiFi STA connected |
| `ip` | string | LAN IP or `""` if not connected |
| `ba` | bool | Backend/API verified (`false` until first successful request) |
| `ti` | bool | Time considered valid (`timeKeeper().timeIsValid()`) |
| `re` | bool | Record task created |
| `sd` | bool | SD card storage ready (`ensureStorage()` && SD mode) |
| `mc`, `si` | string | Device and session ID |

---

### `ty: "config"` — two lines per broadcast

`sendConfigMessage()` prints **two separate JSON lines** on the serial task (see `main.cpp`).

**Line 1 — recorder / audio**

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"config"` |
| `ath` | Audio threshold |
| `mrm` | Min recording (ms) |
| `xrm` | Max recording (ms) |
| `stm` | Silence threshold (ms) |
| `prm` | Pre-record (ms) |
| `cg` | Codec gain (dB) |
| `is` | Sample rate (Hz) |
| `rsc` | Record to SD card |
| `mc`, `si` | Device and session ID |

**Line 2 — general / system**

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"config"` |
| `fw` | Firmware string (`FIRMWARE`) |
| `ss` | WiFi SSID for slot 0 (or `""`) |
| `sie` | WiFi static IP enabled (slot 0) |
| `rte` | RTC enabled |
| `usc` | Use SD card |
| `oh` | Timezone offset hours |
| `wtp` | WiFi TX power (1–10) |
| `mc`, `si` | Device and session ID |

---

### `ty: "health"` — two lines per broadcast

`sendHealthMessage()` prints **two JSON lines** (maintenance task in `health.cpp`).

**Line 1 — system**

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"health"` |
| `st` | Storage label (`"SD"`) |
| `sd` | SD storage mode active |
| `ht`, `hf` | Heap total / free as human-readable strings (e.g. `123.45K`) |
| `tv` | Time valid |
| `wi` | WiFi connected |
| `cp` | Cloud path OK (recent TCP success to API) |
| `ip` | IP or `""` |
| `ri` | RSSI or `0` |
| `ut` | Uptime seconds |
| `mc`, `si` | Device and session ID |

**Line 2 — recording / uploads / optional yearly stats**

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"health"` |
| `rc` | Session recording count |
| `uc` | Session uploaded count |
| `pq` | Pending upload queue size |
| `td` | Total recorded duration (seconds, integer) |
| `am`, `ax`, `aa` | API min / max / avg response time (ms) |
| `yr`, `yf`, `ys`, `yh`, `ym`, `yd` | Present only if yearly summary was loaded: year, file count, size string, hours, months-with-data, days-with-data |
| `mc`, `si` | Device and session ID |

---

### `ty: "short"` — periodic compact audio status

Sent from `serialTask` when WiFi credentials exist, **every 30 seconds**.

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"short"` |
| `rg` | Recording active |
| `ug` | Upload in progress |
| `cd` | Current level (dB) |
| `mi` | Min dB (session/window stats from recorder) |
| `mx` | Max dB |
| `mc`, `si` | Device and session ID |

---

### `ty: "event"` — recording and upload (terminal JSON)

These are **event** lines for human/tools on serial (in addition to internal `sendEvent` usage).

**`ev: "record_begin"`** (recording started)

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"event"` |
| `ms` | `"[Record] 🟢 Recording started"` |
| `ev` | `"record_begin"` |
| `path` | Current recording path |
| `ts` | ISO timestamp |
| `sr` | Sample rate |
| `mc`, `si` | Device and session ID |

**`ev: "record_end"`** (recording stopped)

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"event"` |
| `ms` | `"[Record] 🔴 Recording stopped"` |
| `ev` | `"record_end"` |
| `path` | Final path |
| `ts` | ISO timestamp (if valid epoch) |
| `dur` | Duration (ms) |
| `sr` | Sample rate |
| `db` | Peak dB (omitted if not above −120 dB) |
| `reason` | Stop reason string (e.g. silence) |
| `mc`, `si` | Device and session ID |

**`ev: "audio_upload_success"`** (file uploaded)

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | `"event"` |
| `ms` | `"[Upload] ✅ Audio file successfully uploaded"` |
| `ev` | `"audio_upload_success"` |
| `file` | File name |
| `size` | Size (bytes) |
| `dur` | Audio duration (ms) from request |
| `db` | Peak dB (omitted if not above −120 dB) |
| `speed` | Relative speed factor (`duration_ms / elapsed_ms`) |
| `elapsed` | Upload elapsed time (ms) |
| `mc`, `si` | Device and session ID |

---

### Level-based application logs (`ty` = severity name)

When WiFi credentials **are** configured, completed log lines are sent as JSON built in `sendJsonLogMessage()` (`logger.cpp`). The field **`ty`** is **not** `"log"` here; it is the **severity**:

`fatal` | `error` | `warning` | `info` | `debug` | `event`

| Field | Meaning |
|-------|---------|
| `tm` | Time string |
| `ty` | One of the severities above |
| `ms` | Message text (emoji may be prefixed; quotes/backslashes escaped) |
| `mc`, `si` | Device and session ID |

The special boot **`ty: "log"`** INIT line is separate (see above).

---

## Non-JSON output

- **Bootloader / ROM**: Lines such as `entry 0x4008xxxx` come from the ESP-IDF/ROM bootloader, not application JSON.
- **AP / setup mode** (no WiFi credentials): the logger prints **plain text** lines (with emoji prefixes) instead of JSON (`logger.cpp` — `serialWriteWithTimestamp`).
- **CLI**: User-typed commands still produce JSON responses as documented in [CLI.md](CLI.md); those are not “automated status” but may appear interleaved on the same port.

---

## Implementation references

| Behavior | Primary source |
|----------|----------------|
| INIT JSON | `main.cpp` (`setup`, init document) |
| Ready | `main.cpp` (end of `setup`) |
| `config` / `short` timing | `main.cpp` `serialTask`, `config.h` intervals |
| `health` timing | `health.cpp` maintenance task |
| Recording events | `recorder.cpp` |
| Upload success event | `network.cpp` |
| Level-based JSON logs | `logger.cpp` `sendJsonLogMessage` |
| Mutex + atomic write | `common.cpp` `serialWriteJsonAtomic`, `main.cpp` `outputJsonToSerial*` |

Intervals: `THIRTY_SECONDS_MS` (30 s), `ONE_MINUTE_MS` (60 s) in `src/config.h`.
