# Release: KC-Tango-2026-03-10-Build-and-optimization

## Summary

This release adds a **periodic heartbeat** so the device reports that it is online to the API server about once per minute when connected and not busy with an upload, and **randomizes API host selection** so the device no longer always tries the first endpoint first—spreading load and avoiding a single point of failure when the first host is down. It also **allows uploads when time is invalid**: audio and log uploads (and settings push) are no longer blocked by unsynced NTP; the clock is synced from event responses (e.g. heartbeat), and uploads use a best-effort or fallback timestamp when needed.

---

## Changes

### Allow uploads when time is invalid

- **Summary:** Uploads are no longer gated on `timeKeeper().timeIsValid()`. If NTP never syncs, the device can still upload recordings and logs; the server time from event responses (e.g. heartbeat, online) is used to sync the clock, and when epoch is invalid the existing fallback timestamp format is used.
- **Location:** `src/network.cpp`
- **Changes:**
  - **Sync clock from event responses:** When a successful event HTTP response (status 200) has a valid JSON body, `syncClockFromApiResponse(body)` is called. The server’s `timestamp` or `current_time` in the response (e.g. from heartbeat or online events) sets the device clock, so time often becomes valid after a few heartbeats.
  - **Remove time-invalid gate from `uploadAudioFile`:** The early return when `!timeKeeper().timeIsValid()` was removed. Uploads run whenever WiFi is connected; `formatIsoTimestamp(request.recordedAtEpoch, request.recordedAtMs)` already returns ISO when valid and `ms-<recordedAtMs>` when epoch is 0.
  - **Remove time-invalid gate from `uploadLogFile`:** Same check removed so log uploads are not blocked by invalid time.
  - **Remove time-invalid gate from `network_pushSettingsToServer`:** Same check removed so settings push is not blocked.
- **Doc:** `docs/Stability.md` — item 6 marked fixed with the above fix description.

### Periodic heartbeat in network thread

- **Location:** `src/network.cpp` — `network_loop()`
- **Behavior:** When WiFi is connected, the device sends a `heartbeat` event to `/api/v1/events` approximately every 60 seconds.
- **“Not busy” check:** The heartbeat is sent only when the upload mutex is free (no audio upload in progress). If the network layer is busy, the heartbeat is skipped that cycle and tried again on the next `network_loop()` iteration (no blocking).
- **Guards:**
  - Runs only in the “WiFi connected” path of `network_loop()`.
  - Waits 3 seconds after the first time WiFi is seen connected before sending the first heartbeat (and after reconnect).
  - Uses a non-blocking check: try to take the upload mutex with timeout 0; if taken, release immediately and only then send the event.

**Implementation details:**

- File-scope `s_uploadMutexForHeartbeat` is set in `ensureUploadResources()` so `network_loop()` can check “not busy” without touching the anonymous-namespace upload mutex.
- New helper `isUploadMutexFree()` (file scope): tries `xSemaphoreTake(..., 0)`, releases if taken, returns true only when the mutex was free.
- Static state in `network_loop()`: `lastHeartbeatMs`, `firstConnectedInElseMs`; `firstConnectedInElseMs` is reset when WiFi is disconnected so the 3s delay applies again after reconnect.
- Event sent: `sendEvent("heartbeat", "{\"message\":\"Device online\"}");` — same event pipeline as other events (queued, then POSTed to `/api/v1/events` by the event task).

### Randomize API host selection

- **Location:** `src/network.cpp`
- **Behavior:** Audio upload, event sending, and log upload no longer try API endpoints in fixed order (0, 1, 2). They now try healthy endpoints in a **random order** each time, while still flagging dead hosts and failing over to the next.
- **Helper:** New `getHealthyEndpointIndicesRandomOrder(outIndices, maxCount, outCount)` in the anonymous namespace. It builds the list of healthy indices (enabled, not dead, non-empty host, port ≠ 0), then applies a Fisher–Yates shuffle and fills the caller’s array. Circuit breaker is not considered in the helper; the audio-upload loop still skips circuit-broken endpoints when iterating.
- **Loops updated:**
  - **Audio upload** (~line 1037): Before the endpoint loop, call the helper to get a random order; loop `for (i = 0; i < orderCount; ++i) { idx = order[i]; ... }`. Circuit breaker and success/failure handling unchanged.
  - **Event send** (event task, ~line 2429): Same pattern—random order array, then iterate by `eventOrder[ei]`.
  - **Log upload** (~line 1623): Same pattern—random order array, then iterate by `logOrder[li]`.
- **Unchanged:** Dead-endpoint logic (mark dead after 5 failures, skip dead, reset on WiFi reconnect). Firmware check already used `network_getRandomHealthyEndpoint()` and was not changed.

### Documentation

- **`docs/API_EVENTS_ANALYSIS.md`:** New section **2. `heartbeat` – Periodic Device Online Ping** (trigger, payload, location). Subsequent event sections renumbered (3–13).

---

## Files modified

| File | Change |
|------|--------|
| `src/network.cpp` | Heartbeat logic in `network_loop()`, `isUploadMutexFree()`, `s_uploadMutexForHeartbeat`, and mutex update in `ensureUploadResources()`. **API host randomization:** `getHealthyEndpointIndicesRandomOrder()` helper; audio-upload, event-send, and log-upload loops now iterate over a random permutation of healthy endpoints. **Allow uploads when time invalid:** `syncClockFromApiResponse(body)` called on successful event response with body; removed `timeIsValid()` checks from `uploadAudioFile`, `uploadLogFile`, and `network_pushSettingsToServer`. |
| `docs/API_EVENTS_ANALYSIS.md` | New `heartbeat` event section; renumbered following event sections. |
| `docs/Stability.md` | Item 6 (Revisit upload when time is not valid) marked fixed; added fix-applied note. |

---

## API impact

- **New event type:** `heartbeat`
- **Endpoint:** `POST /api/v1/events` (unchanged)
- **Payload:** `event_type: "heartbeat"`, `event_data: { "message": "Device online" }`
- **Frequency:** About once per minute when online and not uploading; may be delayed if the device is busy with uploads.
