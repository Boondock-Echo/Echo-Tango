#include "health.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <cstring>
#include <time.h>
#include <sys/time.h>

#include "common.h"
#include "network.h"
#include "logger.h"
#include "timekeeper.h"
#include "config.h"
#include "upload_queue.h"
#include "settings.h"
#include "main.h"
#include "networkHandller.h"

// Global metrics
SystemHealthMetrics g_healthMetrics = {};

// External task handles (defined in main.cpp)
extern TaskHandle_t recordTaskHandle;
extern TaskHandle_t serialTaskHandle;
extern TaskHandle_t maintenanceTaskHandle;
extern TaskHandle_t webServerTaskHandle;

// External task functions (defined in main.cpp)
extern void recordTask(void *pvParameters);
extern void serialTask(void *pvParameters);
extern void webServerTask(void *pvParameters);

// External metrics (defined in network.cpp)
extern MutexMetrics g_mutexMetrics;
extern NetworkQualityMetrics g_networkQualityMetrics;
extern StorageHealthMetrics g_storageHealthMetrics;
extern EndpointHealthMetrics g_endpointHealthMetrics[];

// Update task health metrics for a specific task
void updateTaskHealthMetrics(TaskHealthMetrics& metrics, TaskHandle_t taskHandle, const char* taskName, uint32_t allocatedStackSize)
{
    if (taskHandle == nullptr)
    {
        metrics.isRunning = false;
        return;
    }
    
    metrics.taskHandle = taskHandle;
    metrics.taskName = taskName;
    metrics.allocatedStackSize = allocatedStackSize;
    metrics.isRunning = (eTaskGetState(taskHandle) != eDeleted);
    
    if (metrics.isRunning)
    {
        UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(taskHandle);
        uint32_t freeStackBytes = highWaterMark * sizeof(StackType_t);
        uint32_t usedStackBytes = allocatedStackSize - freeStackBytes;
        
        metrics.currentFreeStack = freeStackBytes;
        
        if (freeStackBytes < metrics.minFreeStack)
        {
            metrics.minFreeStack = freeStackBytes;
        }
        
        if (allocatedStackSize > 0)
        {
            metrics.stackUtilizationPercent = (static_cast<float>(usedStackBytes) / static_cast<float>(allocatedStackSize)) * 100.0f;
        }
        
        metrics.lastCheckMs = millis();
    }
}

TaskHealthMetrics health_getTaskHealthMetrics(const char* taskName)
{
    TaskHealthMetrics empty = {};
    
    if (taskName == nullptr)
    {
        return empty;
    }
    
    if (std::strcmp(taskName, "RecordTask") == 0)
    {
        return g_healthMetrics.recordTaskHealth;
    }
    else if (std::strcmp(taskName, "UploadTask") == 0 || std::strcmp(taskName, "NetworkTask") == 0)
    {
        return g_healthMetrics.uploadTaskHealth;
    }
    else if (std::strcmp(taskName, "SerialTask") == 0)
    {
        return g_healthMetrics.serialTaskHealth;
    }
    else if (std::strcmp(taskName, "MaintenanceTask") == 0)
    {
        return g_healthMetrics.maintenanceTaskHealth;
    }
    else if (std::strcmp(taskName, "WebServer") == 0)
    {
        return g_healthMetrics.webServerTaskHealth;
    }
    
    return empty;
}

MutexMetrics health_getMutexMetrics()
{
    return g_mutexMetrics;
}

NetworkQualityMetrics health_getNetworkQualityMetrics()
{
    return g_networkQualityMetrics;
}

StorageHealthMetrics health_getStorageHealthMetrics()
{
    storage_updateHealthMetrics();
    return g_storageHealthMetrics;
}

EndpointHealthMetrics health_getEndpointHealthMetrics(size_t endpointIndex)
{
    if (endpointIndex >= kApiEndpointCount)
    {
        EndpointHealthMetrics empty = {};
        return empty;
    }
    return g_endpointHealthMetrics[endpointIndex];
}

