## Settings JSON Reference

This document describes the full JSON structure used by the Boondock firmware for configuration and runtime metadata.

Unless otherwise noted:
- **All top-level settings** (except the `runtime` and `currentIp` sections) are **persisted in NVS** (including `wse` / `webserverEnabled`).
- The device accepts both **short keys** (space‑optimized) and legacy **long keys** for backward compatibility, but it **always writes short keys** when saving.

Example (simplified) top-level structure:

```json
{
  "fw": "1.0.0",
  "cv": 1,
  "w": [ ... ],
  "a": { ... },
  "u": { ... },
  "r": { ... },
  "s": { ... },
  "t": { ... },
  "l": { ... },
  "wtp": 8,
  "wse": true,
  "rt": { ... },
  "mac": "AA:BB:CC:DD:EE:FF",
  "cws": "MyWiFi",
  "cwi": 0,
  "cip": { ... }
}
```

Where:
- **`fw`**: firmware version string (from `FIRMWARE`).
- **`cv`**: configuration schema version (from `CONFIG_VERSION`).

The remaining sections are described below.

---

## Top-Level Keys

- **`fw` (firmware)**  
  - **Type**: string  
  - **Direction**: read‑only (set by firmware)  
  - **Description**: Identifies the running firmware version. Useful for diagnostics and matching server‑side expectations.

- **`cv` (configVersion)**  
  - **Type**: integer  
  - **Direction**: read‑only (set by firmware)  
  - **Description**: Schema version for the configuration JSON. Used internally for backward‑compatible migrations.

