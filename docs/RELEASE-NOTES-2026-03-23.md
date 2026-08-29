# Release notes — TANGO / SPA (2026-03-23)

This document summarizes changes **since the prior baseline** associated with **Tango-AudioBuffer-Fix-2026-03-23** (PSRAM recording buffer alignment and max-duration behavior).

## Comparison baseline (Git)

| Item | Value |
|------|--------|
| **Reference release** | `Tango-AudioBuffer-Fix-2026-03-23` (GitHub naming; tag not present in this clone) |
| **Baseline commit** | `1f63028` — *Updated the Version* (follows audio-buffer fix `3f1f10c`) |
| **This release range** | `1f63028..HEAD` (commits `251aee2`, `230e2f4` on branch `KC-Add-Web-Audio-Playback`) |
| **Firmware string (current tree)** | `TANGO-2026-03-23-C` — see `src/config.h` (`FIRMWARE`) |

To reproduce the comparison locally:

```bash
git log --oneline 1f63028..HEAD
git diff --stat 1f63028..HEAD
```

---

## Highlights

### Recordings (web SPA)

- **Recordings browser**: Folder tree under `/recordings` with day-folder selection (`YYYY/MM/DD`).
- **Catalog source**: List data comes from per-day **`summary.json`** (JSONL), not full inbox scans for the table.
- **New API**: `GET /api/recordings/summary?path=…` streams raw **`summary.json`** as **`application/x-ndjson`** (chunked from SD; no JSON parse on device for the list).
- **Client-side pagination**: One fetch per day; paging and sorting happen in the browser for responsiveness.
- **In-memory cache**: Per-day summary cached in the SPA; **full page reload** clears the cache.
- **Refresh**: **Refresh** control reloads the current day’s summary (invalidates cache and refetches).
- **Sort order**: **Newest recordings first** (by UTC time derived from filenames / summary fields).
- **Epoch folder handling**: Ignores bogus **`1970`** day trees where applicable (tree + list behavior).

### Header clock (device time display)

- Home/header clock uses **`deviceUtcEpoch`** from **`/api/home/summary`** (and WebSocket home updates): **Unix time in UTC**.
- **Large time**: Shown in the **browser’s local timezone** (`toLocaleTimeString`), **not** using the device **timezone offset** setting for display.
- **Subline**: Replaces the old offset label (e.g. `UTC-6 (GMT-6)`) with **`UTC (HH:MM:SS AM/PM)`** in smaller text (fixed `UTC` calendar time via `timeZone: 'UTC'`).
- **Polling**: Minimal **`changed: false`** responses still include **`deviceUtcEpoch`** so the clock stays aligned on every poll.

### Firmware / runtime (included in this range)

- Supporting changes for recordings paths, inbox/summary helpers, SD listing robustness, logging, upload queue, recorder, and health—see `git diff 1f63028..HEAD` for file-level detail.

### Diagnostics

- Removed temporary **debug ingest** calls from the SPA and stripped **ad-hoc network debug** blocks that were added for investigation (TCP connect / event-loop logging).

---

## API changes (summary)

| Endpoint / field | Change |
|------------------|--------|
| `GET /api/recordings/summary?path=/recordings/YYYY/MM/DD` | **New** — raw JSONL body for the day’s `summary.json`. |
| `GET /api/recordings/list?…` | Retained for compatibility; SPA list uses **`/summary`** + client processing. |
| `GET /api/home/summary` | Adds **`deviceUtcEpoch`**; home payload no longer relies on **`deviceTime` / `deviceTimeFormatted` / `timezoneDisplay`** for the header (removed from home summary JSON). **`changed: false`** responses include **`deviceUtcEpoch`**. |
| WebSocket `home` pushes | Same **`deviceUtcEpoch`** semantics as HTTP summary. |

---

## Upgrade / validation

1. Flash firmware built from this tree; confirm **`FIRMWARE`** in **Advanced** or **device info** matches expectations.
2. **Home**: Confirm header shows **local time** and **`UTC (…)`** subline; advance time for ~1 minute (polling + `setInterval`) to confirm smooth ticking.
3. **Recordings**: Open a day with many files — first load fetches summary once; **page changes** should be fast; **Refresh** forces reload; order **newest first**.

---

## Known notes

- The Git tag **`Tango-AudioBuffer-Fix-2026-03-23`** was not found on `origin` in this workspace; the baseline above uses commit **`1f63028`** as the post–audio-buffer-fix version bump. If your GitHub release points at a different SHA, adjust the baseline row and re-run `git diff <that-sha>..HEAD`.
