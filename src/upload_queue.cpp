#include "upload_queue.h"
#include "network.h"
#include "networkHandller.h"
#include "logger.h"
#include "common.h"
#include "timekeeper.h"
#include "recorder.h"
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>

// Mutex timeout for queue/pool operations (upload task backs off so recorder is not disrupted)
static constexpr uint32_t kQueueMutexTimeoutMs = 300;

// Brief /pending locks for directory scans (do not hold mutex across full-tree walk)
static constexpr uint32_t kPendingScanLockMs = 400;
static constexpr int kPendingScanLockAttempts = 3;

// Recorder rename and markUploaded share longer lock attempts
static constexpr uint32_t kPendingRenameLockMs = 3000;
static constexpr int kPendingRenameLockAttempts = 3;
static constexpr uint32_t kPendingRenameRetryDelayMs = 100;

// Full-tree count of .wav under /pending is expensive; cache briefly for UI/health polling
static constexpr unsigned long kFsPendingWavCountCacheTtlMs = 2000;
static std::atomic<unsigned long> g_fsPendingWavCountCacheStampMs{0};
static size_t g_fsPendingWavCountCachedResult = 0;
static constexpr int kReleaseRetryCount = 3;
static constexpr uint32_t kReleaseRetryDelayMs = 10;

// PSRAM queue for recordings when SD card is unavailable
namespace {
    PsramQueueEntry g_psramQueue[kPsramQueueMaxEntries];
    bool g_psramQueueInitialized = false;

    // PSRAM buffer pool (7 buffers of 480044 bytes each)
    uint8_t *g_psramPoolBuffers[kPsramPoolSize] = {nullptr};
    bool g_psramPoolInUse[kPsramPoolSize] = {false};
    bool g_psramPoolInitialized = false;
    static PsramQueueEntry *g_entryInUpload = nullptr; // skip this entry in dropOldestEntry

    // SD card pending-path queue (SPIRAM when possible)
    SdCardMemoryQueueEntry* g_sdCardMemoryQueue = nullptr;
    bool g_sdCardMemoryQueueInitialized = false;
}

// Mutexes for recorder/upload task synchronization (created in *_begin, never exposed)
static SemaphoreHandle_t g_psramMutex = nullptr;
static SemaphoreHandle_t g_sdCardMemoryQueueMutex = nullptr;
// Mutex for /pending directory access (scan vs create/rename)
static SemaphoreHandle_t g_pendingDirMutex = nullptr;

// Return buffer to pool without taking mutex (call only while holding g_psramMutex)
static void psramPool_return_unlocked(uint8_t* ptr)
{
    if (ptr == nullptr || !g_psramPoolInitialized)
    {
        return;
    }
    for (size_t i = 0; i < kPsramPoolSize; ++i)
    {
        if (g_psramPoolBuffers[i] == ptr)
        {
            g_psramPoolInUse[i] = false;
            return;
        }
    }
}

bool psramPool_begin()
{
    if (g_psramPoolInitialized)
    {
        return true;
    }
    for (size_t i = 0; i < kPsramPoolSize; ++i)
    {
        g_psramPoolBuffers[i] = static_cast<uint8_t *>(heap_caps_malloc(kPsramPoolBufferSize, MALLOC_CAP_SPIRAM));
        if (g_psramPoolBuffers[i] == nullptr)
        {
            while (i > 0)
            {
                heap_caps_free(g_psramPoolBuffers[--i]);
                g_psramPoolBuffers[i] = nullptr;
            }
            return false;
        }
        g_psramPoolInUse[i] = false;
    }
    g_psramPoolInitialized = true;
    return true;
}

uint8_t *psramPool_take()
{
    if (!g_psramPoolInitialized || g_psramMutex == nullptr)
    {
        return nullptr;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        return nullptr;
    }
    uint8_t* result = nullptr;
    for (size_t i = 0; i < kPsramPoolSize; ++i)
    {
        if (!g_psramPoolInUse[i])
        {
            g_psramPoolInUse[i] = true;
            result = g_psramPoolBuffers[i];
            break;
        }
    }
    xSemaphoreGive(g_psramMutex);
    return result;
}

void psramPool_return(uint8_t *ptr)
{
    if (ptr == nullptr || !g_psramPoolInitialized || g_psramMutex == nullptr)
    {
        return;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        return;
    }
    psramPool_return_unlocked(ptr);
    xSemaphoreGive(g_psramMutex);
}

bool psramQueue_begin() {
    if (g_psramQueueInitialized) {
        return true;
    }

    if (g_psramMutex == nullptr) {
        g_psramMutex = xSemaphoreCreateMutex();
        if (g_psramMutex == nullptr) {
            return false;
        }
    }

    if (!psramPool_begin()){
        return false;
    }

    // Initialize all queue entries
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        g_psramQueue[i].data = nullptr;
        g_psramQueue[i].dataSize = 0;
        g_psramQueue[i].durationMs = 0;
        g_psramQueue[i].peakDb = -120.0f;
        g_psramQueue[i].recordedAtEpoch = 0;
        g_psramQueue[i].recordedAtMs = 0;
        g_psramQueue[i].inUse = false;
        g_psramQueue[i].uploadRetryCount = 0;
    }
    
    g_psramQueueInitialized = true;
    return true;
}