SystemHealthMetrics health_getHealthMetrics()
{
    storage_updateHealthMetrics();
    
    g_healthMetrics.mutexMetrics = g_mutexMetrics;
    g_healthMetrics.networkQuality = g_networkQualityMetrics;
    g_healthMetrics.storageHealth = g_storageHealthMetrics;
    
    g_healthMetrics.overallUploadSuccessRate = network_getOverallUploadSuccessRate();
    g_healthMetrics.totalUploadAttempts = network_getTotalUploadAttempts();
    g_healthMetrics.totalSuccessfulUploads = network_getTotalUploadCount();

    // Aggregate API latency metrics across all endpoints
    unsigned long globalMin = UINT32_MAX;
    unsigned long globalMax = 0;
    unsigned long totalTime = 0;
    uint32_t totalRequests = 0;

    for (size_t i = 0; i < kApiEndpointCount; ++i)
    {
        const EndpointHealthMetrics &m = g_endpointHealthMetrics[i];
        if (m.totalRequests == 0)
        {
            continue;
        }

        // Only include min/max if there were successful requests (minResponseTimeMs != UINT32_MAX)
        if (m.minResponseTimeMs != UINT32_MAX && m.minResponseTimeMs < globalMin)
        {
            globalMin = m.minResponseTimeMs;
        }
        if (m.maxResponseTimeMs > globalMax)
        {
            globalMax = m.maxResponseTimeMs;
        }

        totalTime += m.totalResponseTimeMs;
        totalRequests += m.totalRequests;
    }

    if (totalRequests > 0)
    {
        g_healthMetrics.apiMinResponseTimeMs = (globalMin != UINT32_MAX) ? globalMin : 0;
        g_healthMetrics.apiMaxResponseTimeMs = globalMax;
        g_healthMetrics.apiAverageResponseTimeMs = totalTime / totalRequests;
    }
    else
    {
        g_healthMetrics.apiMinResponseTimeMs = 0;
        g_healthMetrics.apiMaxResponseTimeMs = 0;
        g_healthMetrics.apiAverageResponseTimeMs = 0;
    }
    
    return g_healthMetrics;
}

// Load yearly summary from SD card into cache
void health_loadYearlySummary()
{
    if (!isStorageModeSdCard())
    {
        return;
    }
    
    // Get current year
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    int currentYear = timeInfo->tm_year + 1900;
    
    // Build path to yearly summary
    char summaryPath[32];
    std::snprintf(summaryPath, sizeof(summaryPath), "/inbox/%04d/summary.json", currentYear);
    
    if (!SD_MMC.exists(summaryPath))
    {
        // Try to generate it first
        // Feed watchdog before potentially long operation (may iterate through 12 months)
        esp_task_wdt_reset();
        storage_updateYearlySummary(currentYear);
        if (!SD_MMC.exists(summaryPath))
        {
            return;
        }
    }
    
    File summaryFile = SD_MMC.open(summaryPath, FILE_READ);
    if (!summaryFile)
    {
        return;
    }
    
    String content = summaryFile.readString();
    summaryFile.close();
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, content);
    if (error)
    {
        return;
    }
    
    // Populate cache
    g_healthMetrics.yearlySummary.year = doc["year"] | currentYear;
    g_healthMetrics.yearlySummary.totalFiles = doc["totalFiles"] | 0;
    g_healthMetrics.yearlySummary.totalSizeBytes = doc["totalSizeBytes"] | 0ULL;
    g_healthMetrics.yearlySummary.totalDurationMs = doc["totalDurationMs"] | 0ULL;
    g_healthMetrics.yearlySummary.monthsWithRecordings = doc["monthsWithRecordings"] | 0;
    g_healthMetrics.yearlySummary.totalDaysWithRecordings = doc["totalDaysWithRecordings"] | 0;
    g_healthMetrics.yearlySummary.loaded = true;
    g_healthMetrics.yearlySummary.loadedAtMs = millis();
}

void health_resetMetrics()
{
    g_mutexMetrics = {};
    g_healthMetrics = {};
}

void health_initTaskHealth(TaskHealthMetrics& metrics, const char* taskName, TaskHandle_t handle, uint32_t stackSize)
{
    metrics.taskName = taskName;
    metrics.taskHandle = handle;
    metrics.allocatedStackSize = stackSize;
}

