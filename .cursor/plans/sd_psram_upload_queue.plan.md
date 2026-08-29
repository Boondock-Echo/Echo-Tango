---
name: SD PSRAM upload queue
overview: PSRAM-backed recording/upload queue (up to 5000 entries) with per-entry lifecycle flags; boot starts empty queue + existing /pending cleanup; periodic reconciliation only for current and previous UTC day; web lists from summary file; disk reads mainly for upload and playback.
todos:
  - id: psram-queue-5k
    content: Implement SPIRAM queue (5000 slots) with per-entry state TmpRecording | Complete | Uploaded; transitions and removal on upload
    status: pending
  - id: unified-pipeline
    content: Wire recorder (start→tmp, finalize→complete), upload task (complete→upload→remove), summary from queue metadata at finalize
    status: pending
    dependencies:
      - psram-queue-5k
  - id: boot-clean-queue
    content: On startup initialize empty PSRAM queue; rely on existing /pending cleanup process already in codebase
    status: pending
    dependencies:
      - psram-queue-5k
  - id: reconciliation-2day
    content: Periodic reconciliation scan limited to UTC today and yesterday under /pending only; align disk vs queue
    status: pending
    dependencies:
      - psram-queue-5k
  - id: web-summary
    content: Confirm web recordings list uses summary file only; playback remains direct WAV read (no change to list-from-disk)
    status: pending
    dependencies:
      - unified-pipeline
  - id: mutex-withrecord
    content: Serialize uploadQueue_markUploadedWithRecord with g_pendingDirMutex
    status: pending
  - id: observability
    content: Structured reasons (wifi, mutex_timeout, open_failed, endpoint_*, queue_full, reconcile_day_scan)
    status: pending
    dependencies:
      - unified-pipeline
  - id: docs
    content: Update SD_CARD_RECORDING_RECOMMENDATIONS.md with architecture, flags, boot, 2-day reconcile
    status: pending
isProject: false
---

# SD recording: PSRAM queue with lifecycle flags (5000 entries)

## Goal

Use **PSRAM** for a **large queue** (target **5000** entries) that tracks each pending recording through its **lifecycle**, so upload discovery does not require **full-tree** scans of `/pending` in steady state.

**SD card reads** are primarily:

- **Upload**: open known path, read WAV.
- **Web playback**: open known path for streaming.
- **Reconciliation** (periodic, **bounded**): list only **current UTC day** and **previous UTC day** under `/pending` to fix RAM/disk drift.

**Web listing** uses the **summary file** (small, single file to read for the UI) — not a directory tree walk. Only **playing** audio reads the WAV from disk.

---

## Boot / crash (reboot)

- On device start, **initialize an empty PSRAM queue** (no replay of SD contents into RAM at boot).
- Rely on the **existing in-firmware process** that cleans up `/pending` (stale `.tmp`, etc.) so the card does not require the queue to be rebuilt from a full scan at startup.
- **Implication:** Any `.wav` left on disk after reboot that is **not** re-added by reconciliation (e.g. very old paths outside the 2-day reconcile window) depends on **that cleanup policy** and product expectations; document if **orphans** outside the 2-day window need a rare full scan or manual policy.

---

## Per-entry lifecycle flags (PSRAM queue)

Each slot carries at least: **basename or key**, paths/metadata as needed, and **state**:


| State                       | Meaning                                                                                                            |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| **1 — Recording to `.tmp`** | Active capture; file is the current `.tmp` under `/pending/...`. Upload must not consume this slot for upload yet. |
| **2 — Recording complete**  | `.tmp` renamed to `.wav`; ready for upload ordering.                                                               |
| **3 — Uploaded**            | Upload succeeded and file moved/handled; **remove this entry from the PSRAM queue** (slot freed).                  |


**Flow:**

1. **Start recording** — allocate/insert entry with state **Recording tmp** (and current tmp path or reconstructable basename).
2. **Finalize** — after successful rename to `.wav`, set state **Complete**; append **summary** line from this metadata (no SD scan for “what was recorded”).
3. **Upload task** — picks **Complete** entries (policy: oldest first or as defined); after successful `markUploaded` / `markUploadedWithRecord`, set **Uploaded** and **delete** the entry from the queue.

**Note:** `uploadQueue_getPendingCount` / UI “queue” metrics should count entries in states **tmp** + **complete** (not uploaded), or as product-defined.

---

## Periodic reconciliation (bounded)

- Run **periodically** (e.g. every N minutes), **not** a full `/pending` tree walk.
- **Scope:** Only **current UTC calendar day** and **previous UTC calendar day** — i.e. traverse at most:
  - `/pending/YYYY/MM/DD` for **today** (UTC), and  
  - `/pending/YYYY/MM/DD` for **yesterday** (UTC).
- **Purpose:** Find `.wav` files on disk that are **missing** from the PSRAM queue (or fix stale flags) after crashes, failed enqueues, or edge cases — **without** scanning months of history each time.
- Log at debug: `reconcile_day_scan` with day range; rate-limit warnings on drift.

---

## Web recordings UI

- **List / browse:** driven by the **summary file** (already a small single-file read; aligns with “no big folder parse” for UI).
- **Playback:** reads the **audio file** from disk by path — acceptable and expected.

No requirement to list pending from a recursive SD listing in the web stack if summary is authoritative for “what exists” in the product model.

---

## Benefits (brief)

- Next upload from **RAM state machine**, not full tree walk.
- Clear **in-progress vs ready vs done** semantics.
- Reboot is **simple** (empty queue + existing cleanup).
- Reconciliation stays **O(files in two days)** not **O(all pending ever)**.

---

## Risks to keep in mind

1. **Orphans older than 2 days** — If a file sits in `/pending` from **before** yesterday (UTC) and was never uploaded, **reconciliation will not** see it. Mitigation: extend window occasionally, or keep a **rare** full scan on boot only, or rely on existing `/pending` cleanup + ops policy. **Call this out in docs.**
2. **RAM size** — 5000 × (metadata + flags + strings) — verify PSRAM budget.
3. **Queue full** — Define policy when 5000 slots exhausted (block new recording vs drop vs spill).
4. **Concurrency** — Mutex around queue; upload must not read `.tmp` while state is still “recording” unless explicitly allowed (prefer **not** uploading until **Complete**).
5. `**markUploadedWithRecord`** — Serialize with `g_pendingDirMutex` like `markUploaded` (`[upload_queue.cpp](src/upload_queue.cpp)`).

---

## Implementation shape (summary table)


| Area                    | Direction                                                                          |
| ----------------------- | ---------------------------------------------------------------------------------- |
| Queue                   | SPIRAM; 5000 slots; lifecycle enum; mutex                                          |
| Boot                    | Empty queue + existing `/pending` cleanup                                          |
| Recorder                | Insert/update on start; transition to Complete on finalize; summary from same data |
| Upload                  | Iterate Complete entries; on success → remove entry (Uploaded)                     |
| Reconcile               | Periodic; UTC **today + yesterday** only under `/pending`                          |
| Web                     | List from **summary**; play from disk                                              |
| Full-tree `getNextFile` | Remove from hot path; optional rare fallback or boot-only if product requires      |


---

## Relationship to prior iterations

Supersedes the “boot seed from full scan” optional path unless product later requires it. **50-slot** DRAM queue is **replaced** by this unified PSRAM lifecycle queue at scale.