- **`w` / `wifi` – WiFi credential array**  
  - **Short key**: `w`  
  - **Legacy key**: `wifi`  
  - **Type**: array of objects (max `kMaxWifiCredentials`, currently 3)  
  - **Description**: List of WiFi networks the device can connect to, tried in array order.
  - See [WiFi Credential Objects](#wifi-credential-objects) for fields.

- **`a` / `audio` – Audio settings**  
  - **Short key**: `a`  
  - **Legacy key**: `audio`  
  - **Type**: object  
  - **Description**: Controls audio sampling and recording behavior.  
  - See [Audio Settings](#audio-settings) for fields.

- **`u` / `upload` – Upload settings**  
  - **Short key**: `u`  
  - **Legacy key**: `upload`  
  - **Type**: object  
  - **Description**: Controls audio upload behavior and API endpoints.  
  - See [Upload Settings](#upload-settings) for fields.

- **`r` / `rtc` – External RTC settings**  
  - **Short key**: `r`  
  - **Legacy key**: `rtc`  
  - **Type**: object  
  - **Description**: Enables and configures the hardware real‑time clock.  
  - See [RTC Settings](#rtc-settings) for fields.

- **`s` / `sdCard` – SD card settings**  
  - **Short key**: `s`  
  - **Legacy key**: `sdCard`  
  - **Type**: object  
  - **Description**: Controls if and how the SD card is used for recording.  
  - See [SD Card Settings](#sd-card-settings) for fields.

- **`t` / `timezone` – Timezone and maintenance**  
  - **Short key**: `t`  
  - **Legacy key**: `timezone`  
  - **Type**: object  
  - **Description**: Device timezone offset and daily maintenance window.  
  - See [Timezone Settings](#timezone-settings) for fields.

- **`l` / `log` – Logging configuration**  
  - **Short key**: `l`  
  - **Legacy key**: `log`  
  - **Type**: object  
  - **Description**: Per‑channel log level enables for serial and file logging.  
  - See [Log Settings](#log-settings) for fields.

- **`c` / `cli` – CLI settings**  
  - **Short key**: `c`  
  - **Legacy key**: `cli`  
  - **Status**: reserved / currently unused in `settings.cpp`  
  - **Description**: Reserved key for potential future CLI‑specific settings. Not currently written or parsed.

- **`wse` / `webserverEnabled` – Built‑in web UI**  
  - **Short key**: `wse`  
  - **Legacy key**: `webserverEnabled`  
  - **Type**: boolean  
  - **Description**: When true, the device runs the HTTP server on port 80 (SPA and APIs). Persisted in NVS.

- **`wtp` / `wifiTxPower` – WiFi transmit power**  
  - **Short key**: `wtp`  
  - **Legacy key**: `wifiTxPower`  
  - **Type**: integer  
  - **Valid range**: 1–10 (values outside this range are ignored)  
  - **Description**: Scales WiFi transmit power on a 1–10 scale (device‑specific mapping). Lower values reduce range and power usage; higher values increase range and power.

- **`hn` / `hostname` – Local hostname**
  - **Short key**: `hn`
  - **Legacy key**: `hostname`
  - **Type**: string
  - **Valid value**: 1–63 lowercase letters, numbers, or interior hyphens
  - **Default**: `boondock`
  - **Description**: DHCP and mDNS hostname. The device is reachable at `http://<hostname>.local` after rebooting.

- **`rt` / `runtime` – Runtime metadata**  
  - **Short key**: `rt`  
  - **Legacy key**: `runtime`  
  - **Type**: object  
  - **Direction**: read‑only (computed at serialization time)  
  - **Description**: Snapshot of current network/runtime status for UI consumption.  
  - See [Runtime Metadata](#runtime-metadata) for fields.

- **`mac` – Device ID / MAC address**  
  - **Short key**: `mac`  
  - **Type**: string  
  - **Direction**: read‑only (set by firmware using `getDeviceId()`)  
  - **Description**: Unique device identifier, typically derived from the MAC address. Exposed both at the top level and under `runtime.mac`.

- **`cws` – Connected WiFi SSID**  
  - **Short key**: `cws`  
  - **Type**: string, only present when connected  
  - **Direction**: read‑only  
  - **Description**: SSID of the currently connected WiFi network. Written alongside runtime metadata.

- **`cwi` – Connected WiFi index**  
  - **Short key**: `cwi`  
  - **Type**: integer, only present when connected  
  - **Direction**: read‑only  
  - **Description**: Index into the `w` array corresponding to the credential that matched the current WiFi connection, or `-1` if unknown.

- **`cip` / `currentIp` – Current IP info**  
  - **Short key**: `cip`  
  - **Legacy key**: `currentIp`  
  - **Type**: object  
  - **Direction**: read‑only; only present if an IP/subnet/gateway/DNS is available  
  - **Description**: Current IP configuration for the WiFi interface.  
  - See [Current IP Block](#current-ip-block) for fields.

---

## WiFi Credential Objects

Top-level key: **`w`** (short) or **`wifi`** (legacy).  
Type: array of objects, each describing one WiFi profile.

Each entry has:

- **`ss` / `ssid`**  
  - **Short key**: `ss`  
  - **Legacy key**: `ssid`  
  - **Type**: string  
  - **Description**: WiFi network name. Empty string means “unused” entry.

- **`pw` / `password`**  
  - **Short key**: `pw`  
  - **Legacy key**: `password`  
  - **Type**: string  
  - **Description**: WiFi password (WPA/WPA2 PSK). Treated as **sensitive**; some APIs may mask it as `"HIDDEN_FOR_SECURITY"`.

- **`ctm` / `connectTimeoutMs`**  
  - **Short key**: `ctm`  
  - **Legacy key**: `connectTimeoutMs`  
  - **Type**: unsigned integer (milliseconds)  
  - **Description**: Maximum time allowed for connecting to this network before giving up and trying the next entry.

- **`sie` / `staticIpEnabled`**  
  - **Short key**: `sie`  
  - **Legacy key**: `staticIpEnabled`  
  - **Type**: boolean  
  - **Description**: Enables use of a static IP configuration for this WiFi profile instead of DHCP.

- **`sip` / `staticIp`**  
  - **Short key**: `sip`  
  - **Legacy key**: `staticIp`  
  - **Type**: string (IPv4)  
  - **Description**: Static IPv4 address for the device when `staticIpEnabled` is true (e.g. `"192.168.1.50"`).

- **`ssn` / `staticSubnet`**  
  - **Short key**: `ssn`  
  - **Legacy key**: `staticSubnet`  
  - **Type**: string (IPv4 mask)  
  - **Description**: Subnet mask for the static address (e.g. `"255.255.255.0"`).

- **`sgt` / `staticGateway`**  
  - **Short key**: `sgt`  
  - **Legacy key**: `staticGateway`  
  - **Type**: string (IPv4)  
  - **Description**: Default gateway when using static IP.

- **`sd1` / `staticDns1`**  
  - **Short key**: `sd1`  
  - **Legacy key**: `staticDns1`  
  - **Type**: string (IPv4)  
  - **Description**: Primary DNS server for static IP mode.

- **`sd2` / `staticDns2`**  
  - **Short key**: `sd2`  
  - **Legacy key**: `staticDns2`  
  - **Type**: string (IPv4)  
  - **Description**: Secondary DNS server for static IP mode (optional).

---

## Audio Settings

Top-level key: **`a`** (short) or **`audio`** (legacy).  
Type: object.

- **`sr` / `sampleRate`**  
  - **Short key**: `sr`  
  - **Legacy key**: `sampleRate`  
  - **Type**: integer (Hz)  
  - **Valid values**: currently only `8000` is accepted; anything else snaps to the default `DEFAULT_AUDIO_SAMPLE_RATE` (in code).  
  - **Description**: Audio sampling rate.

- **`bs` / `bufferSamples`**  
  - **Short key**: `bs`  
  - **Legacy key**: `bufferSamples`  
  - **Type**: integer (samples)  
  - **Valid range**: 512–4096; values outside are clamped.  
  - **Description**: Size of internal audio buffer. Larger buffers improve robustness at the cost of latency and RAM.

- **`ath` / `audioThreshold`**  
  - **Short key**: `ath`  
  - **Legacy key**: `audioThreshold`  
  - **Type**: integer (0-100)  
  - **Description**: Amplitude threshold used to decide when audio is “loud enough” to trigger or continue recording (exact semantics depend on the recorder implementation).

- **`prm` / `preRecordMs`**  
  - **Short key**: `prm`  
  - **Legacy key**: `preRecordMs`  
  - **Type**: integer (milliseconds)  
  - **Valid range**: 0–500 ms; values are clamped.  
  - **Description**: Amount of audio kept before the trigger point, allowing recordings to include brief pre‑trigger context.

- **`mrm` / `minRecordingMs`**  
  - **Short key**: `mrm`  
  - **Legacy key**: `minRecordingMs`  
  - **Type**: integer (milliseconds)  
  - **Description**: Minimum recording duration; shorter captures may be extended or filtered out.

- **`xrm` / `maxRecordingMs`**  
  - **Short key**: `xrm`  
  - **Legacy key**: `maxRecordingMs`  
  - **Type**: integer (milliseconds)  
  - **Description**: Upper bound on recording length for a single clip.

- **`stm` / `silenceThresholdMs`**  
  - **Short key**: `stm`  
  - **Legacy key**: `silenceThresholdMs`  
  - **Type**: integer (milliseconds)  
  - **Description**: Duration of continuous silence before the recorder auto‑stops.

- **`dsf` / `discardSmallFilesEnabled`**  
  - **Short key**: `dsf`  
  - **Legacy key**: `discardSmallFilesEnabled`  
  - **Type**: boolean  
  - **Description**: If true, very short recordings (see `discardSmallFilesMinMs`) are discarded to reduce noise and storage usage.

- **`dmm` / `discardSmallFilesMinMs`**  
  - **Short key**: `dmm`  
  - **Legacy key**: `discardSmallFilesMinMs`  
  - **Type**: integer (milliseconds)  
  - **Description**: Minimum duration below which recordings are considered “too small” and may be discarded when `discardSmallFilesEnabled` is true.

- **`cg` / `codecGain`**  
  - **Short key**: `cg`  
  - **Legacy key**: `codecGain`  
  - **Type**: integer (dB)  
  - **Allowed values**: `-3, 0, 3, 6, 9, 12, 15, 18, 21, 24`; input is snapped to the closest value.  
  - **Description**: Input gain applied by the audio codec to adjust recording level.

---

## Upload Settings

Top-level key: **`u`** (short) or **`upload`** (legacy).  
Type: object.

The firmware defines **`kApiEndpointCount`** endpoints (currently **4**): index **0** = Ohio, **1** = Oregon, **2** = Virginia, **3** = Custom (user‑defined host/port). Defaults come from `DEFAULT_AUDIO_UPLOAD_HOSTS_*` in `config.h`.

- **`qd` / `queueDepth`**  
  - **Short key**: `qd`  
  - **Legacy key**: `queueDepth`  
  - **Type**: integer  
  - **Valid range**: 4–32; values are clamped.  
  - **Description**: Legacy parameter indicating desired upload queue depth. The modern filesystem‑based queue may not rely on this directly but it is preserved for compatibility.

- **`ctm` / `convertToMp3`**  
  - **Short key**: `ctm`  
  - **Legacy key**: `convertToMp3`  
  - **Type**: boolean  
  - **Description**: Legacy flag indicating whether to transcode recordings to MP3 before upload. Kept for backward compatibility.

- **`ah` / `apiHosts`**  
  - **Short key**: `ah`  
  - **Legacy key**: `apiHosts`  
  - **Type**: array of strings **or** object keyed by index (for legacy formats); length matches `kApiEndpointCount` (4 slots).  
  - **Description**: Hostnames or IPs of API endpoints used for uploads. Index **3** is the Custom region host.  
  - **Notes**:  
    - Parsed from either an array or an object like `{ "0": "host1", "1": "host2", ... }`.  
    - When hosts change, the firmware invalidates cached API endpoint state (`network_invalidateApiEndpoints()`).

- **`ap` / `apiPorts`**  
  - **Short key**: `ap`  
  - **Legacy key**: `apiPorts`  
  - **Type**: array of integers  
  - **Description**: Per‑endpoint TCP ports for API hosts (including Custom at index **3**).  
  - **Validation**: Port value must be non‑zero; entries with `0` are ignored with a warning.

- **`en` / `enabled`**  
  - **Short key**: `en`  
  - **Legacy key**: `enabled`  
  - **Type**: array of booleans (one per endpoint, length 4)  
  - **Description**: Per‑endpoint enable flags. Indices **0–2** are the regional endpoints; **3** enables the Custom host when true.  
  - **Backward compatibility**: If this array is missing, all endpoints default to `true`.

- **`uch` / `useCustomHost`** (legacy alias for Custom slot)  
  - **Short key**: `uch`  
  - **Legacy key**: `useCustomHost`  
  - **Type**: boolean  
  - **Description**: If present, maps to **`upload.enabled[3]`** (enable Custom upload). Prefer setting `en[3]` directly in new JSON.

- **`ch` / `customHost`**  
  - **Short key**: `ch`  
  - **Legacy key**: `customHost`  
  - **Type**: string  
  - **Description**: Maps to **`upload.apiHosts[3]`** (Custom region hostname or IP).

- **`cp` / `customPort`**  
  - **Short key**: `cp`  
  - **Legacy key**: `customPort`  
  - **Type**: integer  
  - **Description**: Maps to **`upload.apiPorts[3]`**.

Changing any of the above may cause the firmware to **reinitialize API endpoints** to pick up new settings.

---

## RTC Settings

Top-level key: **`r`** (short) or **`rtc`** (legacy).  
Type: object.

- **`en` / `enabled`**  
  - **Short key**: `en`  
  - **Legacy key**: `enabled`  
  - **Type**: boolean  
  - **Description**: If true, the firmware will use and maintain an external RTC.

- **`sda` / `sdaPin`**  
  - **Short key**: `sda`  
  - **Legacy key**: `sdaPin`  
  - **Type**: integer (pin number)  
  - **Description**: I²C SDA pin for the RTC module.

- **`scl` / `sclPin`**  
  - **Short key**: `scl`  
  - **Legacy key**: `sclPin`  
  - **Type**: integer (pin number)  
  - **Description**: I²C SCL pin for the RTC module.

---

## SD Card Settings

Top-level key: **`s`** (short) or **`sdCard`** (legacy).  
Type: object.

- **`usc` / `useSdCard`**  
  - **Short key**: `usc`  
  - **Legacy key**: `useSdCard`  
  - **Type**: boolean  
  - **Description**: Enables SD card usage in general (mounting, space checks, etc.).

- **`rsc` / `recordToSdCard`**  
  - **Short key**: `rsc`  
  - **Legacy key**: `recordToSdCard`  
  - **Type**: boolean  
  - **Description**: If true, audio recordings are written to the SD card.

- **`m1b` / `mode1bit`**  
  - **Short key**: `m1b`  
  - **Legacy key**: `mode1bit`  
  - **Type**: boolean  
  - **Description**: Selects 1‑bit SD bus mode when true, which may reduce pin count at the cost of throughput.

- **`frq` / `frequency`**  
  - **Short key**: `frq`  
  - **Legacy key**: `frequency`  
  - **Type**: integer (Hz)  
  - **Valid range**: 1,000,000 – 20,000,000 Hz (1–20 MHz); out‑of‑range values are ignored.  
  - **Description**: SD card bus clock frequency.

- **`fmf` / `formatIfMountFailed`**  
  - **Short key**: `fmf`  
  - **Legacy key**: `formatIfMountFailed`  
  - **Type**: boolean  
  - **Description**: If true, the device may attempt to format the SD card when mounting fails (use with caution).

---

## Timezone Settings

Top-level key: **`t`** (short) or **`timezone`** (legacy).  
Type: object.

- **`oh` / `offsetHours`**  
  - **Short key**: `oh`  
  - **Legacy key**: `offsetHours`  
  - **Type**: integer  
  - **Valid range**: −12 to +14; values are clamped.  
  - **Description**: Fixed timezone offset from UTC, in hours.

- **`mh` / `maintenanceHour`**  
  - **Short key**: `mh`  
  - **Legacy key**: `maintenanceHour`  
  - **Type**: integer  
  - **Valid range**: 0–23; values are clamped.  
  - **Description**: Hour of day (local time) when daily maintenance tasks may run.

- **`mm` / `maintenanceMinute`**  
  - **Short key**: `mm`  
  - **Legacy key**: `maintenanceMinute`  
  - **Type**: integer  
  - **Valid range**: 0–59; values are clamped.  
  - **Description**: Minute within the maintenance hour for scheduled maintenance.

---

## Log Settings

Top-level key: **`l`** (short) or **`log`** (legacy).  
Type: object.

Each field is a **boolean** enabling or disabling a specific log severity/channel, either for **serial** output or **file** logging.

- **`sf` / `serialFatal`** – Enable fatal‑level messages on serial.  
- **`se` / `serialError`** – Enable error‑level messages on serial.  
- **`sw` / `serialWarning`** – Enable warning‑level messages on serial.  
- **`si` / `serialInfo`** – Enable informational messages on serial.  
- **`sd` / `serialDebug`** – Enable debug‑level messages on serial.  
- **`sev` / `serialEvent`** – Enable event‑style messages on serial (e.g. state transitions).

- **`ff` / `fileFatal`** – Enable fatal‑level messages in log files.  
- **`fe` / `fileError`** – Enable error‑level messages in log files.  
- **`fw` / `fileWarning`** – Enable warning‑level messages in log files.  
- **`fi` / `fileInfo`** – Enable informational messages in log files.  
- **`fd` / `fileDebug`** – Enable debug‑level messages in log files.  
- **`fev` / `fileEvent`** – Enable event‑style messages in log files.

These flags allow fine‑grained control over verbosity and storage use.

---

## Runtime Metadata

Top-level key: **`rt`** (short) or **`runtime`** (legacy).  
Type: object.  
Direction: **read‑only** – this block is recomputed on each serialization and is **not** persisted.

- **`mac` – Runtime MAC / device ID**  
  - **Short key**: `mac`  
  - **Type**: string  
  - **Description**: Same device identifier as top-level `mac`, exposed inside `runtime` for convenience.

- **`wc` / `wifiConnected`**  
  - **Short key**: `wc`  
  - **Legacy key**: `wifiConnected`  
  - **Type**: boolean  
  - **Description**: True if the device is currently connected to a WiFi network.

- **`css` / `connectedSsid`**  
  - **Short key**: `css`  
  - **Legacy key**: `connectedSsid`  
  - **Type**: string (only when connected)  
  - **Description**: SSID of the currently connected WiFi network.

- **`rss` / `rssi`**  
  - **Short key**: `rss`  
  - **Legacy key**: `rssi`  
  - **Type**: integer (dBm)  
  - **Description**: Current RSSI of the active WiFi connection.

- **`ci` / `connectedIndex`**  
  - **Short key**: `ci`  
  - **Legacy key**: `connectedIndex`  
  - **Type**: integer  
  - **Description**: Index into the WiFi credentials array that matches the current connection, or `-1` if not found.

---

## Current IP Block

Embedded under: **`cip`** (short) or **`currentIp`** (legacy).  
Type: object.  
Direction: **read‑only**; only present when at least one field is available.

- **`ip`**  
  - **Type**: string (IPv4)  
  - **Description**: Current local IP address (e.g. `"192.168.1.42"`).

- **`sub` / `subnet`**  
  - **Short key**: `sub`  
  - **Legacy key**: `subnet`  
  - **Type**: string (IPv4 mask)  
  - **Description**: Current subnet mask.

- **`gw` / `gateway`**  
  - **Short key**: `gw`  
  - **Legacy key**: `gateway`  
  - **Type**: string (IPv4)  
  - **Description**: Current default gateway.

- **`d1` / `dns1`**  
  - **Short key**: `d1`  
  - **Legacy key**: `dns1`  
  - **Type**: string (IPv4)  
  - **Description**: Primary DNS server currently in use.

- **`d2` / `dns2`**  
  - **Short key**: `d2`  
  - **Legacy key**: `dns2`  
  - **Type**: string (IPv4)  
  - **Description**: Secondary DNS server currently in use (if any).

---

## Notes on JSON Updates

- The firmware’s **`settings_updateAllFromJson`** and **`settings_applyJsonFromServer`** functions accept a full JSON document in any mix of **short** and **legacy** keys.
- Unknown fields are ignored; missing fields keep their current/default values.
- Some fields (WiFi passwords, etc.) are treated as **sensitive** and may be masked as `"HIDDEN_FOR_SECURITY"` when JSON is sent to external systems. When applying JSON from the server, masked values are typically ignored to avoid erasing the real secret stored locally.

