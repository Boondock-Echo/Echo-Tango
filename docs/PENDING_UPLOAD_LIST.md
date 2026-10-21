# Per-day SD upload list

## Why this exists

A large upload backlog used to be tracked as an in-memory (SPIRAM) filename queue. That design grew more expensive as pending recordings accumulated.

The SD card is now the **source of truth** for what still needs to be uploaded. Firmware keeps only a small working set in RAM (the current file being uploaded, retry/skip state, and short-lived scan strings). It does **not** hold the entire backlog of names in RAM.

Recording files themselves were already on the card. This change only persists the **pending name list**.

## Layout

Recordings remain in date folders under `/pending`. Each day has:

- **`upload_list`** — append-only text file, one WAV **basename** per line (never rewritten to remove lines).
- **`upload_list.idx`** — single line with a 0-based line number pointing at the next pending entry in `upload_list`.

```
/pending/2026/09/01/
    2026-09-01-10-00-00.wav
    2026-09-01-10-00-30.wav
    upload_list
    upload_list.idx

/pending/2026/09/02/
    2026-09-02-09-15-00.wav
    upload_list
    upload_list.idx
```

Example after the first file uploaded (`upload_list.idx` contains `1`):

```
upload_list:
2026-09-01-10-00-00.wav
2026-09-01-10-00-30.wav

upload_list.idx:
1
```

Line 0 is skipped on read; only line 1 and later are candidates. Uploaded lines stay in `upload_list` — they are not deleted and the file is not rewritten.

September 1 recordings are appended only to `/pending/2026/09/01/upload_list`. September 2 recordings go to a new list in the September 2 folder.

**Design rule:** keep this simple. Append to the list, bump the index on success, read from the index forward. Do not rewrite `upload_list` or add compaction logic unless there is a proven SD wear or space problem.

## Lifecycle

| Event | List behavior |
| --- | --- |
| Recording finalized to `/pending/.../*.wav` | `uploadList_addPending()` appends the basename to that day's `upload_list` |
| Upload succeeds | File moves to `/inbox/YYYY/MM/DD/`; `upload_list.idx` advances past that basename (small write) |
| Upload fails | Index unchanged; basename stays eligible for retry |
| After a few consecutive failures on the same file | Upload task **skips** that path for the next pick so other pending names can proceed; the skipped name remains on the list |
| Stale line (name on list, WAV missing) | Index advances past that line the next time the list is read |

`.tmp` files are never added. Small discarded recordings are never added.

## How the upload task picks the next file

`uploadQueue_getNextFile()`:

1. **Fast path:** build today's UTC folder `/pending/YYYY/MM/DD` from the clock, read `upload_list.idx`, then read `upload_list` from that line onward (no `/pending` tree walk, no scan mutex).
2. **Fallback:** if today has nothing pending (or the clock is invalid), walk `/pending` and drain older days as before.

### Why the fast path exists

The fallback tree walk needs `g_pendingDirMutex`. While a recording finishes, the recorder can hold that lock for up to **3 seconds**. During back-to-back recordings the upload task often lost the lock, got no day folder, and logged `queue_empty` even though `upload_list` had names. Reading today's list by known path avoids that mutex fight for the common case (today's backlog).

### Newest-day-first policy

Day folders sort as `/pending/YYYY/MM/DD`. The upload task always drains the **most recent** day with pending names before older backlogs.

Example on calendar day 11 with pending files on days 9, 10, and 11:

| Order | Day folder | Notes |
| --- | --- | --- |
| 1 | Day 11 | Today's recordings upload first |
| 2 | Day 10 | Yesterday's backlog |
| 3 | Day 9 | Older backlog drains last |

Within a single day, names upload in list order (first unconsumed line first).

**Trade-off:** Fresh recordings reach the server quickly, but a large old backlog (e.g. day 9) may linger if new recordings keep arriving on newer days. Older days still upload whenever uploads outpace new captures.

Picking a file is **not** blocked while another clip is recording. Only an already-open `.wav` is uploaded; the live `.tmp` is never on `upload_list`. A tree walk is deferred only if an upload is already in progress.

Today's `upload_list` is read by path (UTC date + fixed filenames) so uploads are not blocked by the scan mutex during recording.

If a day folder has WAV files but no `upload_list` (legacy card or a failed append), the first **fallback** visit **seeds** a list from the WAV files in that folder and creates `upload_list.idx` at `0`.

Cards that already have `upload_list` but no index file treat the index as **0** (all lines pending).

## RAM vs SD

| Data | Where it lives |
| --- | --- |
| Full backlog of pending names | Per-day `upload_list` on SD (append-only) |
| Upload progress within a day | Per-day `upload_list.idx` on SD |
| WAV audio | `/pending/...` then `/inbox/...` after upload |
| Current upload + retry/skip path | Small buffers in the upload task |
| Pending count for UI/health | Non-empty lines at or after index, cached ~2 s |

There is no SPIRAM/DRAM array of pending SD filenames.

## API

| Function | Role |
| --- | --- |
| `uploadList_addPending(path)` | Append basename to the day's `upload_list` |
| `uploadList_removePending(path)` | Advance `upload_list.idx` past basename |
| `uploadQueue_getNextFile(skipPath)` | Next pending path from index + lists |
| `uploadQueue_markUploaded(path)` | Move WAV to inbox **and** advance the day's index |

## Related

- [Uploader.md](./Uploader.md) — upload task pipeline
- [SD_CARD_UPLOAD_RETRY_LOGIC.md](./SD_CARD_UPLOAD_RETRY_LOGIC.md) — retry / skip behavior
- [SD_BUS_LOCK.md](./SD_BUS_LOCK.md) — list I/O still uses `/pending` + SD bus locks
