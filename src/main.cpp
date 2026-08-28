#include "main.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>
#include <cstring>

#include "boondock_server.h"
#include "common.h"
#include "network.h"
#include "recorder.h"
#include "settings.h"
#include "logger.h"
#include "timekeeper.h"
#include "upload_queue.h"
#include "system_assets.h"
#include "nvs.h"
#include "health.h"
#include "networkHandller.h"
#if defined(ECHO)
#include "echo_led.h"
#include "echo_keypad.h"
#include "mqtt_task.h"
#endif

// Task handles
TaskHandle_t recordTaskHandle = nullptr;
TaskHandle_t serialTaskHandle = nullptr;
TaskHandle_t maintenanceTaskHandle = nullptr;
TaskHandle_t webServerTaskHandle = nullptr;

#if defined(ECHO)
TaskHandle_t echoLedTaskHandle = nullptr;
TaskHandle_t echoKeypadTaskHandle = nullptr;
#endif

static const char* resetReasonToString(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "Power On";
    case ESP_RST_EXT:
        return "External Reset";
    case ESP_RST_SW:
        return "Software Reset";
    case ESP_RST_PANIC:
        return "Panic";
    case ESP_RST_INT_WDT:
        return "Interrupt Watchdog Timer";
    case ESP_RST_TASK_WDT:
        return "Task Watchdog Timer";
    case ESP_RST_WDT:
        return "Other Watchdog Timer";
    case ESP_RST_DEEPSLEEP:
        return "Deep Sleep";
    case ESP_RST_BROWNOUT:
        return "Power Supply Reset";
    case ESP_RST_SDIO:
        return "SDIO Reset";
    default:
        return "reserved";
    }
}

const char* system_getResetReasonString()
{
    return resetReasonToString(esp_reset_reason());
}

namespace
{
    
    // Memory trend tracking
    struct MemoryTrend {
        uint32_t heapFreeHistory[5] = {0};
        uint32_t minFreeHeapHistory[5] = {0};
        uint32_t largestBlockHistory[5] = {0};
        size_t historyIndex = 0;
        bool historyFilled = false;
        unsigned long lastWarningMs = 0;
    };
    MemoryTrend g_memoryTrend = {};

    void logResetReason()
    {
        const esp_reset_reason_t reason = esp_reset_reason();
        logInfof("[Startup] Reset reason: %s", resetReasonToString(reason));
    }

    void cliLog(const char* level, const char* message)
    {
        StaticJsonDocument<256> doc;
        doc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
        doc["ty"] = "log";
        doc["lv"] = level;
        doc["ms"] = message;
        doc["mc"] = getDeviceId();
        doc["si"] = getSessionId();
        String output;
        serializeJson(doc, output);
        serialWriteJsonAtomic(output);
    }

} // namespace

// Forward declarations
void maintenanceTask(void *pvParameters);
void recordTask(void *pvParameters);
void webServerTask(void *pvParameters);

// Variables to track settings changes
volatile unsigned long g_lastSettingsChangeMs = 0;
volatile bool g_pendingConfigMessage = false;

void system_safetyReboot(const char* reason)
{
    logErrorf("[SafetyReboot] Memory safety reboot triggered: %s", reason);
    
    g_healthMetrics.safetyRebootCount++;
    g_healthMetrics.lastSafetyRebootMs = millis();
    
    networkHandler_requestShutdown();

    if (recordTaskHandle != nullptr)
    {
        vTaskSuspend(recordTaskHandle);
        recorder_stopActiveRecording("safety_reboot");
        vTaskDelete(recordTaskHandle);
        recordTaskHandle = nullptr;
    }

    settings_save();
    logger_flush();
    Serial.flush();
    
    delay(100);
    ESP.restart();
}

void performMemoryCleanup()
{
    logger_flush();
    Serial.flush();
    settings_save();
    heap_caps_check_integrity_all(true);
}

void checkMemorySafety()
{
    const uint32_t heapFree = ESP.getFreeHeap();
    const uint32_t minFreeHeap = ESP.getMinFreeHeap();
    const uint32_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const bool heapIntegrity = heap_caps_check_integrity_all(true);
    
    constexpr uint32_t kCriticalHeapFree = MEMORY_CRITICAL_HEAP_FREE_KB * 1024;
    constexpr uint32_t kCriticalMinFreeHeap = MEMORY_CRITICAL_MIN_FREE_HEAP_KB * 1024;
    constexpr uint32_t kCriticalLargestBlock = MEMORY_CRITICAL_LARGEST_BLOCK_KB * 1024;
    
    // Update memory trend tracking
    g_memoryTrend.heapFreeHistory[g_memoryTrend.historyIndex] = heapFree;
    g_memoryTrend.minFreeHeapHistory[g_memoryTrend.historyIndex] = minFreeHeap;
    g_memoryTrend.largestBlockHistory[g_memoryTrend.historyIndex] = largestFreeBlock;
    g_memoryTrend.historyIndex = (g_memoryTrend.historyIndex + 1) % 5;
    if (g_memoryTrend.historyIndex == 0)
    {
        g_memoryTrend.historyFilled = true;
    }
    
    if (!heapIntegrity)
    {
        performMemoryCleanup();
        system_safetyReboot("heap_corruption");
        return;
    }
    
    if (heapFree < kCriticalHeapFree)
    {
        performMemoryCleanup();
        system_safetyReboot("low_heap_free");
        return;
    }
    
    if (minFreeHeap < kCriticalMinFreeHeap)
    {
        performMemoryCleanup();
        system_safetyReboot("low_min_free_heap");
        return;
    }
    
    if (largestFreeBlock < kCriticalLargestBlock)
    {
        performMemoryCleanup();
        system_safetyReboot("severe_fragmentation");
        return;
    }
}

