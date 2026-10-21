#include "upload_queue.h"
#include "network.h"
#include "networkHandller.h"
#include "logger.h"
#include "common.h"
#include "timekeeper.h"
#include "recorder.h"
#include "sd_bus.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
}

// Mutexes for recorder/upload task synchronization (created in *_begin, never exposed)
static SemaphoreHandle_t g_psramMutex = nullptr;
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
    if (!sd_bus::exists(kPendingDir)) {
        if (!sd_bus::mkdir(kPendingDir)) {
            logErrorf("[UploadQueue] Failed to create %s directory\n", kPendingDir);
            return false;
        }
    }
    // Create /trash directory if it doesn't exist
    if (!sd_bus::exists(kTrashDir)) {
        if (!sd_bus::mkdir(kTrashDir)) {
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

// Do not walk /pending while an upload is already in progress (same task can nest; health must not).
// Recording is allowed: pickNextFromDayList only returns existing .wav names, never the open .tmp.
static bool shouldDeferPendingFilesystemScan()
{
    return networkHandler_isUploading();
}

static bool shouldDeferTempFileCleanup()
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

static bool dayDirFromPendingWavPath(const char* pathOrName, char* dayDir, size_t dayDirLen)
{
    if (pathOrName == nullptr || dayDir == nullptr || dayDirLen < 24)
    {
        return false;
    }
    const char* slash = strrchr(pathOrName, '/');
    const char* base = slash ? slash + 1 : pathOrName;
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    unsigned long sfx = 0;
    if (std::sscanf(base, "%d-%d-%d-%d-%d-%d_%lu.wav", &y, &mo, &d, &hh, &mm, &ss, &sfx) != 7 &&
        std::sscanf(base, "%d-%d-%d-%d-%d-%d.wav", &y, &mo, &d, &hh, &mm, &ss) != 6)
    {
        return false;
    }
    std::snprintf(dayDir, dayDirLen, "/pending/%04d/%02d/%02d", y, mo, d);
    return true;
}

static bool listPathForPendingWav(const char* pathOrName, char* listPath, size_t listPathLen)
{
    char dayDir[40];
    if (!dayDirFromPendingWavPath(pathOrName, dayDir, sizeof(dayDir)))
    {
        return false;
    }
    std::snprintf(listPath, listPathLen, "%s/%s", dayDir, kUploadListFileName);
    return true;
}

static bool pendingFullPathFromBasename(const char* basename, char* out, size_t outLen)
{
    char dayDir[40];
    if (basename == nullptr || out == nullptr || !dayDirFromPendingWavPath(basename, dayDir, sizeof(dayDir)))
    {
        return false;
    }
    std::snprintf(out, outLen, "%s/%s", dayDir, basename);
    return true;
}

static bool readLineFromSd(sd_bus::SdFile& file, char* buf, size_t bufLen)
{
    if (buf == nullptr || bufLen == 0)
    {
        return false;
    }
    size_t n = 0;
    while (file.available() > 0)
    {
        const int c = file.read();
        if (c < 0)
        {
            break;
        }
        if (c == '\r')
        {
            continue;
        }
        if (c == '\n')
        {
            buf[n] = '\0';
            return true;
        }
        if (n + 1 < bufLen)
        {
            buf[n++] = static_cast<char>(c);
        }
    }
    if (n > 0)
    {
        buf[n] = '\0';
        return true;
    }
    return false;
}

static void trimInPlace(char* s)
{
    if (s == nullptr)
    {
        return;
    }
    char* start = s;
    while (*start == ' ' || *start == '\t')
    {
        ++start;
    }
    if (start != s)
    {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
    {
        s[--len] = '\0';
    }
}

static bool uploadList_appendBasename(const char* listPath, const char* basename)
{
    if (listPath == nullptr || basename == nullptr || basename[0] == '\0')
    {
        return false;
    }
    sd_bus::SdFile listFile = sd_bus::open(listPath, FILE_APPEND);
    if (!listFile)
    {
        logErrorf("[UploadList] Failed to append %s to %s\n", basename, listPath);
        return false;
    }
    listFile.println(basename);
    listFile.close();
    return true;
}

static uint32_t uploadList_readIndex(const char* dayDir)
{
    if (dayDir == nullptr || dayDir[0] == '\0')
    {
        return 0;
    }
    char idxPath[64];
    std::snprintf(idxPath, sizeof(idxPath), "%s/%s", dayDir, kUploadListIndexFileName);
    if (!sd_bus::exists(idxPath))
    {
        return 0;
    }
    sd_bus::SdFile idxFile = sd_bus::open(idxPath, FILE_READ);
    if (!idxFile)
    {
        return 0;
    }
    char buf[16];
    uint32_t index = 0;
    if (readLineFromSd(idxFile, buf, sizeof(buf)))
    {
        index = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
    }
    idxFile.close();
    return index;
}

static bool uploadList_writeIndex(const char* dayDir, uint32_t index)
{
    if (dayDir == nullptr || dayDir[0] == '\0')
    {
        return false;
    }
    char idxPath[64];
    std::snprintf(idxPath, sizeof(idxPath), "%s/%s", dayDir, kUploadListIndexFileName);
    sd_bus::SdFile idxFile = sd_bus::open(idxPath, FILE_WRITE);
    if (!idxFile)
    {
        logErrorf("[UploadList] Failed to write index %lu to %s\n", static_cast<unsigned long>(index), idxPath);
        return false;
    }
    idxFile.printf("%lu\n", static_cast<unsigned long>(index));
    idxFile.close();
    return true;
}

static bool uploadList_advancePastBasename(const char* dayDir, const char* basename)
{
    if (dayDir == nullptr || dayDir[0] == '\0' || basename == nullptr || basename[0] == '\0')
    {
        return false;
    }
    char listPath[64];
    std::snprintf(listPath, sizeof(listPath), "%s/%s", dayDir, kUploadListFileName);
    if (!sd_bus::exists(listPath))
    {
        return true;
    }

    const uint32_t startIndex = uploadList_readIndex(dayDir);
    sd_bus::SdFile listFile = sd_bus::open(listPath, FILE_READ);
    if (!listFile)
    {
        return false;
    }

    char line[80];
    uint32_t lineNum = 0;
    uint32_t newIndex = startIndex;
    while (readLineFromSd(listFile, line, sizeof(line)))
    {
        trimInPlace(line);
        if (lineNum >= startIndex && line[0] != '\0' && strcmp(line, basename) == 0)
        {
            newIndex = lineNum + 1;
            break;
        }
        ++lineNum;
    }
    listFile.close();

    if (newIndex > startIndex)
    {
        return uploadList_writeIndex(dayDir, newIndex);
    }
    return true;
}

static void uploadList_seedDayFromWavs(const char* dayDir)
{
    if (dayDir == nullptr || dayDir[0] == '\0')
    {
        return;
    }
    char listPath[64];
    std::snprintf(listPath, sizeof(listPath), "%s/%s", dayDir, kUploadListFileName);
    if (sd_bus::exists(listPath))
    {
        return;
    }

    sd_bus::SdFile dir = sd_bus::open(dayDir);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }

    bool wrote = false;
    while (true)
    {
        sd_bus::SdFile entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        const bool isDir = entry.isDirectory();
        const String nameStr = String(entry.name());
        entry.close();
        if (isDir || nameStr.length() == 0)
        {
            continue;
        }
        const String justName = basenameOnly(nameStr);
        if (justName.endsWith(".wav") && justName != kUploadListFileName && justName != kUploadListIndexFileName)
        {
            if (uploadList_appendBasename(listPath, justName.c_str()))
            {
                wrote = true;
            }
        }
    }
    dir.close();
    if (wrote)
    {
        uploadList_writeIndex(dayDir, 0);
        logInfof("[UploadList] Seeded %s from existing .wav files", listPath);
        uploadQueue_invalidateFilesystemPendingCountCache();
    }
}

static void findNewestDayDir(const String& dirPath, String& newestDayDir, uint32_t& iterationCount,
                             const String& beforeDayDir = String())
{
    if (!lockPendingDirScanAttempt())
    {
        return;
    }
    sd_bus::SdFile dir = sd_bus::open(dirPath);
    uploadQueue_unlockPendingDir();
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        return;
    }

    bool sawChildDir = false;
    bool sawWav = false;
    bool sawList = false;
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
        sd_bus::SdFile entry = dir.openNextFile();
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
            sawChildDir = true;
            findNewestDayDir(fullPath, newestDayDir, iterationCount, beforeDayDir);
        }
        else
        {
            const String justName = basenameOnly(nameStr);
            if (justName == kUploadListFileName)
            {
                sawList = true;
            }
            else if (justName.endsWith(".wav"))
            {
                sawWav = true;
            }
        }
    }
    dir.close();

    if (!sawChildDir && (sawList || sawWav))
    {
        if (!sawList && sawWav)
        {
            uploadList_seedDayFromWavs(dirPath.c_str());
        }
        if (!beforeDayDir.isEmpty() && dirPath >= beforeDayDir)
        {
            return;
        }
        if (newestDayDir.isEmpty() || dirPath > newestDayDir)
        {
            char listPath[64];
            std::snprintf(listPath, sizeof(listPath), "%s/%s", dirPath.c_str(), kUploadListFileName);
            if (sd_bus::exists(listPath))
            {
                newestDayDir = dirPath;
            }
        }
    }
}

