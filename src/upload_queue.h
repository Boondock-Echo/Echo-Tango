#pragma once

#include <Arduino.h>
#include <time.h>

// Filesystem-based upload queue
// WAV files live in /pending/YYYY/MM/DD/. Pending names are append-only in upload_list;
// upload_list.idx holds the next line to upload. After success the file moves to /inbox
// and the index advances (the list file is not rewritten).

// Directory paths
constexpr const char* kPendingDir = "/pending";
constexpr const char* kTrashDir = "/trash";

// PSRAM queue entry for recordings when SD card is unavailable
struct PsramQueueEntry {
    uint8_t* data = nullptr;      // Pointer to WAV data in PSRAM
    size_t dataSize = 0;          // Size of WAV data
    uint32_t durationMs = 0;      // Recording duration
    float peakDb = -120.0f;       // Peak audio level
    time_t recordedAtEpoch = 0;   // Recording timestamp
    unsigned long recordedAtMs = 0; // Recording timestamp millis
    bool inUse = false;           // Whether this slot is in use
    uint8_t uploadRetryCount = 0; // Consecutive upload failures for this entry
};

// PSRAM queue constants
// Queue holds up to 6 completed recordings (30 sec each = 480044 bytes per recording)
// Total queue memory: 6 × 480044 bytes = 2880264 bytes ≈ 2.88MB
constexpr size_t kPsramQueueMaxEntries = 6;
constexpr size_t kPsramPoolSize = 7;
constexpr size_t kPsramPoolBufferSize = 480044U;

// Per-day pending-upload list on SD: /pending/YYYY/MM/DD/upload_list (append-only)
// and upload_list.idx (0-based line cursor; incremented on successful upload).
constexpr const char* kUploadListFileName = "upload_list";
constexpr const char* kUploadListIndexFileName = "upload_list.idx";

// PSRAM queue functions
bool psramQueue_begin();
bool psramQueue_addRecording(uint8_t* data, size_t dataSize, uint32_t durationMs, float peakDb, time_t recordedAtEpoch, unsigned long recordedAtMs);
PsramQueueEntry* psramQueue_getNextEntry();
void psramQueue_releaseEntry(PsramQueueEntry* entry);
void psramQueue_dropOldestEntry(); // Drop oldest entry when queue full (throttled error log; uploads may be stalled)
size_t psramQueue_getPendingCount();
size_t psramQueue_getAvailableSlots();

// PSRAM pool functions (used by queue and recorder)
bool psramPool_begin();
uint8_t *psramPool_take();

void psramPool_return(uint8_t *ptr);

// Append a pending WAV basename to that day's SD upload_list. Source of truth is the card, not RAM.
bool uploadList_addPending(const char* fullWavPath);

// Advance that day's upload_list.idx past basename after a successful upload (no list rewrite).
bool uploadList_removePending(const char* fullWavPath);

// Structure for index record data (matches upload tags)
struct UploadIndexRecord {
    uint32_t durationMs = 0;
    float peakDb = -120.0f;
    size_t sizeBytes = 0;
    size_t fileSize = 0;
    char endReason[32] = {0};
    char timestamp[32] = {0};           // Recording timestamp ISO format
    char uploadedAtTimestamp[32] = {0}; // Upload completion timestamp ISO format
    time_t uploadedAtEpoch = 0;         // Upload completion epoch
};

// Next pending path from per-day upload_list files (newest day first).
// Tries /pending/YYYY/MM/DD/upload_list from UTC clock before mutex-heavy tree walk.
// skipFullPath: optional full path to exclude (e.g. after max upload retries)
String uploadQueue_getNextFile(const char* skipFullPath = nullptr);

// Mark a file as uploaded by moving it to /inbox
// Returns true if file was moved/deleted successfully
bool uploadQueue_markUploaded(const char* path);

// Mark a file as uploaded and append record to daily index.json
// Returns true if file was moved/deleted successfully
bool uploadQueue_markUploadedWithRecord(const char* path, const UploadIndexRecord& record);

// Get count of files pending upload in /pending directory
size_t uploadQueue_getPendingCount();

// Invalidate cached filesystem pending .wav count (safe from any task; call after pending tree changes)
void uploadQueue_invalidateFilesystemPendingCountCache();

// Lock/unlock /pending directory for serialized access (recorder and upload task).
// Only directory operations are serialized: scan, create file, rename, remove. The lock is
// NOT held while reading file content or uploading; so continuous recording does not block
// uploads. Hold times are brief (ms). Timeout ms for lock (0 = no wait).
bool uploadQueue_lockPendingDir(uint32_t timeoutMs = 500);
void uploadQueue_unlockPendingDir();

// Initialize the upload queue (creates /pending directory if needed)
bool uploadQueue_begin();

// Check if a file exists in /pending
bool uploadQueue_fileExists(const char* path);

// Append a record to the daily index.json file after successful upload
// inboxPath: full path to file in inbox (e.g., /inbox/2025/11/26/2025-11-26-14-30-45.wav)
// record: metadata about the recording and upload
bool uploadQueue_appendToIndex(const char* inboxPath, const UploadIndexRecord& record);

// Clean up .tmp files by moving them to /trash/YYYY/MM/DD/
// Processes ALL files at once (use only during startup)
bool uploadQueue_cleanupTempFiles();

// Clean up ONE .tmp file by moving it to /trash/YYYY/MM/DD/
// Returns true if a file was processed, false if no files to process
// Should be called periodically (e.g., every 10 seconds) to avoid hanging
bool uploadQueue_cleanupOneTempFile();

