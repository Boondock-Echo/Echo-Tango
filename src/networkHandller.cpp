#include <Arduino.h>

#include "networkHandller.h"

#include <atomic>
#include "logger.h"
#include "network.h"
#include "upload_queue.h"
#include "common.h"
#include "config.h"
#include "esp_task_wdt.h"
#include "recorder.h"
#include "sd_bus.h"
#include <cstring>
#include "settings.h"

TaskHandle_t networkTaskHandle = nullptr;

volatile bool g_networkTaskShutdownRequested = false;

namespace {

    static volatile bool s_uploadBusy = false;
}

bool networkHandler_isUploading()
{
    return s_uploadBusy;
}

static std::atomic<bool> s_uploadPausedForLiveAudio{false};

void networkHandler_setUploadPaused(bool paused)
{
    s_uploadPausedForLiveAudio.store(paused, std::memory_order_relaxed);
}

bool networkHandler_isUploadPaused()
{
    return s_uploadPausedForLiveAudio.load(std::memory_order_relaxed);
}

void networkHandler_requestShutdown()
{
    g_networkTaskShutdownRequested = true;
}

bool handleUploadOne()
{
    static unsigned long taskStartMs = millis();
    static bool startupDelayComplete = false;

    static unsigned long lastSuccessfulUploadMs = 0;
    static unsigned long lastSustainedFailureErrorMs = 0;
    constexpr unsigned long kSustainedFailureThresholdMs = 300000;

    static char lastWarnedFilePath[kMaxUploadPathLength] = {0};
    static unsigned long nextFsFallbackScanAllowedMs = 0;
    static char fsFallbackRetryPath[kMaxUploadPathLength] = {0};
    static uint8_t fsFallbackRetryCount = 0;

    constexpr uint32_t kRetryDelayMs = 1000;
    constexpr uint8_t kUploadMaxRetries = 3;
    constexpr uint32_t kStartupDelayMs = 30000;
    constexpr unsigned long kFsFallbackEmptyScanMinIntervalMs = 5000;

    esp_task_wdt_reset();

    if (networkHandler_isUploadPaused())
    {
        logDebugf("[UploadTask] skip reason=pause_live_audio_ui");
        return false;
    }

    // 🔹 Credentials check
    if (!network_hasAnyWiFiCredentials())
    {
        logDebugf("[UploadTask] skip reason=no_credentials");
        return false;
    }

    // 🔹 WiFi check
    if (!isWiFiConnected())
    {
        logDebugf("[UploadTask] skip reason=wifi");
        esp_task_wdt_reset();
        connectToWiFi();
        return false;
    }

    // Events bootstrap the short-lived Edge token. Leave every queued upload
    // untouched until that bootstrap completes; the network task continues to
    // drain events below while uploads are on hold.
    if (!hasApiAuthToken())
    {
        logDebugf("[UploadTask] skip reason=waiting_for_api_token");
        return false;
    }

    // A dead endpoint is a cooldown state, not a new upload failure. Do not
    // dequeue/retry files or repeat old failure logs while recovery is pending.
    if (network_areAllEndpointsDead())
    {
        logDebugf("[UploadTask] skip reason=waiting_for_endpoint_recovery");
        return false;
    }

    // =========================================================
    // PSRAM MODE
    // =========================================================
    const bool shouldDrainPsramQueue =
        isStorageModePsram() || psramQueue_getPendingCount() > 0;

    if (shouldDrainPsramQueue)
    {
        size_t pendingCount = psramQueue_getPendingCount();

        if (pendingCount >= 8)
        {
            logWarnf("[UploadTask] High backlog in PSRAM queue (%u), reconnecting WiFi", pendingCount);
            network_reconnectWiFi();
        }

        PsramQueueEntry *entry = psramQueue_getNextEntry();
        if (entry == nullptr)
        {
            logDebugf("[UploadTask] skip reason=psram_queue_empty");
            return false;
        }

        s_uploadBusy = true;

        UploadRequest request = {};
        snprintf(request.path, kMaxUploadPathLength, "PSRAM_%lu",
                 static_cast<unsigned long>(entry->recordedAtEpoch));

        request.fileSize = entry->dataSize;
        request.sizeBytes = entry->dataSize > sizeof(WaveHeader)
                                ? entry->dataSize - sizeof(WaveHeader)
                                : entry->dataSize;

        request.recordedAtEpoch = entry->recordedAtEpoch;
        request.recordedAtMs = entry->recordedAtMs;
        request.durationMs = entry->durationMs;
        request.peakDb = entry->peakDb;
        request.isPsramMode = true;
        request.psramData = entry->data;
        request.attempts = 0;

        logDebugf("[UploadTask] Processing PSRAM entry: %s (size=%u)", request.path, request.fileSize);

        network_updateRssi();
        network_incrementUploadAttempt();

        esp_task_wdt_reset();

        bool uploadSuccess = uploadAudioFile(request, false);

        esp_task_wdt_reset();

        if (uploadSuccess)
        {
            logInfof("[UploadTask] PSRAM upload success: %s", request.path);

            entry->uploadRetryCount = 0;
            psramQueue_releaseEntry(entry);

            lastSuccessfulUploadMs = millis();
            lastSustainedFailureErrorMs = 0;
            lastWarnedFilePath[0] = '\0';
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTER_FILE_COOLDOWN_MS));
        }
        else
        {
            recorder_incrementErrorCount();

            logWarnf("[UploadTask] upload failed reason=%s path=%s",
                     network_getLastUploadFailureReason(), request.path);

            entry->uploadRetryCount++;
            if (entry->uploadRetryCount >= kUploadMaxRetries)
            {
                logWarnf("[UploadTask] PSRAM upload failed %u consecutive times, dropping entry: %s",
                         static_cast<unsigned>(entry->uploadRetryCount), request.path);
                psramQueue_releaseEntry(entry);
                lastWarnedFilePath[0] = '\0';
            }
            else
            {
                if (strcmp(request.path, lastWarnedFilePath) != 0)
                {
                    logWarnf("[UploadTask] PSRAM upload failed (will retry %u/%u)",
                             static_cast<unsigned>(entry->uploadRetryCount),
                             static_cast<unsigned>(kUploadMaxRetries));
                    strncpy(lastWarnedFilePath, request.path, kMaxUploadPathLength - 1);
                    lastWarnedFilePath[kMaxUploadPathLength - 1] = '\0';
                }

                unsigned long now = millis();

                if (lastSuccessfulUploadMs > 0)
                {
                    unsigned long diff = now - lastSuccessfulUploadMs;

                    if (diff >= kSustainedFailureThresholdMs)
                    {
                        if (lastSustainedFailureErrorMs == 0 ||
                            (now - lastSustainedFailureErrorMs) >= kSustainedFailureThresholdMs)
                        {
                            logErrorf("[UploadTask] Sustained upload failure: No success for %lu minutes",
                                      diff / 60000);
                            lastSustainedFailureErrorMs = now;
                        }
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }

        s_uploadBusy = false;
        return true;
    }

    // =========================================================
    //  SD CARD MODE
    // =========================================================

    if (!isStorageModeSdCard())
    {
        logDebugf("[UploadTask] skip reason=sd_unavailable");
        return false;
    }

    unsigned long now = millis();

    if (!startupDelayComplete)
    {
        unsigned long elapsed = now - taskStartMs;

        if (elapsed < kStartupDelayMs)
        {
            logDebugf("[UploadTask] Startup delay active (%lu/%lu ms), deferring SD upload_list drain",
                      elapsed, kStartupDelayMs);
            logDebugf("[UploadTask] skip reason=startup_delay");
            return false;
        }

        startupDelayComplete = true;
        logInfof("[UploadTask] Startup delay complete, filesystem fallback enabled");
    }

    if (now < nextFsFallbackScanAllowedMs)
    {
        logDebugf("[UploadTask] upload_list scan deferred");
        return false;
    }

    logDebugf("[UploadTask] Checking per-day upload_list (newest day first)");

    const char *skipPath = (fsFallbackRetryPath[0] != '\0') ? fsFallbackRetryPath : nullptr;
    String filePath = uploadQueue_getNextFile(skipPath);

    if (filePath.isEmpty())
    {
        if (skipPath != nullptr)
        {
            logDebugf("[UploadTask] No alternate pending file after skipping exhausted retries: %s", skipPath);
            fsFallbackRetryPath[0] = '\0';
            fsFallbackRetryCount = 0;
        }
        // A miss during an active recording is often a /pending lock fail, not an empty list.
        // Do not apply the long empty-scan backoff or uploads stall for the whole clip.
        if (!recorder_isRecording())
        {
            nextFsFallbackScanAllowedMs = now + kFsFallbackEmptyScanMinIntervalMs;
        }
        logDebugf("[UploadTask] skip reason=queue_empty");
        return false;
    }

    if (fsFallbackRetryPath[0] != '\0' && strcmp(filePath.c_str(), fsFallbackRetryPath) != 0)
    {
        fsFallbackRetryPath[0] = '\0';
        fsFallbackRetryCount = 0;
    }

    nextFsFallbackScanAllowedMs = 0;

    logInfof("[UploadTask] Processing SD card file: %s", filePath.c_str());

    s_uploadBusy = true;

    sd_bus::SdFile audioFile = sd_bus::open(filePath, FILE_READ);
    if (!audioFile)
    {
        storage_recordReadError();
        logErrorf("[UploadTask] Failed to open file: %s", filePath.c_str());
        s_uploadBusy = false;
        return false;
    }

    size_t fileSize = audioFile.size();
    audioFile.close();

    time_t recordedAtEpoch = 0;
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    unsigned long sfx = 0;
    const char *base = strrchr(filePath.c_str(), '/');
    const char *namePtr = base ? base + 1 : filePath.c_str();
    if (std::sscanf(namePtr, "%d-%d-%d-%d-%d-%d_%lu.wav", &y, &mo, &d, &hh, &mm, &ss, &sfx) == 7)
    {
        char isoBuf[32];
        std::snprintf(isoBuf, sizeof(isoBuf), "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, hh, mm, ss);
        long micro = 0;
        parseIsoTimestampToEpoch(String(isoBuf), recordedAtEpoch, micro);
    }
    else if (std::sscanf(namePtr, "%d-%d-%d-%d-%d-%d.wav", &y, &mo, &d, &hh, &mm, &ss) == 6)
    {
        char isoBuf[32];
        std::snprintf(isoBuf, sizeof(isoBuf), "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, hh, mm, ss);
        long micro = 0;
        parseIsoTimestampToEpoch(String(isoBuf), recordedAtEpoch, micro);
    }

    UploadRequest request = {};
    request.recordedAtEpoch = recordedAtEpoch;
    request.recordedAtMs = 0;
    request.attempts = 0;
    filePath.toCharArray(request.path, kMaxUploadPathLength);
    request.fileSize = fileSize;
    request.sizeBytes = fileSize > sizeof(WaveHeader)
                            ? fileSize - sizeof(WaveHeader)
                            : fileSize;
    if (appSettings.audio.sampleRate > 0)
    {
        request.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(request.sizeBytes) * 1000ULL) /
                                                   (appSettings.audio.sampleRate * sizeof(int16_t)));
    }
    request.isPsramMode = false;

    network_updateRssi();
    network_incrementUploadAttempt();

    esp_task_wdt_reset();

    bool uploadSuccess = uploadAudioFile(request, false);

    esp_task_wdt_reset();

    if (uploadSuccess)
    {
        logInfof("[UploadTask] SD upload success: %s", request.path);

        if (!uploadQueue_markUploaded(request.path))
        {
            logWarnf("[UploadTask] Failed to move file to /inbox: %s", request.path);
        }

        fsFallbackRetryPath[0] = '\0';
        fsFallbackRetryCount = 0;
        lastWarnedFilePath[0] = '\0';
        vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTER_FILE_COOLDOWN_MS));
    }
    else
    {
        recorder_incrementErrorCount();

        logWarnf("[UploadTask] upload failed reason=%s path=%s",
                 network_getLastUploadFailureReason(), request.path);

        if (strcmp(request.path, fsFallbackRetryPath) == 0 || fsFallbackRetryPath[0] == '\0')
        {
            strncpy(fsFallbackRetryPath, request.path, kMaxUploadPathLength - 1);
            fsFallbackRetryPath[kMaxUploadPathLength - 1] = '\0';
            fsFallbackRetryCount++;
        }
        else
        {
            strncpy(fsFallbackRetryPath, request.path, kMaxUploadPathLength - 1);
            fsFallbackRetryPath[kMaxUploadPathLength - 1] = '\0';
            fsFallbackRetryCount = 1;
        }

        if (fsFallbackRetryCount >= kUploadMaxRetries)
        {
            logWarnf("[UploadTask] SD filesystem upload failed %u consecutive times, skipping file for now: %s",
                     static_cast<unsigned>(fsFallbackRetryCount), request.path);
            fsFallbackRetryCount = 0;
            lastWarnedFilePath[0] = '\0';
        }
        else if (strcmp(request.path, lastWarnedFilePath) != 0)
        {
            logWarnf("[UploadTask] Upload failed (will retry %u/%u): %s",
                     static_cast<unsigned>(fsFallbackRetryCount),
                     static_cast<unsigned>(kUploadMaxRetries),
                     request.path);
            strncpy(lastWarnedFilePath, request.path, kMaxUploadPathLength - 1);
            lastWarnedFilePath[kMaxUploadPathLength - 1] = '\0';
        }

        vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
    }

    s_uploadBusy = false;
    return true;
}


void networkTask(void *pvParameters)
{
    (void)pvParameters;
    while (!g_networkTaskShutdownRequested)
    {
        esp_task_wdt_reset();
        const bool uploadDidWork = handleUploadOne();
        if (!uploadDidWork)
        {
            network_drainEventsUntilMillis(millis() + 120);
        }
        // While uploading, defer event TCP so POST body is the only active cloud socket.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    networkTaskHandle = nullptr;
    vTaskDelete(nullptr);
}


/* Create network task on Core 0 */
void networkHandler_init()
{
    if (networkTaskHandle != nullptr)
    {
        return;
    }

    g_networkTaskShutdownRequested = false;

    BaseType_t result = xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        16384,
        nullptr,
        2,
        &networkTaskHandle,
        0);

    if (result != pdPASS)
    {
        logErrorf("[Startup] Failed to create networkTask");
        networkTaskHandle = nullptr;
        return;
    }

    esp_task_wdt_add(networkTaskHandle);
}