namespace
{
    // Helper function to output JSON to Serial directly (assumes mutex is already held or not needed)
    void outputJsonToSerialDirect(const String& jsonOutput)
    {
        if (jsonOutput.length() == 0)
        {
            return;
        }
        
        // Add newline if not present
        String messageToSend = jsonOutput;
        if (!messageToSend.endsWith("\n") && !messageToSend.endsWith("\r\n"))
        {
            messageToSend += "\n";
        }
        
        // Send entire message atomically using write() instead of print()
        // This ensures the message is sent as a single unit
        Serial.write(reinterpret_cast<const uint8_t*>(messageToSend.c_str()), messageToSend.length());
        
        // Flush to ensure message is fully transmitted
        Serial.flush();
    }
    
    // Helper function to output JSON to Serial with mutex protection
    void outputJsonToSerial(const String& jsonOutput)
    {
        if (jsonOutput.length() == 0)
        {
            return;
        }
        
        // Acquire Serial mutex to prevent interleaving with CLI command output
        // Wait up to 5 seconds to allow CLI commands to complete first
        SemaphoreHandle_t serialMutex = settings_getSerialMutex();
        bool mutexAcquired = false;
        
        if (serialMutex != nullptr)
        {
            mutexAcquired = (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(5000)) == pdTRUE);
        }
        
        // Only send if we acquired mutex or mutex is not available (fallback)
        if (mutexAcquired || serialMutex == nullptr)
        {
            outputJsonToSerialDirect(jsonOutput);
        }
        
        // Release mutex if we acquired it
        if (mutexAcquired && serialMutex != nullptr)
        {
            xSemaphoreGive(serialMutex);
        }
    }
}

void sendConfigMessage()
{
    // First message: Recorder settings
    DynamicJsonDocument recorderDoc(1024);
    recorderDoc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
    recorderDoc["ty"] = "config";
    recorderDoc["ath"] = appSettings.audio.audioThreshold;
    recorderDoc["mrm"] = appSettings.audio.minRecordingMs;
    recorderDoc["xrm"] = appSettings.audio.maxRecordingMs;
    recorderDoc["stm"] = appSettings.audio.silenceThresholdMs;
    recorderDoc["prm"] = appSettings.audio.preRecordMs;
    recorderDoc["cg"] = appSettings.audio.codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    recorderDoc["ric"] = appSettings.audio.recordInputChannel;
#endif
    recorderDoc["is"] = appSettings.audio.sampleRate;
    recorderDoc["rsc"] = appSettings.sdCard.recordToSdCard;
    recorderDoc["mc"] = getDeviceId();
    recorderDoc["si"] = getSessionId();
    
    String recorderJsonOutput;
    serializeJson(recorderDoc, recorderJsonOutput);
    
    // Send recorder config event
    DynamicJsonDocument recorderEventData(1024);
    recorderEventData["message"] = "Recorder Configuration";
    recorderEventData["config"] = recorderDoc;
    String recorderEventMessage;
    serializeJson(recorderEventData, recorderEventMessage);
    sendEvent("config", recorderEventMessage);
    
    outputJsonToSerial(recorderJsonOutput);
    
    // Second message: General settings
    DynamicJsonDocument generalDoc(1024);
    generalDoc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
    generalDoc["ty"] = "config";
    generalDoc["fw"] = FIRMWARE;
    
    if (appSettings.wifi[0].ssid && strlen(appSettings.wifi[0].ssid) > 0)
    {
        generalDoc["ss"] = String(appSettings.wifi[0].ssid);
    }
    else
    {
        generalDoc["ss"] = "";
    }
    
    generalDoc["sie"] = appSettings.wifi[0].staticIpEnabled;
    generalDoc["rte"] = appSettings.rtc.enabled;
    generalDoc["usc"] = appSettings.sdCard.useSdCard;
    generalDoc["oh"] = appSettings.timezone.offsetHours;
    generalDoc["wtp"] = appSettings.wifiTxPower;
    generalDoc["mc"] = getDeviceId();
    generalDoc["si"] = getSessionId();
    
    String generalJsonOutput;
    serializeJson(generalDoc, generalJsonOutput);
    
    // Send general config event
    DynamicJsonDocument generalEventData(1024);
    generalEventData["message"] = "General Configuration";
    generalEventData["config"] = generalDoc;
    String generalEventMessage;
    serializeJson(generalEventData, generalEventMessage);
    sendEvent("config", generalEventMessage);
    
    outputJsonToSerial(generalJsonOutput);
}