static String pickNextFromDayList(const char* dayDir, const char* skipFullPath)
{
    char listPath[64];
    std::snprintf(listPath, sizeof(listPath), "%s/%s", dayDir, kUploadListFileName);
    if (!sd_bus::exists(listPath))
    {
        return String();
    }

    uint32_t lineIndex = uploadList_readIndex(dayDir);
    sd_bus::SdFile listFile = sd_bus::open(listPath, FILE_READ);
    if (!listFile)
    {
        return String();
    }

    char line[80];
    String chosen;
    uint32_t lineNum = 0;
    while (readLineFromSd(listFile, line, sizeof(line)))
    {
        const uint32_t thisLine = lineNum++;
        trimInPlace(line);
        if (thisLine < lineIndex || line[0] == '\0' || !strstr(line, ".wav"))
        {
            continue;
        }
        char fullPath[kMaxUploadPathLength];
        if (!pendingFullPathFromBasename(line, fullPath, sizeof(fullPath)))
        {
            std::snprintf(fullPath, sizeof(fullPath), "%s/%s", dayDir, line);
        }
        if (skipFullPath != nullptr && skipFullPath[0] != '\0' && strcmp(fullPath, skipFullPath) == 0)
        {
            continue;
        }
        if (!sd_bus::exists(fullPath))
        {
            if (lockPendingDirRenameAttempt())
            {
                uploadList_writeIndex(dayDir, thisLine + 1);
                uploadQueue_unlockPendingDir();
                uploadQueue_invalidateFilesystemPendingCountCache();
                lineIndex = thisLine + 1;
            }
            continue;
        }
        chosen = String(fullPath);
        break;
    }
    listFile.close();
    return chosen;
}