// Maintenance task - simplified for filesystem-based queue
void maintenanceTask(void *pvParameters)
{
    (void)pvParameters;
    
    constexpr unsigned long kHealthCheckIntervalMs = 30000;
    unsigned long lastHealthCheckMs = 0;
    
    // Health message timing
    static bool healthMessageSentOnStartup = false;
    unsigned long lastPeriodicHealthMs = 0;
    const unsigned long kHealthStartupDelayMs = THIRTY_SECONDS_MS;
    const unsigned long kPeriodicHealthIntervalMs = ONE_MINUTE_MS;
    
    // Yearly summary loading (2 minutes after WiFi connects)
    static bool yearlySummaryLoaded = false;
    static unsigned long wifiConnectedAtMs = 0;
    const unsigned long kYearlySummaryDelayMs = 2 * ONE_MINUTE_MS;  // 2 minutes

    // Settings sync state
    static bool nightlySettingsPushedToday = false;
    static int lastSettingsPushDay = -1;
    static bool settingsPulledAfterOnline = false;
    
    while (true)
    {
        unsigned long now = millis();
        
        // Feed watchdog every iteration to prevent timeout during potentially long operations
        // (network operations, file operations, etc. can take >30 seconds when WiFi is unstable)
        esp_task_wdt_reset();

        if (network_isCloudPathFullRebootDue())
        {
            system_requestGracefulReboot("cloud_path_60min");
        }
        
        // Health check (every 30 seconds)
        if (now - lastHealthCheckMs >= kHealthCheckIntervalMs)
        {
            lastHealthCheckMs = now;
            
            esp_task_wdt_reset(); // Feed watchdog at start of health check block
            network_updateRssi();
            storage_updateHealthMetrics();
            
            // Handle SD card runtime failures (retry with exponential backoff, reboot after 10 min)
            storage_handleRuntimeSdCardFailure();
            
            // Update task health metrics
            updateTaskHealthMetrics(g_healthMetrics.recordTaskHealth, recordTaskHandle, "RecordTask", 8192);
            updateTaskHealthMetrics(g_healthMetrics.uploadTaskHealth, networkTaskHandle, "NetworkTask", 16384);
            updateTaskHealthMetrics(g_healthMetrics.serialTaskHealth, serialTaskHandle, "SerialTask", kSerialTaskStackSize);
            updateTaskHealthMetrics(g_healthMetrics.maintenanceTaskHealth, maintenanceTaskHandle, "MaintenanceTask", 8192);
            updateTaskHealthMetrics(g_healthMetrics.webServerTaskHealth, webServerTaskHandle, "WebServer", 8192);
            
            esp_task_wdt_reset(); // Feed watchdog after task health updates (before potentially long operations)
            
            // Auto-restart tasks if they've stopped
            auto restartTaskIfNeeded = [&](TaskHealthMetrics& metrics, TaskHandle_t& handle, 
                                          const char* taskName, void (*taskFunc)(void*), 
                                          uint32_t stackSize, UBaseType_t priority, BaseType_t coreId) -> bool
            {
                if (handle == nullptr)
                {
                    return false;
                }
                
                eTaskState taskState = eTaskGetState(handle);
                bool isTaskRunning = (taskState != eDeleted && taskState != eInvalid);
                
                if (!isTaskRunning && metrics.isRunning)
                {
                    constexpr uint32_t kMaxTaskRestarts = 5;
                    constexpr unsigned long kRestartBackoffMs = 60000;
                    
                    unsigned long timeSinceLastRestart = now - metrics.lastRestartMs;
                    
                    if (metrics.restartCount < kMaxTaskRestarts && 
                        (metrics.lastRestartMs == 0 || timeSinceLastRestart >= kRestartBackoffMs))
                    {
                        logErrorf("[Maintenance] Task %s stopped, attempting restart\n", taskName);
                        
                        if (handle != nullptr)
                        {
                            vTaskDelete(handle);
                            handle = nullptr;
                        }
                        
                        BaseType_t result = xTaskCreatePinnedToCore(
                            taskFunc, taskName, stackSize, nullptr, priority, &handle, coreId);
                        
                        if (result == pdPASS)
                        {
                            // WebServer is not on TWDT (see main.cpp) — same for restarts
                            if (strcmp(taskName, "WebServer") != 0)
                            {
                                esp_task_wdt_add(handle);
                            }
                            metrics.restartCount++;
                            metrics.lastRestartMs = now;
                            metrics.taskHandle = handle;
                            return true;
                        }
                    }
                }
                
                return false;
            };

            restartTaskIfNeeded(g_healthMetrics.recordTaskHealth, recordTaskHandle,
                                "RecordTask", recordTask, 8192, 2, 1);
            restartTaskIfNeeded(g_healthMetrics.uploadTaskHealth, networkTaskHandle,
                              "NetworkTask", networkTask, 16384, 1, 0);
            restartTaskIfNeeded(g_healthMetrics.serialTaskHealth, serialTaskHandle,
                                "SerialTask", serialTask, kSerialTaskStackSize, 3, 0);
            restartTaskIfNeeded(g_healthMetrics.webServerTaskHealth, webServerTaskHandle, 
                              "WebServer", webServerTask, 8192, 2, 1);
            
            // Evict oldest inbox day when SD is >70% full; refresh metrics and capacity logs
            if (isStorageModeSdCard() && timeKeeper().timeIsValid())
            {
                if (g_storageHealthMetrics.utilizationPercent >= 80.0f)
                {
                    esp_task_wdt_reset();
                    Serial.println("[Maintenance] Storage >80% full, attempting to delete oldest folder");
                    storage_deleteOldestFolderIfNeeded();
                    esp_task_wdt_reset();
                }

                esp_task_wdt_reset();
                storage_updateHealthMetrics();
                storage_checkCapacityAlerts();
                esp_task_wdt_reset();
            }

            // Only retry mount if not permanently disabled
            if (!g_storageHealthMetrics.mountStable && !isSdCardPermanentlyDisabled())
            {
                esp_task_wdt_reset(); // Feed watchdog before potentially long SD card operation
                storage_retryMount();
                esp_task_wdt_reset(); // Feed watchdog after SD card operation
            }
            
            // Check network
            static bool lastWiFiState = false;
            bool currentWiFiState = isWiFiConnected();
            if (currentWiFiState && !lastWiFiState)
            {
                g_healthMetrics.networkReconnectCount++;
                g_healthMetrics.lastNetworkReconnectMs = now;
                wifiConnectedAtMs = now;  // Track when WiFi connected
                // Reset "pulled" flag when WiFi reconnects; pull will happen after we're fully online
                settingsPulledAfterOnline = false;
            }
            else if (!currentWiFiState && lastWiFiState)
            {
                // WiFi disconnected - reset the "pulled" flag so we'll retry when WiFi reconnects
                settingsPulledAfterOnline = false;
                wifiConnectedAtMs = 0;  // Reset WiFi connection timestamp
            }
            lastWiFiState = currentWiFiState;
            
            // Load yearly summary ~2 minutes after WiFi connects
            if (!yearlySummaryLoaded && currentWiFiState && wifiConnectedAtMs > 0 &&
                (now - wifiConnectedAtMs >= kYearlySummaryDelayMs))
            {
                esp_task_wdt_reset(); // Feed watchdog before potentially long file operation
                health_loadYearlySummary();
                esp_task_wdt_reset(); // Feed watchdog after file operation
                yearlySummaryLoaded = true;
            }
            
            esp_task_wdt_reset(); // Before network_retryDeadEndpoints (keeps gap under TWDT budget)
            network_retryDeadEndpoints();
            esp_task_wdt_reset(); // Feed watchdog at end of health check block
        }
        
        // Health message timing (always send, not just in CLI mode)
        if (!healthMessageSentOnStartup)
        {
            if (now >= kHealthStartupDelayMs)
            {
                extern void sendHealthMessage(bool mutexAlreadyHeld = false);
                esp_task_wdt_reset(); // Feed watchdog before health message (may call health_getHealthMetrics)
                sendHealthMessage();
                esp_task_wdt_reset(); // Feed watchdog after health message
                healthMessageSentOnStartup = true;
                lastPeriodicHealthMs = now;
            }
        }
        else if (now - lastPeriodicHealthMs >= kPeriodicHealthIntervalMs)
        {
            extern void sendHealthMessage(bool mutexAlreadyHeld = false);
            esp_task_wdt_reset(); // Feed watchdog before health message (may call health_getHealthMetrics)
            sendHealthMessage();
            esp_task_wdt_reset(); // Feed watchdog after health message
            lastPeriodicHealthMs = now;
        }
        
        // Periodic cleanup (every hour)
        static unsigned long lastCleanupMs = 0;
        if (isStorageModeSdCard() && timeKeeper().timeIsValid())
        {
            if (lastCleanupMs == 0 || (now - lastCleanupMs) >= ONE_HOUR_MS)
            {
                lastCleanupMs = now;
                esp_task_wdt_reset(); // Feed watchdog before potentially very long cleanup operation
                storage_cleanupOldUploadedFiles(30);
                esp_task_wdt_reset(); // Feed watchdog after cleanup
                storage_checkCapacityAlerts();
                esp_task_wdt_reset();
                storage_pruneRecordingsSummariesWithoutInbox();
                esp_task_wdt_reset();
            }
        }
        
        // Nightly monthly summary update (runs at maintenance hour, e.g., 3 AM)
        static int lastSummaryDay = -1;
        if (timeKeeper().timeIsValid())
        {
            time_t currentTime = time(nullptr);
            struct tm* timeInfo = localtime(&currentTime);
            
            int currentDay = timeInfo->tm_mday;
            int currentHour = timeInfo->tm_hour;
            int currentMinute = timeInfo->tm_min;
            
            // Run once per day at the configured maintenance hour
            uint8_t maintHour = appSettings.timezone.maintenanceHour;
            uint8_t maintMinute = appSettings.timezone.maintenanceMinute;
            
            // Check if we're within the maintenance window (within 5 minutes of maintenance time)
            bool inMaintenanceWindow = (currentHour == maintHour && 
                                        currentMinute >= maintMinute && 
                                        currentMinute < maintMinute + 5);
            
            if (inMaintenanceWindow)
            {
                // Reset daily flags when we enter the maintenance window on a new day
                if (currentDay != lastSummaryDay)
                {
                    lastSummaryDay = currentDay;
                    nightlySettingsPushedToday = false;
                }

                // Run nightly summary update (if SD card available)
                if (isStorageModeSdCard())
                {
                    esp_task_wdt_reset(); // Feed watchdog before potentially very long summary operation
                    storage_runNightlySummaryUpdate();
                    esp_task_wdt_reset(); // Feed watchdog after summary operation
                }

                // Overnight .tmp file cleanup (process one file every 10 seconds during maintenance window)
                static unsigned long lastTempCleanupMs = 0;
                constexpr uint32_t kTempCleanupIntervalMs = 10000; // 10 seconds between files
                if (isStorageModeSdCard() && (lastTempCleanupMs == 0 || (now - lastTempCleanupMs) >= kTempCleanupIntervalMs))
                {
                    lastTempCleanupMs = now;
                    logDebugf("[Maintenance] Running overnight .tmp file cleanup (one file per 10 seconds)");
                    esp_task_wdt_reset(); // Feed watchdog before file operation
                    if (uploadQueue_cleanupOneTempFile())
                    {
                        logInfof("[Maintenance] Cleaned up one .tmp file during overnight maintenance");
                    } else {
                        logDebugf("[Maintenance] No more .tmp files to cleanup");
                    }
                    esp_task_wdt_reset(); // Feed watchdog after file operation
                }

                // Nightly settings push (once per day)
                if (!nightlySettingsPushedToday && isWiFiConnected())
                {
                    String maskedJson = settings_getMaskedJsonForServer();
                    esp_task_wdt_reset(); // Feed watchdog before potentially long network operation
                    bool ok = network_pushSettingsToServer(maskedJson);
                    esp_task_wdt_reset(); // Feed watchdog after network operation
                    if (ok)
                    {
                        nightlySettingsPushedToday = true;
                        lastSettingsPushDay = currentDay;
                    }
                    else
                    {
                    }
                }
            }
        }

        // One-time settings pull after device is fully online:
        // Only attempt once after WiFi is connected and time is valid.
        // Mark as attempted regardless of success/failure to prevent retries.
        // Also check WiFi.status() for more reliable connection detection
        if (!settingsPulledAfterOnline && isWiFiConnected() && WiFi.status() == WL_CONNECTED && timeKeeper().timeIsValid())
        {
            // Mark as attempted immediately to prevent retries
            settingsPulledAfterOnline = true;
            
            String serverJson;
            esp_task_wdt_reset(); // Feed watchdog before potentially long network operation
            if (network_pullSettingsFromServer(serverJson) && serverJson.length() > 0)
            {
                esp_task_wdt_reset(); // Feed watchdog after network operation, before settings apply
                settings_applyJsonFromServer(serverJson);
            }
            else
            {
                esp_task_wdt_reset(); // Feed watchdog after failed network operation
                // Settings pull failed - don't retry, will try again on next reboot/WiFi reconnect
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