void sendHealthMessage(bool mutexAlreadyHeld)
{
    auto formatSizeString = [](int64_t bytes) -> String {
        if (bytes < 1024) {
            return String(bytes) + "B";
        } else if (bytes < 1024 * 1024) {
            double kb = static_cast<double>(bytes) / 1024.0;
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.2fK", kb);
            return String(buffer);
        } else {
            double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.2fM", mb);
            return String(buffer);
        }
    };
    
    // Acquire Serial mutex to prevent interleaving with CLI command output
    // Skip mutex acquisition if caller already holds it (e.g., from serial command handler)
    // Wait up to 5 seconds to allow CLI commands to complete first
    SemaphoreHandle_t serialMutex = settings_getSerialMutex();
    bool mutexAcquired = mutexAlreadyHeld;
    
    if (!mutexAlreadyHeld && serialMutex != nullptr)
    {
        mutexAcquired = (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(5000)) == pdTRUE);
    }
    
    // Only send if we acquired mutex or mutex is not available (fallback)
    if (!mutexAcquired && serialMutex != nullptr)
    {
        // If mutex acquisition fails after timeout, skip output to avoid indefinite blocking
        return;
    }
    
    // First message: System health (RAM, Heap, WiFi, etc.)
    DynamicJsonDocument systemDoc(1024);
    systemDoc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
    systemDoc["ty"] = "health";
    
    // Storage info
    systemDoc["st"] = "SD";
    systemDoc["sd"] = isStorageModeSdCard();
    
    // Memory info
    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapTotal = ESP.getHeapSize();
    systemDoc["ht"] = formatSizeString(static_cast<int64_t>(heapTotal));
    systemDoc["hf"] = formatSizeString(static_cast<int64_t>(heapFree));
    
    // Time validity info (keep existing field but with different name to avoid conflict)
    systemDoc["tv"] = timeKeeper().timeIsValid();
    
    // WiFi info
    systemDoc["wi"] = WiFi.isConnected();
    // Cloud path: recent TCP success to API (STA can be up while cloud is unreachable)
    systemDoc["cp"] = network_isCloudPathOk();
    if (WiFi.isConnected())
    {
        systemDoc["ip"] = WiFi.localIP().toString();
        systemDoc["ri"] = WiFi.RSSI();
    }
    else
    {
        systemDoc["ip"] = "";
        systemDoc["ri"] = 0;
    }
    
    // Uptime
    systemDoc["ut"] = static_cast<int64_t>(millis() / 1000);
    
    systemDoc["mc"] = getDeviceId();
    systemDoc["si"] = getSessionId();
    
    String systemJsonOutput;
    serializeJson(systemDoc, systemJsonOutput);
    outputJsonToSerialDirect(systemJsonOutput);
    
    // Second message: Recording health (recording stats, upload stats, etc.)
    DynamicJsonDocument recordingDoc(1024);
    recordingDoc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
    recordingDoc["ty"] = "health";
    
    // Recording and upload stats
    RecordingStats sessionStats = recorder_getSessionStats();
    recordingDoc["rc"] = sessionStats.recordingCount;
    recordingDoc["uc"] = sessionStats.uploadedCount;
    recordingDoc["pq"] = system_getUploadQueueSize();
    recordingDoc["td"] = static_cast<int>(sessionStats.totalDurationMs / 1000);
    
    // API latency metrics
    SystemHealthMetrics metrics = health_getHealthMetrics();
    recordingDoc["am"] = metrics.apiMinResponseTimeMs;
    recordingDoc["ax"] = metrics.apiMaxResponseTimeMs;
    recordingDoc["aa"] = metrics.apiAverageResponseTimeMs;
    
    // Yearly summary from SD card (if loaded)
    if (metrics.yearlySummary.loaded)
    {
        recordingDoc["yr"] = metrics.yearlySummary.year;
        recordingDoc["yf"] = metrics.yearlySummary.totalFiles;
        recordingDoc["ys"] = formatSizeString(static_cast<int64_t>(metrics.yearlySummary.totalSizeBytes));
        recordingDoc["yh"] = static_cast<uint32_t>(metrics.yearlySummary.totalDurationMs / 3600000);
        recordingDoc["ym"] = metrics.yearlySummary.monthsWithRecordings;
        recordingDoc["yd"] = metrics.yearlySummary.totalDaysWithRecordings;
    }
    
    recordingDoc["mc"] = getDeviceId();
    recordingDoc["si"] = getSessionId();
    
    String recordingJsonOutput;
    serializeJson(recordingDoc, recordingJsonOutput);
    outputJsonToSerialDirect(recordingJsonOutput);
    
    // Release mutex only if we acquired it (not if caller already held it)
    if (!mutexAlreadyHeld && mutexAcquired && serialMutex != nullptr)
    {
        xSemaphoreGive(serialMutex);
    }
}