bool psramQueue_addRecording(uint8_t* data, size_t dataSize, uint32_t durationMs, float peakDb, time_t recordedAtEpoch, unsigned long recordedAtMs) {
    if (!g_psramQueueInitialized) {
        psramQueue_begin();
    }
    if (g_psramMutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE) {
        return false;
    }
    bool added = false;
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        if (!g_psramQueue[i].inUse) {
            g_psramQueue[i].data = data;
            g_psramQueue[i].dataSize = dataSize;
            g_psramQueue[i].durationMs = durationMs;
            g_psramQueue[i].peakDb = peakDb;
            g_psramQueue[i].recordedAtEpoch = recordedAtEpoch;
            g_psramQueue[i].recordedAtMs = recordedAtMs;
            g_psramQueue[i].inUse = true;
            g_psramQueue[i].uploadRetryCount = 0;
            added = true;
            break;
        }
    }
    xSemaphoreGive(g_psramMutex);
    if (!added) {
        logWarnf("[PsramQueue] Queue full, cannot add recording");
    }
    return added;
}

PsramQueueEntry* psramQueue_getNextEntry() {
    if (!g_psramQueueInitialized || g_psramMutex == nullptr) {
        g_entryInUpload = nullptr;
        return nullptr;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE) {
        return nullptr;
    }
    PsramQueueEntry* oldest = nullptr;
    time_t oldestEpoch = 0;
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        if (g_psramQueue[i].inUse && g_psramQueue[i].data != nullptr) {
            if (oldest == nullptr || g_psramQueue[i].recordedAtEpoch < oldestEpoch) {
                oldest = &g_psramQueue[i];
                oldestEpoch = g_psramQueue[i].recordedAtEpoch;
            }
        }
    }
    g_entryInUpload = oldest;
    xSemaphoreGive(g_psramMutex);
    return oldest;
}

void psramQueue_releaseEntry(PsramQueueEntry* entry) {
    if (entry == nullptr || g_psramMutex == nullptr) {
        return;
    }
    for (int retry = 0; retry < kReleaseRetryCount; ++retry) {
        if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) == pdTRUE) {
            if (g_entryInUpload == entry) {
                g_entryInUpload = nullptr;
            }
            if (entry->data != nullptr) {
                psramPool_return_unlocked(entry->data);
                entry->data = nullptr;
            }
            entry->dataSize = 0;
            entry->durationMs = 0;
            entry->peakDb = -120.0f;
            entry->recordedAtEpoch = 0;
            entry->recordedAtMs = 0;
            entry->inUse = false;
            entry->uploadRetryCount = 0;
            xSemaphoreGive(g_psramMutex);
            return;
        }
        if (retry < kReleaseRetryCount - 1) {
            vTaskDelay(pdMS_TO_TICKS(kReleaseRetryDelayMs));
        }
    }
    logErrorf("[PsramQueue] releaseEntry mutex timeout, slot may be stuck until reboot");
}

void psramQueue_dropOldestEntry() {
    if (!g_psramQueueInitialized || g_psramMutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE) {
        return;
    }
    PsramQueueEntry *oldest = nullptr;
    time_t oldestEpoch = 0;
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        if (!g_psramQueue[i].inUse || g_psramQueue[i].data == nullptr) {
            continue;
        }
        if (&g_psramQueue[i] == g_entryInUpload) {
            continue;
        }
        if (oldest == nullptr || g_psramQueue[i].recordedAtEpoch < oldestEpoch) {
            oldest = &g_psramQueue[i];
            oldestEpoch = g_psramQueue[i].recordedAtEpoch;
        }
    }
    if (oldest == nullptr) {
        xSemaphoreGive(g_psramMutex);
        return;
    }
    // Server unreachable or upload stalled: dropping is expected backpressure, not a system-fatal condition.
    {
        static unsigned long s_lastPsramDropLogMs = 0;
        constexpr unsigned long kPsramDropLogMinIntervalMs = 30000;
        const unsigned long now = millis();
        if (s_lastPsramDropLogMs == 0 || (now - s_lastPsramDropLogMs) >= kPsramDropLogMinIntervalMs)
        {
            s_lastPsramDropLogMs = now;
            logErrorf("[PsramQueue] Dropping oldest recording (queue full; uploads not draining — check network/server)");
        }
    }
    if (oldest->data != nullptr) {
        psramPool_return_unlocked(oldest->data);
        oldest->data = nullptr;
    }
    oldest->dataSize = 0;
    oldest->durationMs = 0;
    oldest->peakDb = -120.0f;
    oldest->recordedAtEpoch = 0;
    oldest->recordedAtMs = 0;
    oldest->inUse = false;
    oldest->uploadRetryCount = 0;
    xSemaphoreGive(g_psramMutex);
}

size_t psramQueue_getPendingCount() {
    if (!g_psramQueueInitialized || g_psramMutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        if (g_psramQueue[i].inUse) {
            count++;
        }
    }
    xSemaphoreGive(g_psramMutex);
    return count;
}