bool uploadList_addPending(const char* fullWavPath)
{
    if (fullWavPath == nullptr || fullWavPath[0] == '\0' || !isStorageModeSdCard())
    {
        return false;
    }
    char basename[64];
    copyBasenameFromFullPath(fullWavPath, basename, sizeof(basename));
    char listPath[80];
    if (basename[0] == '\0' || !listPathForPendingWav(fullWavPath, listPath, sizeof(listPath)))
    {
        logWarnf("[UploadList] Cannot derive list path for %s", fullWavPath);
        return false;
    }
    if (!lockPendingDirRenameAttempt())
    {
        logWarnf("[UploadList] Failed to lock /pending to append %s", basename);
        return false;
    }
    const bool ok = uploadList_appendBasename(listPath, basename);
    uploadQueue_unlockPendingDir();
    if (ok)
    {
        logDebugf("[UploadList] Added %s to %s", basename, listPath);
        uploadQueue_invalidateFilesystemPendingCountCache();
    }
    return ok;
}

bool uploadList_removePending(const char* fullWavPath)
{
    if (fullWavPath == nullptr || fullWavPath[0] == '\0' || !isStorageModeSdCard())
    {
        return false;
    }
    char basename[64];
    copyBasenameFromFullPath(fullWavPath, basename, sizeof(basename));
    char dayDir[40];
    if (basename[0] == '\0' || !dayDirFromPendingWavPath(fullWavPath, dayDir, sizeof(dayDir)))
    {
        return false;
    }
    if (!lockPendingDirRenameAttempt())
    {
        logWarnf("[UploadList] Failed to lock /pending to remove %s", basename);
        return false;
    }
    const bool ok = uploadList_advancePastBasename(dayDir, basename);
    uploadQueue_unlockPendingDir();
    if (ok)
    {
        logDebugf("[UploadList] Advanced index past %s in %s", basename, dayDir);
        uploadQueue_invalidateFilesystemPendingCountCache();
    }
    return ok;
}