void webServerTask(void *pvParameters)
{
    (void)pvParameters;
    
    constexpr TickType_t kActivePollDelay = pdMS_TO_TICKS(10);   // 10ms when clients are active
    constexpr TickType_t kIdlePollDelay = pdMS_TO_TICKS(100);    // 100ms when no clients (10x slower)
    uint32_t watchdogFeedCounter = 0;
    constexpr uint32_t kWatchdogFeedInterval = 20; // Feed every 20 iterations (~200ms-2s depending on delay)
    
    while (true)
    {
        // Feed watchdog BEFORE potentially blocking operations
        if (++watchdogFeedCounter >= kWatchdogFeedInterval)
        {
            esp_task_wdt_reset();
            watchdogFeedCounter = 0;
        }
        
        // Check if webserver should be active:
        // - Always active in AP mode (required for WiFi configuration)
        // - Active in main mode only if webserverEnabled setting is true
        bool isAPMode = boondock_server_isAPModeActive();
        bool shouldRunWebserver = isAPMode || appSettings.webserverEnabled;
        
        if (shouldRunWebserver)
        {
            boondock_server_loop();
            
            // Adaptive polling: use shorter delay when clients are active, longer when idle
            bool hasClient = boondock_server_hasClient();
            TickType_t delay = hasClient ? kActivePollDelay : kIdlePollDelay;
            
            vTaskDelay(delay);
        }
        else
        {
            // Webserver disabled - just feed watchdog and wait
            vTaskDelay(kIdlePollDelay);
        }
    }
}