size_t psramQueue_getAvailableSlots() {
    if (!g_psramQueueInitialized || g_psramMutex == nullptr) {
        return kPsramQueueMaxEntries;
    }
    if (xSemaphoreTake(g_psramMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < kPsramQueueMaxEntries; ++i) {
        if (g_psramQueue[i].inUse) {
            count++;
        }
    }
    size_t available = kPsramQueueMaxEntries - count;
    xSemaphoreGive(g_psramMutex);
    return available;
}

// SD card pending-path queue (SPIRAM-backed slot array)
namespace {
    static void copyBasenameFromFullPath(const char* fullPath, char* out, size_t outLen)
    {
        if (fullPath == nullptr || out == nullptr || outLen == 0)
        {
            return;
        }
        const char* slash = strrchr(fullPath, '/');
        const char* base = slash ? slash + 1 : fullPath;
        strncpy(out, base, outLen - 1);
        out[outLen - 1] = '\0';
    }

    static void sdCardMemoryQueue_dropOldest_unlocked()
    {
        if (g_sdCardMemoryQueue == nullptr)
        {
            return;
        }
        SdCardMemoryQueueEntry* oldest = nullptr;
        time_t oldestEpoch = 0;
        for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
        {
            if (g_sdCardMemoryQueue[i].inUse && g_sdCardMemoryQueue[i].basename[0] != '\0')
            {
                if (oldest == nullptr || g_sdCardMemoryQueue[i].recordedAtEpoch < oldestEpoch)
                {
                    oldest = &g_sdCardMemoryQueue[i];
                    oldestEpoch = g_sdCardMemoryQueue[i].recordedAtEpoch;
                }
            }
        }
        if (oldest != nullptr)
        {
            logWarnf("[SdCardMemoryQueue] Dropping oldest queued basename (queue full): %s", oldest->basename);
            oldest->basename[0] = '\0';
            oldest->recordedAtEpoch = 0;
            oldest->recordedAtMs = 0;
            oldest->inUse = false;
            oldest->uploadRetryCount = 0;
        }
    }
}

bool sdCardMemoryQueue_buildFullPath(const char* basename, char* out, size_t outLen)
{
    if (basename == nullptr || out == nullptr || outLen < kMaxUploadPathLength)
    {
        return false;
    }
    if (basename[0] == '\0')
    {
        return false;
    }
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    unsigned long sfx = 0;
    if (std::sscanf(basename, "%d-%d-%d-%d-%d-%d_%lu.wav", &y, &mo, &d, &hh, &mm, &ss, &sfx) == 7)
    {
        std::snprintf(out, outLen, "/pending/%04d/%02d/%02d/%s", y, mo, d, basename);
        return true;
    }
    if (std::sscanf(basename, "%d-%d-%d-%d-%d-%d.wav", &y, &mo, &d, &hh, &mm, &ss) == 6)
    {
        std::snprintf(out, outLen, "/pending/%04d/%02d/%02d/%s", y, mo, d, basename);
        return true;
    }
    return false;
}

bool sdCardMemoryQueue_begin()
{
    if (g_sdCardMemoryQueueInitialized)
    {
        return true;
    }
    if (g_sdCardMemoryQueueMutex == nullptr)
    {
        g_sdCardMemoryQueueMutex = xSemaphoreCreateMutex();
        if (g_sdCardMemoryQueueMutex == nullptr)
        {
            return false;
        }
    }
    const size_t bytes = kSdCardMemoryQueueMaxEntries * sizeof(SdCardMemoryQueueEntry);
    g_sdCardMemoryQueue = static_cast<SdCardMemoryQueueEntry*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (g_sdCardMemoryQueue == nullptr)
    {
        g_sdCardMemoryQueue = static_cast<SdCardMemoryQueueEntry*>(malloc(bytes));
        if (g_sdCardMemoryQueue == nullptr)
        {
            logErrorf("[SdCardMemoryQueue] Failed to allocate queue storage\n");
            return false;
        }
        logWarnf("[SdCardMemoryQueue] SPIRAM alloc failed; using DRAM for pending-path queue");
    }
    for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
    {
        g_sdCardMemoryQueue[i].basename[0] = '\0';
        g_sdCardMemoryQueue[i].recordedAtEpoch = 0;
        g_sdCardMemoryQueue[i].recordedAtMs = 0;
        g_sdCardMemoryQueue[i].inUse = false;
        g_sdCardMemoryQueue[i].uploadRetryCount = 0;
    }
    g_sdCardMemoryQueueInitialized = true;
    return true;
}

bool sdCardMemoryQueue_addRecording(const char* fullPath, time_t recordedAtEpoch, unsigned long recordedAtMs)
{
    if (!g_sdCardMemoryQueueInitialized)
    {
        sdCardMemoryQueue_begin();
    }
    if (fullPath == nullptr || strlen(fullPath) == 0 || g_sdCardMemoryQueueMutex == nullptr || g_sdCardMemoryQueue == nullptr)
    {
        return false;
    }
    if (xSemaphoreTake(g_sdCardMemoryQueueMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        logDebugf("[UploadTask] skip reason=mutex_timeout (sdCardMemoryQueue add)");
        return false;
    }
    char baseBuf[64];
    copyBasenameFromFullPath(fullPath, baseBuf, sizeof(baseBuf));
    if (baseBuf[0] == '\0')
    {
        xSemaphoreGive(g_sdCardMemoryQueueMutex);
        return false;
    }
    bool added = false;
    for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
    {
        if (!g_sdCardMemoryQueue[i].inUse)
        {
            strncpy(g_sdCardMemoryQueue[i].basename, baseBuf, sizeof(g_sdCardMemoryQueue[i].basename) - 1);
            g_sdCardMemoryQueue[i].basename[sizeof(g_sdCardMemoryQueue[i].basename) - 1] = '\0';
            g_sdCardMemoryQueue[i].recordedAtEpoch = recordedAtEpoch;
            g_sdCardMemoryQueue[i].recordedAtMs = recordedAtMs;
            g_sdCardMemoryQueue[i].inUse = true;
            logDebugf("[SdCardMemoryQueue] Added basename to queue: %s", baseBuf);
            added = true;
            break;
        }
    }
    if (!added)
    {
        sdCardMemoryQueue_dropOldest_unlocked();
        for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
        {
            if (!g_sdCardMemoryQueue[i].inUse)
            {
                strncpy(g_sdCardMemoryQueue[i].basename, baseBuf, sizeof(g_sdCardMemoryQueue[i].basename) - 1);
                g_sdCardMemoryQueue[i].basename[sizeof(g_sdCardMemoryQueue[i].basename) - 1] = '\0';
                g_sdCardMemoryQueue[i].recordedAtEpoch = recordedAtEpoch;
                g_sdCardMemoryQueue[i].recordedAtMs = recordedAtMs;
                g_sdCardMemoryQueue[i].inUse = true;
                logDebugf("[SdCardMemoryQueue] Added basename after drop-oldest: %s", baseBuf);
                added = true;
                break;
            }
        }
    }
    xSemaphoreGive(g_sdCardMemoryQueueMutex);
    if (!added)
    {
        logWarnf("[SdCardMemoryQueue] Queue full, cannot add recording: %s", fullPath);
    }
    return added;
}

SdCardMemoryQueueEntry* sdCardMemoryQueue_getNextEntry()
{
    if (!g_sdCardMemoryQueueInitialized || g_sdCardMemoryQueueMutex == nullptr || g_sdCardMemoryQueue == nullptr)
    {
        return nullptr;
    }
    if (xSemaphoreTake(g_sdCardMemoryQueueMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        logDebugf("[UploadTask] skip reason=mutex_timeout (sdCardMemoryQueue getNext)");
        return nullptr;
    }
    SdCardMemoryQueueEntry* oldest = nullptr;
    time_t oldestEpoch = 0;
    for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
    {
        if (g_sdCardMemoryQueue[i].inUse && g_sdCardMemoryQueue[i].basename[0] != '\0')
        {
            if (oldest == nullptr || g_sdCardMemoryQueue[i].recordedAtEpoch < oldestEpoch)
            {
                oldest = &g_sdCardMemoryQueue[i];
                oldestEpoch = g_sdCardMemoryQueue[i].recordedAtEpoch;
            }
        }
    }
    xSemaphoreGive(g_sdCardMemoryQueueMutex);
    return oldest;
}

void sdCardMemoryQueue_releaseEntry(SdCardMemoryQueueEntry* entry)
{
    if (entry == nullptr || g_sdCardMemoryQueueMutex == nullptr)
    {
        return;
    }
    if (xSemaphoreTake(g_sdCardMemoryQueueMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        logDebugf("[UploadTask] skip reason=mutex_timeout (sdCardMemoryQueue release)");
        return;
    }
    entry->basename[0] = '\0';
    entry->recordedAtEpoch = 0;
    entry->recordedAtMs = 0;
    entry->inUse = false;
    xSemaphoreGive(g_sdCardMemoryQueueMutex);
}

size_t sdCardMemoryQueue_getPendingCount()
{
    if (!g_sdCardMemoryQueueInitialized || g_sdCardMemoryQueueMutex == nullptr || g_sdCardMemoryQueue == nullptr)
    {
        return 0;
    }
    if (xSemaphoreTake(g_sdCardMemoryQueueMutex, pdMS_TO_TICKS(kQueueMutexTimeoutMs)) != pdTRUE)
    {
        logDebugf("[UploadTask] skip reason=mutex_timeout (sdCardMemoryQueue getPendingCount)");
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < kSdCardMemoryQueueMaxEntries; ++i)
    {
        if (g_sdCardMemoryQueue[i].inUse)
        {
            count++;
        }
    }
    xSemaphoreGive(g_sdCardMemoryQueueMutex);
    return count;
}

bool sdCardMemoryQueue_isEmpty()
{
    return sdCardMemoryQueue_getPendingCount() == 0;
}

bool uploadQueue_lockPendingDir(uint32_t timeoutMs) {
    if (g_pendingDirMutex == nullptr) {
        return true; // not yet initialized, single-threaded
    }
    return (xSemaphoreTake(g_pendingDirMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
}

void uploadQueue_unlockPendingDir() {
    if (g_pendingDirMutex != nullptr) {
        xSemaphoreGive(g_pendingDirMutex);
    }
}

void uploadQueue_invalidateFilesystemPendingCountCache()
{
    g_fsPendingWavCountCacheStampMs.store(0, std::memory_order_release);
}

bool uploadQueue_begin() {
    uploadQueue_invalidateFilesystemPendingCountCache();
    // In-memory queue for SD card mode (harmless when in PSRAM mode)
    if (!sdCardMemoryQueue_begin()) {
        logErrorf("[UploadQueue] Failed to initialize SD card memory queue\n");
        return false;
    }
    // Only create /pending, /trash, and mutex when SD card is the active storage
    // (no SD card, disabled, init failed, or PSRAM recording mode → skip directory creation)
    if (!isStorageModeSdCard()) {
        return true;
    }
    if (g_pendingDirMutex == nullptr) {
        g_pendingDirMutex = xSemaphoreCreateMutex();
        if (g_pendingDirMutex == nullptr) {
            return false;
        }
    }
    // Create /pending directory if it doesn't exist
    if (!SD_MMC.exists(kPendingDir)) {
        if (!SD_MMC.mkdir(kPendingDir)) {
            logErrorf("[UploadQueue] Failed to create %s directory\n", kPendingDir);
            return false;
        }
    }
    // Create /trash directory if it doesn't exist
    if (!SD_MMC.exists(kTrashDir)) {
        if (!SD_MMC.mkdir(kTrashDir)) {
            logErrorf("[UploadQueue] Failed to create %s directory\n", kTrashDir);
            return false;
        }
    }
    return true;
}

// Pending layout: /pending/YYYY/MM/DD/YYYY-MM-DD-HH-MM-SS.{tmp,wav} (same hierarchy as inbox)
static String joinPendingPath(const String& base, const String& nameIn)
{
    const String name = nameIn;
    if (name.length() == 0)
    {
        return base;
    }
    if (name.startsWith("/"))
    {
        return name;
    }
    if (base.endsWith("/"))
    {
        return base + name;
    }
    return base + "/" + name;
}

static String basenameOnly(const String& nameStr)
{
    const int lastSlash = nameStr.lastIndexOf('/');
    if (lastSlash >= 0)
    {
        return nameStr.substring(lastSlash + 1);
    }
    return nameStr;
}

static bool lockPendingDirScanAttempt()
{
    for (int attempt = 0; attempt < kPendingScanLockAttempts; ++attempt)
    {
        if (uploadQueue_lockPendingDir(kPendingScanLockMs))
        {
            return true;
        }
        if (attempt + 1 < kPendingScanLockAttempts)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

static bool lockPendingDirRenameAttempt()
{
    for (int attempt = 0; attempt < kPendingRenameLockAttempts; ++attempt)
    {
        if (uploadQueue_lockPendingDir(kPendingRenameLockMs))
        {
            return true;
        }
        if (attempt + 1 < kPendingRenameLockAttempts)
        {
            vTaskDelay(pdMS_TO_TICKS(kPendingRenameRetryDelayMs));
        }
    }
    return false;
}

static bool shouldDeferPendingFilesystemScan()
{
    return recorder_isRecording() || networkHandler_isUploading();
}

static size_t getCachedFilesystemPendingCountIfValid()
{
    const unsigned long now = millis();
    const unsigned long stamp = g_fsPendingWavCountCacheStampMs.load(std::memory_order_acquire);
    if (stamp != 0 && (unsigned long)(now - stamp) < kFsPendingWavCountCacheTtlMs)
    {
        return g_fsPendingWavCountCachedResult;
    }
    return SIZE_MAX;
}

static void scanPendingCollectNewestWav(const String& dirPath, String& newestFullPath, String& newestName,
                                        size_t& fileCount, uint32_t& iterationCount,
                                        const char* skipFullPath = nullptr)
{
    if (!lockPendingDirScanAttempt())
    {
        return;
    }
    File dir = SD_MMC.open(dirPath);
    uploadQueue_unlockPendingDir();
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }
    while (true)
    {
        if (++iterationCount % 10 == 0)
        {
            esp_task_wdt_reset();
        }
        if (!lockPendingDirScanAttempt())
        {
            break;
        }
        File entry = dir.openNextFile();
        uploadQueue_unlockPendingDir();
        if (!entry)
        {
            break;
        }
        const bool isDir = entry.isDirectory();
        const String nameStr = String(entry.name());
        entry.close();
        if (nameStr.length() == 0 || nameStr == "." || nameStr == "..")
        {
            continue;
        }
        const String fullPath = joinPendingPath(dirPath, nameStr);
        if (isDir)
        {
            scanPendingCollectNewestWav(fullPath, newestFullPath, newestName, fileCount, iterationCount,
                                        skipFullPath);
        }
        else
        {
            const String justName = basenameOnly(nameStr);
            if (justName.endsWith(".wav"))
            {
                fileCount++;
                if (skipFullPath != nullptr && skipFullPath[0] != '\0' && fullPath == skipFullPath)
                {
                    continue;
                }
                if (newestName.isEmpty() || justName > newestName)
                {
                    if (!newestName.isEmpty())
                    {
                        logDebugf("[UploadQueue] Found newer file: %s (replacing %s)", justName.c_str(), newestName.c_str());
                    }
                    newestName = justName;
                    newestFullPath = fullPath;
                }
            }
        }
    }
    esp_task_wdt_reset();
    dir.close();
}

static void scanPendingCountWav(const String& dirPath, size_t& count, uint32_t& iterationCount)
{
    if (!lockPendingDirScanAttempt())
    {
        return;
    }
    File dir = SD_MMC.open(dirPath);
    uploadQueue_unlockPendingDir();
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }
    while (true)
    {
        if (++iterationCount % 10 == 0)
        {
            esp_task_wdt_reset();
        }
        if (!lockPendingDirScanAttempt())
        {
            break;
        }
        File entry = dir.openNextFile();
        uploadQueue_unlockPendingDir();
        if (!entry)
        {
            break;
        }
        const bool isDir = entry.isDirectory();
        const String nameStr = String(entry.name());
        entry.close();
        if (nameStr.length() == 0 || nameStr == "." || nameStr == "..")
        {
            continue;
        }
        const String fullPath = joinPendingPath(dirPath, nameStr);
        if (isDir)
        {
            scanPendingCountWav(fullPath, count, iterationCount);
        }
        else if (basenameOnly(nameStr).endsWith(".wav"))
        {
            count++;
        }
    }
    esp_task_wdt_reset();
    dir.close();
}

static bool findFirstTmpPending(const String& dirPath, const String& skipPath, String& outFullPath, size_t& tmpFileCount,
                                uint32_t& iterationCount)
{
    if (!lockPendingDirScanAttempt())
    {
        return false;
    }
    File dir = SD_MMC.open(dirPath);
    uploadQueue_unlockPendingDir();
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return false;
    }
    while (true)
    {
        if (++iterationCount % 10 == 0)
        {
            esp_task_wdt_reset();
        }
        if (!lockPendingDirScanAttempt())
        {
            break;
        }
        File entry = dir.openNextFile();
        uploadQueue_unlockPendingDir();
        if (!entry)
        {
            break;
        }
        const bool isDir = entry.isDirectory();
        const String nameStr = String(entry.name());
        entry.close();
        if (nameStr.length() == 0 || nameStr == "." || nameStr == "..")
        {
            continue;
        }
        const String fullPath = joinPendingPath(dirPath, nameStr);
        if (isDir)
        {
            if (findFirstTmpPending(fullPath, skipPath, outFullPath, tmpFileCount, iterationCount))
            {
                dir.close();
                return true;
            }
            continue;
        }
        const String justName = basenameOnly(nameStr);
        if (justName.endsWith(".tmp"))
        {
            tmpFileCount++;
            if (!skipPath.isEmpty() && fullPath == skipPath)
            {
                continue;
            }
            outFullPath = fullPath;
            dir.close();
            return true;
        }
    }
    esp_task_wdt_reset();
    dir.close();
    return false;
}

// Get the NEWEST file from /pending tree (most recent timestamp has highest priority)
// Filenames are YYYY-MM-DD-HH-MM-SS.wav so lexicographic comparison works
String uploadQueue_getNextFile(const char* skipFullPath) {
    if (!isStorageModeSdCard()) {
        return String();
    }
    if (shouldDeferPendingFilesystemScan()) {
        logDebugf("[UploadQueue] defer /pending scan (recording or upload active)");
        return String();
    }
    if (!sdCardMemoryQueue_isEmpty()) {
        logDebugf("[UploadQueue] skip filesystem scan (memory queue non-empty)");
        return String();
    }
    logDebugf("[UploadQueue] Scanning /pending tree for .wav files (newest first priority)");
    if (!SD_MMC.exists(kPendingDir)) {
        return String();
    }

    String newestFile;
    String newestName;
    size_t fileCount = 0;
    uint32_t iterationCount = 0;
    scanPendingCollectNewestWav(String(kPendingDir), newestFile, newestName, fileCount, iterationCount,
                                skipFullPath);

    if (!newestFile.isEmpty()) {
        logDebugf("[UploadQueue] Selected newest file from %zu .wav files: %s", fileCount, newestFile.c_str());
    } else {
        logDebugf("[UploadQueue] No .wav files found under /pending (scanned %zu .wav)", fileCount);
    }
    return newestFile;
}

// Internal helper to move file to inbox and return destination path
static String moveFileToInbox(const char* path) {
    if (path == nullptr || strlen(path) == 0) {
        return String();
    }
    
    if (!isStorageModeSdCard()) {
        return String();
    }
    
    if (!SD_MMC.exists(path)) {
        return String();
    }
    
    // Extract filename from path (e.g., "2025-11-26-14-30-45.wav")
    String srcPath = path;
    int lastSlash = srcPath.lastIndexOf('/');
    String filename = (lastSlash >= 0) ? srcPath.substring(lastSlash + 1) : srcPath;
    
    // Parse date from filename: YYYY-MM-DD-HH-MM-SS.wav
    // Extract YYYY, MM, DD
    if (filename.length() < 19) {
        // Filename too short, just delete it
        SD_MMC.remove(path);
        return String();
    }
    
    String year = filename.substring(0, 4);
    String month = filename.substring(5, 7);
    String day = filename.substring(8, 10);
    
    // Build destination path: /inbox/YYYY/MM/DD/filename
    String destDir = "/inbox/" + year + "/" + month + "/" + day;
    String destPath = destDir + "/" + filename;
    
    // Create directory structure
    if (!storage_ensureDirectoryPath(destDir.c_str())) {
        logErrorf("[UploadQueue] Failed to create inbox directory: %s\n", destDir.c_str());
        // Fall back to deletion
        SD_MMC.remove(path);
        return String();
    }
    
    // Move file (rename)
    logDebugf("[UploadQueue] Moving file to inbox: %s -> %s", path, destPath.c_str());
    if (SD_MMC.rename(path, destPath.c_str())) {
        logDebugf("[UploadQueue] File moved successfully: %s", destPath.c_str());
        return destPath;
    }
    
    logErrorf("[UploadQueue] Failed to move file, deleting: %s\n", path);
    logDebugf("[UploadQueue] Deleting file after failed move: %s", path);
    if (SD_MMC.remove(path)) {
        logDebugf("[UploadQueue] File deleted successfully: %s", path);
    } else {
        logDebugf("[UploadQueue] Failed to delete file: %s", path);
    }
    return String();
}

bool uploadQueue_markUploaded(const char* path) {
    if (!isStorageModeSdCard()) {
        return true; // In PSRAM mode, consider it "uploaded" (already handled)
    }
    if (!lockPendingDirRenameAttempt()) {
        logDebugf("[UploadTask] skip reason=mutex_timeout (markUploaded)");
        return false;
    }
    String destPath = moveFileToInbox(path);
    const bool ok = !destPath.isEmpty() || !SD_MMC.exists(path);
    uploadQueue_unlockPendingDir();
    if (ok) {
        uploadQueue_invalidateFilesystemPendingCountCache();
    }
    return ok;
}

bool uploadQueue_markUploadedWithRecord(const char* path, const UploadIndexRecord& record) {
    if (!isStorageModeSdCard()) {
        return true; // In PSRAM mode, consider it "uploaded" (already handled)
    }
    if (!lockPendingDirRenameAttempt()) {
        logDebugf("[UploadTask] skip reason=mutex_timeout (markUploadedWithRecord)");
        return false;
    }
    String destPath = moveFileToInbox(path);
    if (destPath.isEmpty()) {
        const bool gone = !SD_MMC.exists(path);
        uploadQueue_unlockPendingDir();
        if (gone) {
            uploadQueue_invalidateFilesystemPendingCountCache();
        }
        return gone;
    }

    // Release /pending mutex before index I/O: append writes /inbox/.../index.json and can be slow on SD;
    // holding g_pendingDirMutex here blocked the recorder and caused "Failed to lock /pending directory".
    uploadQueue_unlockPendingDir();
    uploadQueue_appendToIndex(destPath.c_str(), record);
    uploadQueue_invalidateFilesystemPendingCountCache();
    return true;
}

// Append a record to the daily index.json file (JSONL format - one JSON per line)
// Called after a file is successfully uploaded and moved to inbox
bool uploadQueue_appendToIndex(const char* inboxPath, const UploadIndexRecord& record)
{
    if (inboxPath == nullptr || strlen(inboxPath) == 0) {
        return false;
    }
    
    if (!isStorageModeSdCard()) {
        return false;
    }
    
    // Extract date from path: /inbox/YYYY/MM/DD/filename.wav
    int year = 0, month = 0, day = 0;
    if (sscanf(inboxPath, "/inbox/%d/%d/%d/", &year, &month, &day) != 3) {
        return false;
    }
    
    // Build index file path: /inbox/YYYY/MM/DD/index.json
    char indexPath[64];
    snprintf(indexPath, sizeof(indexPath), "/inbox/%04d/%02d/%02d/index.json", year, month, day);
    
    // Build single-line JSON record
    StaticJsonDocument<1024> doc;
    
    // Extract filename from path
    String pathStr = inboxPath;
    int lastSlash = pathStr.lastIndexOf('/');
    String filename = (lastSlash >= 0) ? pathStr.substring(lastSlash + 1) : pathStr;
    
    doc["filename"] = filename;
    doc["path"] = inboxPath;
    
    // Recorder info
    JsonObject recorder = doc.createNestedObject("recorder");
    recorder["id"] = getDeviceId();
    recorder["trigger"] = 1;
    if (strlen(record.endReason) > 0) {
        recorder["endReason"] = record.endReason;
    }
    recorder["duration"] = record.durationMs / 1000;
    recorder["durationMs"] = record.durationMs;
    recorder["size"] = static_cast<uint32_t>(record.fileSize);
    recorder["dataBytes"] = static_cast<uint32_t>(record.sizeBytes);
    recorder["timestamp"] = record.timestamp;
    recorder["path"] = filename;
    if (record.peakDb > -120.0f) {
        recorder["decibel"] = record.peakDb;
    }
    
    // Dock info
    JsonObject dock = doc.createNestedObject("dock");
    dock["id"] = getDeviceId();
    
    // User info
    JsonObject user = doc.createNestedObject("user");
    user["name"] = getDeviceId();
    
    // Upload timestamp
    doc["uploadedAt"] = record.uploadedAtTimestamp;
    doc["uploadedAtEpoch"] = static_cast<unsigned long>(record.uploadedAtEpoch);
    
    // Append to file (each record is one line)
    File indexFile = SD_MMC.open(indexPath, FILE_APPEND);
    if (!indexFile) {
        logErrorf("[UploadQueue] Failed to open index file for appending: %s\n", indexPath);
        return false;
    }
    
    serializeJson(doc, indexFile);
    indexFile.println(); // Newline after each JSON record
    indexFile.close();
    
    return true;
}

size_t uploadQueue_getPendingCount() {
    if (!isStorageModeSdCard()) {
        return 0;
    }
    if (shouldDeferPendingFilesystemScan()) {
        const size_t cached = getCachedFilesystemPendingCountIfValid();
        return (cached != SIZE_MAX) ? cached : 0;
    }
    if (!sdCardMemoryQueue_isEmpty()) {
        const size_t cached = getCachedFilesystemPendingCountIfValid();
        return (cached != SIZE_MAX) ? cached : 0;
    }
    const unsigned long now = millis();
    const size_t cachedNow = getCachedFilesystemPendingCountIfValid();
    if (cachedNow != SIZE_MAX) {
        return cachedNow;
    }
    if (!SD_MMC.exists(kPendingDir)) {
        g_fsPendingWavCountCachedResult = 0;
        g_fsPendingWavCountCacheStampMs.store(now, std::memory_order_release);
        return 0;
    }
    size_t count = 0;
    uint32_t iterationCount = 0;
    scanPendingCountWav(String(kPendingDir), count, iterationCount);
    g_fsPendingWavCountCachedResult = count;
    g_fsPendingWavCountCacheStampMs.store(now, std::memory_order_release);
    return count;
}

bool uploadQueue_fileExists(const char* path) {
    if (path == nullptr || strlen(path) == 0) {
        return false;
    }
    if (!isStorageModeSdCard()) {
        return false;
    }
    return SD_MMC.exists(path);
}

// Helper function to move a single .tmp file to trash
// Returns true if file was processed, false if no file found or file is currently being recorded
static bool moveOneTempFileToTrash() {
    if (!isStorageModeSdCard()) {
        return false;
    }
    if (shouldDeferPendingFilesystemScan()) {
        return false;
    }
    if (!SD_MMC.exists(kPendingDir)) {
        logDebugf("[UploadQueue] /pending directory does not exist, skipping .tmp cleanup");
        return false;
    }
    String currentRecordingPath = recorder_getCurrentRecordingPath();
    if (!currentRecordingPath.isEmpty()) {
        logDebugf("[UploadQueue] Current recording active: %s (will skip this file)", currentRecordingPath.c_str());
    }
    logDebugf("[UploadQueue] Scanning /pending tree for .tmp files to cleanup");
    String foundPath;
    size_t tmpFileCount = 0;
    uint32_t iterationCount = 0;
    if (!findFirstTmpPending(String(kPendingDir), currentRecordingPath, foundPath, tmpFileCount, iterationCount)) {
        logDebugf("[UploadQueue] No .tmp files to cleanup (found %zu .tmp files, all skipped or none exist)", tmpFileCount);
        return false;
    }

    if (!lockPendingDirRenameAttempt()) {
        return false;
    }
    
    logDebugf("[UploadQueue] Processing .tmp file for cleanup: %s", foundPath.c_str());
    
    // Extract filename from path
    int lastSlash = foundPath.lastIndexOf('/');
    String filename = (lastSlash >= 0) ? foundPath.substring(lastSlash + 1) : foundPath;
    
    // Try to parse date from filename: YYYY-MM-DD-HH-MM-SS.tmp
    String year, month, day;
    if (filename.length() >= 19) {
        year = filename.substring(0, 4);
        month = filename.substring(5, 7);
        day = filename.substring(8, 10);
    } else {
        // Fallback: use current date if filename doesn't match expected format
        time_t now = 0;
        time(&now);
        if (now > 0) {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            char yearStr[8], monthStr[8], dayStr[8];
            snprintf(yearStr, sizeof(yearStr), "%04d", timeinfo.tm_year + 1900);
            snprintf(monthStr, sizeof(monthStr), "%02d", timeinfo.tm_mon + 1);
            snprintf(dayStr, sizeof(dayStr), "%02d", timeinfo.tm_mday);
            year = String(yearStr);
            month = String(monthStr);
            day = String(dayStr);
        } else {
            // Last resort: use "unknown" date
            year = "unknown";
            month = "01";
            day = "01";
        }
    }
    
    // Build destination path: /trash/YYYY/MM/DD/filename.tmp
    String destDir = String(kTrashDir) + "/" + year + "/" + month + "/" + day;
    String destPath = destDir + "/" + filename;
    
    // Create directory structure
    if (!storage_ensureDirectoryPath(destDir.c_str())) {
        logErrorf("[UploadQueue] Failed to create trash directory: %s\n", destDir.c_str());
        SD_MMC.remove(foundPath);
        uploadQueue_unlockPendingDir();
        return true;
    }
    
    logDebugf("[UploadQueue] Moving .tmp file to trash: %s -> %s", foundPath.c_str(), destPath.c_str());
    if (SD_MMC.rename(foundPath, destPath.c_str())) {
        logInfof("[UploadQueue] Successfully moved .tmp file to trash: %s -> %s", foundPath.c_str(), destPath.c_str());
        uploadQueue_unlockPendingDir();
        return true;
    } else {
        logErrorf("[UploadQueue] Failed to move .tmp file to trash, deleting instead: %s\n", foundPath.c_str());
        if (SD_MMC.remove(foundPath)) {
            logDebugf("[UploadQueue] Deleted .tmp file: %s", foundPath.c_str());
        } else {
            logErrorf("[UploadQueue] Failed to delete .tmp file: %s", foundPath.c_str());
        }
        uploadQueue_unlockPendingDir();
        return true;
    }
}

// Clean up ONE .tmp file by moving it to /trash/YYYY/MM/DD/
// Returns true if a file was processed, false if no files to process
bool uploadQueue_cleanupOneTempFile() {
    return moveOneTempFileToTrash();
}

// Clean up .tmp files by moving them to /trash/YYYY/MM/DD/
// Processes files one at a time with 10 second delays to avoid hanging
bool uploadQueue_cleanupTempFiles() {
    if (!isStorageModeSdCard()) {
        return true; // Not applicable in PSRAM mode
    }
    
    if (!SD_MMC.exists(kPendingDir)) {
        return true; // No pending directory, nothing to clean
    }
    
    size_t movedCount = 0;
    constexpr uint32_t kCleanupDelayMs = 10000; // 10 seconds between files
    
    // Process files one at a time with delays
    while (true) {
        if (moveOneTempFileToTrash()) {
            movedCount++;
            // Wait 10 seconds before processing next file
            delay(kCleanupDelayMs);
        } else {
            // No more files to process
            break;
        }
    }
    
    if (movedCount > 0) {
        logInfof("[UploadQueue] Cleaned up %zu .tmp file(s) from /pending", movedCount);
    }
    
    return true;
}