static void scanPendingCountListLines(const String& dirPath, size_t& count, uint32_t& iterationCount)
{
    if (!lockPendingDirScanAttempt())
    {
        return;
    }
    sd_bus::SdFile dir = sd_bus::open(dirPath);
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
        sd_bus::SdFile entry = dir.openNextFile();
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
            scanPendingCountListLines(fullPath, count, iterationCount);
        }
        else if (basenameOnly(nameStr) == kUploadListFileName)
        {
            const uint32_t startIndex = uploadList_readIndex(dirPath.c_str());
            sd_bus::SdFile listFile = sd_bus::open(fullPath, FILE_READ);
            if (listFile)
            {
                char line[80];
                uint32_t lineNum = 0;
                while (readLineFromSd(listFile, line, sizeof(line)))
                {
                    const uint32_t thisLine = lineNum++;
                    trimInPlace(line);
                    if (thisLine >= startIndex && line[0] != '\0')
                    {
                        count++;
                    }
                }
                listFile.close();
            }
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
    sd_bus::SdFile dir = sd_bus::open(dirPath);
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
        sd_bus::SdFile entry = dir.openNextFile();
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

// Next pending upload from per-day upload_list files (newest day first). Does not load the backlog into RAM.
String uploadQueue_getNextFile(const char* skipFullPath) {
    if (!isStorageModeSdCard()) {
        return String();
    }
    if (shouldDeferPendingFilesystemScan()) {
        logDebugf("[UploadQueue] defer upload_list read (upload already active)");
        return String();
    }
    if (!sd_bus::exists(kPendingDir)) {
        return String();
    }

    // Fast path: upload_list lives at /pending/YYYY/MM/DD/upload_list — use UTC today, no tree walk.
    time_t now = 0;
    time(&now);
    if (isEpochValid(now))
    {
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        char todayDir[40];
        std::snprintf(todayDir, sizeof(todayDir), "/pending/%04d/%02d/%02d",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        String chosen = pickNextFromDayList(todayDir, skipFullPath);
        if (!chosen.isEmpty())
        {
            logDebugf("[UploadQueue] Selected from %s/%s: %s", todayDir, kUploadListFileName, chosen.c_str());
            return chosen;
        }
    }

    String newestDay;
    uint32_t iterationCount = 0;
    findNewestDayDir(String(kPendingDir), newestDay, iterationCount);
    while (!newestDay.isEmpty()) {
        String chosen = pickNextFromDayList(newestDay.c_str(), skipFullPath);
        if (!chosen.isEmpty()) {
            logDebugf("[UploadQueue] Selected from %s/%s: %s", newestDay.c_str(), kUploadListFileName, chosen.c_str());
            return chosen;
        }
        String nextDay;
        findNewestDayDir(String(kPendingDir), nextDay, iterationCount, newestDay);
        newestDay = nextDay;
    }
    logDebugf("[UploadQueue] No pending names in per-day upload_list files");
    return String();
}

// Internal helper to move file to inbox and return destination path
static String moveFileToInbox(const char* path) {
    if (path == nullptr || strlen(path) == 0) {
        return String();
    }
    
    if (!isStorageModeSdCard()) {
        return String();
    }
    
    if (!sd_bus::exists(path)) {
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
        sd_bus::remove(path);
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
        sd_bus::remove(path);
        return String();
    }
    
    // Move file (rename)
    logDebugf("[UploadQueue] Moving file to inbox: %s -> %s", path, destPath.c_str());
    if (sd_bus::rename(path, destPath.c_str())) {
        logDebugf("[UploadQueue] File moved successfully: %s", destPath.c_str());
        return destPath;
    }
    
    logErrorf("[UploadQueue] Failed to move file, deleting: %s\n", path);
    logDebugf("[UploadQueue] Deleting file after failed move: %s", path);
    if (sd_bus::remove(path)) {
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
    const bool ok = !destPath.isEmpty() || !sd_bus::exists(path);
    if (ok) {
        char basename[64];
        copyBasenameFromFullPath(path, basename, sizeof(basename));
        char dayDir[40];
        if (basename[0] != '\0' && dayDirFromPendingWavPath(path, dayDir, sizeof(dayDir))) {
            uploadList_advancePastBasename(dayDir, basename);
        }
    }
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
    if (!destPath.isEmpty() || !sd_bus::exists(path)) {
        char basename[64];
        copyBasenameFromFullPath(path, basename, sizeof(basename));
        char dayDir[40];
        if (basename[0] != '\0' && dayDirFromPendingWavPath(path, dayDir, sizeof(dayDir))) {
            uploadList_advancePastBasename(dayDir, basename);
        }
    }
    if (destPath.isEmpty()) {
        const bool gone = !sd_bus::exists(path);
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
    sd_bus::SdFile indexFile = sd_bus::open(indexPath, FILE_APPEND);
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
        // Last known count even if the 2 s TTL was invalidated (do not report 0 during upload).
        return g_fsPendingWavCountCachedResult;
    }
    const unsigned long now = millis();
    const size_t cachedNow = getCachedFilesystemPendingCountIfValid();
    if (cachedNow != SIZE_MAX) {
        return cachedNow;
    }
    if (!sd_bus::exists(kPendingDir)) {
        g_fsPendingWavCountCachedResult = 0;
        g_fsPendingWavCountCacheStampMs.store(now, std::memory_order_release);
        return 0;
    }
    size_t count = 0;
    uint32_t iterationCount = 0;
    scanPendingCountListLines(String(kPendingDir), count, iterationCount);
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
    return sd_bus::exists(path);
}

// Helper function to move a single .tmp file to trash
// Returns true if file was processed, false if no file found or file is currently being recorded
static bool moveOneTempFileToTrash() {
    if (!isStorageModeSdCard()) {
        return false;
    }
    if (shouldDeferTempFileCleanup()) {
        return false;
    }
    if (!sd_bus::exists(kPendingDir)) {
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
        sd_bus::remove(foundPath);
        uploadQueue_unlockPendingDir();
        return true;
    }
    
    logDebugf("[UploadQueue] Moving .tmp file to trash: %s -> %s", foundPath.c_str(), destPath.c_str());
    if (sd_bus::rename(foundPath, destPath.c_str())) {
        logInfof("[UploadQueue] Successfully moved .tmp file to trash: %s -> %s", foundPath.c_str(), destPath.c_str());
        uploadQueue_unlockPendingDir();
        return true;
    } else {
        logErrorf("[UploadQueue] Failed to move .tmp file to trash, deleting instead: %s\n", foundPath.c_str());
        if (sd_bus::remove(foundPath)) {
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
    
    if (!sd_bus::exists(kPendingDir)) {
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