void serialTask(void *pvParameters)
{
    (void)pvParameters;
    
    unsigned long lastShortStatusMs = 0;
    const unsigned long kShortStatusIntervalMs = THIRTY_SECONDS_MS;
    
    static bool configMessageSentOnStartup = false;
    unsigned long lastPeriodicConfigMs = 0;
    const unsigned long kPeriodicConfigIntervalMs = ONE_MINUTE_MS;
    
    uint32_t watchdogFeedCounter = 0;
    constexpr uint32_t kWatchdogFeedInterval = 100; // Feed every 100 iterations (~1 second)
    
    while (true)
    {
        // Feed watchdog BEFORE potentially blocking operations (like settings_processSerial)
        // This prevents watchdog timeout if command processing takes a long time
        if (++watchdogFeedCounter >= kWatchdogFeedInterval)
        {
            esp_task_wdt_reset();
            watchdogFeedCounter = 0;
        }
        
        settings_processSerial();

        /* Feeding watchdog again here after processing serial commands, in case there were multiple commands or a long-running command that could 
        approach the watchdog timeout. This ensures we reset the watchdog both before and after potentially time-consuming operations. */
        esp_task_wdt_reset();

        unsigned long now = millis();

#if defined(ECHO)
        // Keypad-driven message navigation playback (SD recordings).
        static unsigned long lastNavRequestMs = 0;
        constexpr unsigned long kNavCooldownMs = 300;
        if (echoKeypad_consumeNextPressed() && (now - lastNavRequestMs >= kNavCooldownMs))
        {
            recorder_requestPlayNextRecording();
            lastNavRequestMs = now;
        }
        if (echoKeypad_consumePrevPressed() && (now - lastNavRequestMs >= kNavCooldownMs))
        {
            recorder_requestPlayPrevRecording();
            lastNavRequestMs = now;
        }
#endif
        
        // Check if WiFi credentials are configured before sending periodic messages
        bool hasWiFiCredentials = network_hasAnyWiFiCredentials();
        
        // Only send config messages if WiFi credentials are configured
        if (hasWiFiCredentials)
        {
            if (!configMessageSentOnStartup)
            {
                sendConfigMessage();
                configMessageSentOnStartup = true;
                lastPeriodicConfigMs = now;
            }
            
            if (g_pendingConfigMessage && (g_lastSettingsChangeMs > 0) && (now - g_lastSettingsChangeMs >= 5000))
            {
                sendConfigMessage();
                g_pendingConfigMessage = false;
                g_lastSettingsChangeMs = 0;
                lastPeriodicConfigMs = now;
            }
            
            if (configMessageSentOnStartup && 
                (now - lastPeriodicConfigMs >= kPeriodicConfigIntervalMs))
            {
                sendConfigMessage();
                lastPeriodicConfigMs = now;
            }
        }
        
        // Only send short status messages if WiFi credentials are configured
        if (hasWiFiCredentials && (lastShortStatusMs == 0 || (now - lastShortStatusMs >= kShortStatusIntervalMs)))
        {
                // Acquire Serial mutex to prevent interleaving with CLI command output
                // Wait up to 5 seconds to allow CLI commands to complete first
                SemaphoreHandle_t serialMutex = settings_getSerialMutex();
                if (serialMutex != nullptr)
                {
                    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(5000)) == pdTRUE)
                    {
                        DynamicJsonDocument doc(512);
                        doc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
                        doc["ty"] = "short";
                        doc["rg"] = recorder_isRecording();
                        doc["ug"] = system_isUploading();
                        
                        AudioLevelStats audioStats = recorder_getAudioLevelStats();
                        doc["cd"] = static_cast<int>(audioStats.currentDb * 100.0) / 100.0;
                        doc["mi"] = static_cast<int>(audioStats.minDb * 100.0) / 100.0;
                        doc["mx"] = static_cast<int>(audioStats.maxDb * 100.0) / 100.0;
                        
                        // Move mc and si to the end
                        doc["mc"] = getDeviceId();
                        doc["si"] = getSessionId();
                        
                        String jsonOutput;
                        serializeJson(doc, jsonOutput);
                        outputJsonToSerialDirect(jsonOutput);
                        
                        xSemaphoreGive(serialMutex);
                        lastShortStatusMs = now;
                    }
                    // If mutex acquisition fails after timeout, skip this status message
                    // This prevents indefinite blocking if something goes wrong
                }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool system_rebootFromCli()
{
    cliLog("info", "Reboot command received");
    networkHandler_requestShutdown();

    if (recordTaskHandle != nullptr)
    {
        vTaskSuspend(recordTaskHandle);
        recorder_stopActiveRecording("cli_reboot");
        vTaskDelete(recordTaskHandle);
        recordTaskHandle = nullptr;
    }

    if (networkTaskHandle != nullptr)
    {
        xTaskAbortDelay(networkTaskHandle);
        const uint32_t waitStart = millis();
        while (networkTaskHandle != nullptr && (millis() - waitStart) < 5000)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (networkTaskHandle != nullptr)
        {
            vTaskDelete(networkTaskHandle);
            networkTaskHandle = nullptr;
        }
    }

    settings_save();
    logger_flush();
    Serial.flush();
    
    delay(100);
    ESP.restart();
    return true;
}

void system_requestGracefulReboot(const char* reason)
{
    const char* r = (reason != nullptr) ? reason : "graceful";
    logErrorf("[System] Graceful reboot: %s (waiting for active recording to finish)", r);
    network_clearCloudPathFullRebootDue();

    networkHandler_requestShutdown();

    const unsigned long start = millis();
    unsigned long maxWait = static_cast<unsigned long>(appSettings.audio.maxRecordingMs) + 120000UL;
    if (maxWait < 90000UL)
    {
        maxWait = 90000UL;
    }
    if (maxWait > 45UL * 60UL * 1000UL)
    {
        maxWait = 45UL * 60UL * 1000UL;
    }

    while (recorder_isRecording() && (millis() - start) < maxWait)
    {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (recorder_isRecording())
    {
        logWarnf("[System] Graceful reboot: max wait elapsed, finalizing recording");
        recorder_stopActiveRecording(r);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    settings_save();
    logger_flush();
    Serial.flush();
    delay(50);
    ESP.restart();
}

void setup()
{
    Serial.begin(115200);
    
    // Initialize task watchdog timer (30 second timeout, panic on timeout)
    esp_err_t wdtResult = esp_task_wdt_init(30, true);
    if (wdtResult != ESP_OK)
    {
        // Can't use logger here yet, use Serial
        Serial.printf("[FATAL] Failed to initialize task watchdog timer (error: %d)\n", wdtResult);
        // System can continue but without watchdog protection
    }
    
    constexpr unsigned long kSerialReadyTimeoutMs = 200;
    const unsigned long serialStartMs = millis();
    
    while (!Serial && (millis() - serialStartMs) < kSerialReadyTimeoutMs)
    {
        delay(10);
    }
    
    const unsigned long elapsed = millis() - serialStartMs;
    if (elapsed < 50)
    {
        delay(50 - elapsed);
    }
    
    logger_begin();
    logResetReason();

#if defined(ECHO)
    // Start ECHO-only LED/keypad early so the user sees startup animation immediately.
    // These tasks are compile-time gated and do not exist in TANGO builds.
    {
        BaseType_t ledTaskResult = xTaskCreatePinnedToCore(
            echoLedTask,
            "EchoLedTask",
            4096,
            nullptr,
            2,
            &echoLedTaskHandle,
            1);
        if (ledTaskResult != pdPASS)
        {
            logErrorf("[Startup] Failed to create EchoLedTask");
            echoLedTaskHandle = nullptr;
        }

        BaseType_t keypadTaskResult = xTaskCreatePinnedToCore(
            echoKeypadTask,
            "EchoKeypadTask",
            4096,
            nullptr,
            1,
            &echoKeypadTaskHandle,
            0);
        if (keypadTaskResult != pdPASS)
        {
            logErrorf("[Startup] Failed to create EchoKeypadTask");
            echoKeypadTaskHandle = nullptr;
        }
    }
#endif

    // Check memory availability after logger initialization
    const uint32_t freeHeap = ESP.getFreeHeap();
    constexpr uint32_t kCriticalStartupHeap = 50 * 1024; // 50KB minimum
    if (freeHeap < kCriticalStartupHeap)
    {
        logFatalf("[Startup] CRITICAL: Insufficient heap memory (%u bytes) - system may be unstable", freeHeap);
    }
    
    // Initialize session ID early (before any messages are sent)
    initializeSessionId();
    
    if (!nvs_begin())
    {
        logFatalf("[Startup] Failed to initialize NVS - settings persistence unavailable");
        // System can continue but with degraded functionality
    }
    
    if (!settings_begin())
    {
        logFatalf("[Startup] Failed to initialize settings - device configuration unavailable");
        // System may continue with defaults, but this is a critical failure
    }
    
    // Always send INIT message in JSON format with reboot reason in message text
    const esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = resetReasonToString(reason);
    StaticJsonDocument<256> initDoc;
    initDoc["tm"] = getFormattedTimeWithTimezone(); // Time as first parameter
    initDoc["ty"] = "log";
    initDoc["lv"] = "info";
    String initMsg = "INIT - Reset reason: ";
    initMsg += reasonStr;
    initMsg += " (";
    initMsg += String(static_cast<int>(reason));
    initMsg += ")";
    initDoc["ms"] = initMsg;
    initDoc["rr"] = reasonStr;
    initDoc["mc"] = getDeviceId();
    initDoc["si"] = getSessionId();
    String initOutput;
    serializeJson(initDoc, initOutput);
    serialWriteJsonAtomic(initOutput);

    timeKeeper().begin();

    startAudioCodec();
        
    if (ensureStorage())
    {
        //Initialize in-memory queue PSRAM even if SD card is available.
        psramQueue_begin();

        // Initialize filesystem-based queue
        uploadQueue_begin();
        
        // Clean up one .tmp file in SD card mode (move to /trash)
        // Remaining files will be cleaned up by overnight maintenance
        if (isStorageModeSdCard()) {
            uploadQueue_cleanupOneTempFile();
        }
    }
    else
    {
        logFatalf("[Startup] CRITICAL: No storage available (SD card failed and PSRAM unavailable) - device cannot record");
        // System can continue but cannot record
    }
    
    // Log startup errors for SD card and WiFi
    if (isSdCardPermanentlyDisabled())
    {
        logErrorf("[Startup] ERROR: SD card not found or failed - using PSRAM mode");
        
        // Check if PSRAM is also unavailable
        #ifdef ESP32
        if (ESP.getPsramSize() == 0 || ESP.getFreePsram() == 0)
        {
            logFatalf("[Startup] CRITICAL: SD card failed and PSRAM unavailable - device cannot record");
        }
        #endif
    }
    
    bool hasWiFiCredentials = network_hasAnyWiFiCredentials();
    if (!hasWiFiCredentials)
    {
        logErrorf("[Startup] ERROR: No WiFi credentials configured");
    }

    // Start recording task (will be paused if no WiFi credentials)
    BaseType_t recordTaskResult = xTaskCreatePinnedToCore(
        recordTask,
        "RecordTask",
        8192,
        nullptr,
        2,
        &recordTaskHandle,
        1);

    if (recordTaskResult != pdPASS)
    {
        logFatalf("[Startup] Failed to create record task");
    }
    else
    {
        esp_task_wdt_add(recordTaskHandle);
        health_initTaskHealth(g_healthMetrics.recordTaskHealth, "RecordTask", recordTaskHandle, 8192);

        // RecordTask will check for WiFi credentials and feed watchdog in AP mode
        // No need to suspend it - it will idle and feed watchdog when no credentials
    }

    // Initialize network based on WiFi credentials
    if (hasWiFiCredentials)
    {
        logDebugf("[Startup] Initializing network...");
        //network_begin();
#if defined(ECHO)
        mqtt_task_begin();
#endif
    }
    else
    {
        // No WiFi credentials - start AP mode for configuration
        logInfof("[Startup] No WiFi credentials - starting Access Point mode");
        boondock_server_startAPMode();
    }

    // Boot notification (ECHO speaker): prefer SD-cached boot.wav, otherwise fall back to a short "good" beep.
    // This is queued to run on the recorder task (AudioKit context).
#if defined(ECHO)
    {
        const String bootWav = system_assets_localBootWavPath();
        if (isStorageModeSdCard() && bootWav.length() > 0 && SD_MMC.exists(bootWav))
        {
            // Prevent the recorder from triggering on startup audio (speaker output can leak into mic).
            // We don't know WAV duration here, so use a conservative small window.
            recorder_inhibitRecordingForMs(6000);
            recorder_requestPlayWavAtOrAfter(bootWav, 0);
        }
        else
        {
            // Two short beeps; inhibit slightly longer than the beep sequence.
            recorder_inhibitRecordingForMs(static_cast<uint32_t>((120 + 60) * 2 + 500));
            recorder_beep(120, 60, 2, true);
        }
    }
#endif

    if (hasWiFiCredentials)
    {
        logDebugf("[Startup] Starting NetworkTask (upload + events)...");
        networkHandler_init();
        if (networkTaskHandle != nullptr)
        {
            health_initTaskHealth(g_healthMetrics.uploadTaskHealth, "NetworkTask", networkTaskHandle, 16384);
        }
    }

    BaseType_t serialTaskResult = xTaskCreatePinnedToCore(
        serialTask,
        "SerialTask",
        kSerialTaskStackSize,
        nullptr,
        3,
        &serialTaskHandle,
        0);

    if (serialTaskResult != pdPASS)
    {
        logFatalf("[Startup] Failed to create serial task");
    }
    else
    {
        esp_task_wdt_add(serialTaskHandle);
        health_initTaskHealth(g_healthMetrics.serialTaskHealth, "SerialTask", serialTaskHandle, kSerialTaskStackSize);
    }

    // Only start Maintenance Task if WiFi credentials are configured
    if (hasWiFiCredentials)
    {
        logDebugf("[Startup] Starting Maintenance Task...");
        BaseType_t maintenanceTaskResult = xTaskCreatePinnedToCore(
            maintenanceTask,
            "MaintenanceTask",
            8192,
            nullptr,
            1,
            &maintenanceTaskHandle,
            0);

        if (maintenanceTaskResult != pdPASS)
        {
            logFatalf("[Startup] Failed to create maintenance task");
        }
        else
        {
            // Do not register MaintenanceTask with the task WDT: it frequently blocks on the SD
            // mutex while WebServer (recordings, etc.) scans or streams files; a blocked task
            // cannot call esp_task_wdt_reset() and would abort the whole system after the timeout.
            health_initTaskHealth(g_healthMetrics.maintenanceTaskHealth, "MaintenanceTask", maintenanceTaskHandle, 8192);
        }
    }
    else
    {
        //logDebug("[Startup] Skipping Maintenance Task - no WiFi credentials");
        maintenanceTaskHandle = nullptr;
    }

    // Start Web Server Task (needed for both AP mode and normal mode)
    // Pin to core 1 so HTTP/SD work does not preempt MaintenanceTask (prio 1) on core 0.
    logDebugf("[Startup] Starting Web Server Task...");
    BaseType_t webServerTaskResult = xTaskCreatePinnedToCore(
        webServerTask,
        "WebServer",
        8192,
        nullptr,
        2,  // Priority 2 (same as RecordTask, higher than NetworkTask/MaintenanceTask)
        &webServerTaskHandle,
        1);  // Core 1 (keep Maintenance/Upload/Serial on core 0)

    if (webServerTaskResult != pdPASS)
    {
        logFatalf("[Startup] Failed to create web server task");
    }
    else
    {
        // Do not register WebServer with the task WDT: handleClient() and SD-backed handlers can block
        // for extended periods (same class of issue as MaintenanceTask — mutex / slow I/O); a blocked task
        // cannot call esp_task_wdt_reset() and would abort the whole system.
        health_initTaskHealth(g_healthMetrics.webServerTaskHealth, "WebServer", webServerTaskHandle, 8192);
    }

    // Device ready: print JSON status after every startup process is complete
    {
        const bool storageOk = ensureStorage();
        const bool sdReady = storageOk && isStorageModeSdCard();
        StaticJsonDocument<384> readyDoc;
        readyDoc["ty"] = "ready";
        readyDoc["tm"] = getFormattedTimeWithTimezone();
        readyDoc["ms"] = "Device setup complete";
        readyDoc["mc"] = getDeviceId();
        readyDoc["si"] = getSessionId();
        readyDoc["dv"] = true;
        readyDoc["nw"] = WiFi.isConnected();
        readyDoc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
        readyDoc["ba"] = false;  // Not verified until first successful request
        readyDoc["ti"] = timeKeeper().timeIsValid();
        readyDoc["re"] = (recordTaskHandle != nullptr);
        readyDoc["sd"] = sdReady;
        String readyOutput;
        serializeJson(readyDoc, readyOutput);
        serialWriteJsonAtomic(readyOutput);
    }

#if defined(ECHO)
    echoLed_notifyDeviceReady();
#endif
}

bool system_isUploading()
{
    return networkHandler_isUploading();
}

int system_getUploadQueueSize()
{
    // PSRAM mode: PSRAM queue count; SD mode: files in /pending + items in memory priority queue
    if (isStorageModePsram()) {
        return static_cast<int>(psramQueue_getPendingCount());
    }
    return static_cast<int>(uploadQueue_getPendingCount() + sdCardMemoryQueue_getPendingCount());
}


void system_notifySettingsChanged()
{
    g_lastSettingsChangeMs = millis();
    g_pendingConfigMessage = true;
}

void system_clearPendingConfigMessage()
{
    g_pendingConfigMessage = false;
    g_lastSettingsChangeMs = 0;
}

// Check if WiFi credentials were just added and start tasks if needed
void system_checkAndStartTasksIfWiFiConfigured()
{
    static bool lastWiFiState = false;
    static bool initialized = false;
    bool currentWiFiState = network_hasAnyWiFiCredentials();
    
    // Initialize on first call (during setup)
    if (!initialized)
    {
        lastWiFiState = currentWiFiState;
        initialized = true;
        return;  // Don't trigger on first call, just initialize
    }
    
    // If WiFi credentials were just added (transition from false to true)
    if (!lastWiFiState && currentWiFiState)
    {
        logInfof("[System] WiFi credentials detected - starting tasks");

        if (networkTaskHandle == nullptr)
        {
            logInfof("[System] Starting NetworkTask");
            networkHandler_init();
            if (networkTaskHandle != nullptr)
            {
                health_initTaskHealth(g_healthMetrics.uploadTaskHealth, "NetworkTask", networkTaskHandle, 16384);
            }
        }
        
        // Start Maintenance Task if it doesn't exist
        if (maintenanceTaskHandle == nullptr)
        {
            logInfof("[System] Starting Maintenance Task");
            BaseType_t maintenanceTaskResult = xTaskCreatePinnedToCore(
                maintenanceTask,
                "MaintenanceTask",
                8192,
                nullptr,
                1,
                &maintenanceTaskHandle,
                0);
            
            if (maintenanceTaskResult != pdPASS)
            {
                logErrorf("[System] Failed to create maintenance task");
            }
            else
            {
                // See startup path: MaintenanceTask is not on the task WDT (SD mutex vs WebServer).
                health_initTaskHealth(g_healthMetrics.maintenanceTaskHealth, "MaintenanceTask", maintenanceTaskHandle, 8192);
            }
        }
        
        // Reinitialize network to start WiFi connection
        // BUT: If we're in AP mode, don't call network_begin() yet - it will switch WiFi mode
        // and kill the AP connection. The device will reboot anyway, so let it happen on next boot.
        if (!boondock_server_isAPModeActive())
        {
            network_begin();
        }
        else
        {
            logInfof("[System] WiFi credentials saved in AP mode - will connect after reboot");
        }
    }
    else if (lastWiFiState && !currentWiFiState)
    {
        // WiFi credentials were removed - pause recorder and stop upload/maintenance
        logWarnf("[System] WiFi credentials removed - pausing tasks");
        
        // Pause Recorder Task
        if (recordTaskHandle != nullptr)
        {
            eTaskState taskState = eTaskGetState(recordTaskHandle);
            if (taskState == eRunning || taskState == eReady)
            {
                logInfof("[System] Pausing Recorder Task");
                vTaskSuspend(recordTaskHandle);
            }
        }
        
        // Note: We don't delete Upload/Maintenance tasks here as they check for WiFi in their loops
        // They will just idle until WiFi is configured again
    }
    
    lastWiFiState = currentWiFiState;
}

void loop()
{
    // Starting the network Loop
    network_loop();
    
    static unsigned long lastRssiUpdateMs = 0;
    constexpr unsigned long kRssiUpdateIntervalMs = 5000;
    unsigned long currentTimeMs = millis();
    if (currentTimeMs - lastRssiUpdateMs >= kRssiUpdateIntervalMs)
    {
        network_updateRssi();
        lastRssiUpdateMs = currentTimeMs;
    }

    logger_tick();
    timeKeeper().loop();

    static unsigned long lastMemoryCheckMs = 0;
    const unsigned long kMemoryCheckIntervalMs = MEMORY_MONITOR_INTERVAL_MS;
    if (lastMemoryCheckMs == 0 || (currentTimeMs - lastMemoryCheckMs >= kMemoryCheckIntervalMs))
    {
        checkMemorySafety();
        lastMemoryCheckMs = currentTimeMs;
    }
    
    // Feed watchdog for loop() task
    esp_task_wdt_reset();

    vTaskDelay(pdMS_TO_TICKS(10));
}

SystemHealthMetrics system_getHealthMetrics()
{
    return health_getHealthMetrics();
}

void system_resetMetrics()
{
    health_resetMetrics();
}

TaskHealthMetrics system_getTaskHealthMetrics(const char* taskName)
{
    return health_getTaskHealthMetrics(taskName);
}
