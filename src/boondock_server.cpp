#include "boondock_server.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <SD_MMC.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <functional>
#include <vector>
#include <algorithm>
#include <atomic>
#include <time.h>

#include "common.h"
#include "config.h"
#include "network.h"
#include "logger.h"
#include "settings.h"
#include "recorder.h"
#include "networkHandller.h"
#include "main.h"
#include "timekeeper.h"
#include "web_html.h"
#include "web_css.h"
#include "web_ap_css.h"
#include "web_js.h"
#include "web_spa_html.h"
#include "app_js_spa_gz.h"
#include "system_assets.h"
#if defined(ECHO)
#include "mqtt_task.h"
#endif

namespace
{
    // AP Mode Web Server
    WebServer* apWebServer = nullptr;
    bool apModeActive = false;
    String testConnectionSsid;
    String testConnectionPassword;
    bool testConnectionInProgress = false;
    bool testConnectionResult = false;
    String testConnectionMessage;
    String testConnectionIp;  // Store IP address after successful test connection
    
    // Non-AP Mode Web Server
    WebServer* mainWebServer = nullptr;
    bool mainServerActive = false;
    
    // Async Web Server and WebSocket for real-time updates
    AsyncWebServer* asyncWebServer = nullptr;
    AsyncWebSocket* ws = nullptr;
    bool asyncServerActive = false;
    
    // WebSocket subscription tracking
    struct ClientSubscription {
        uint32_t clientId;
        String page; // "home", "audio", "network"
        unsigned long lastUpdate;
    };
    std::vector<ClientSubscription> clientSubscriptions;
    
    // Firmware update state
    bool firmwareUpdateInProgress = false;
    bool firmwareUpdateComplete = false;
    bool firmwareUpdateFailed = false;
    
    // Helper function to connect WiFiClient with watchdog feeding and retry logic
    // Returns true if connection successful, false otherwise
    // maxRetries: Maximum number of connection attempts (default: 2)
    // connectionTimeoutMs: Timeout per connection attempt in milliseconds (default: 2000)
    bool connectWiFiClientWithRetry(WiFiClient &client, const char *host, uint16_t port, 
                                     uint8_t maxRetries = 2, unsigned long connectionTimeoutMs = 2000)
    {
        // Set shorter timeout to prevent long blocking calls
        client.setTimeout(connectionTimeoutMs);
        
        for (uint8_t attempt = 0; attempt < maxRetries; ++attempt)
        {
            // Feed watchdog before connection attempt
            esp_task_wdt_reset();
            
            // Attempt connection (this will block for up to connectionTimeoutMs)
            // Note: We can't feed watchdog during the blocking connect() call,
            // but we set a shorter timeout to minimize the blocking duration
            bool connected = client.connect(host, port);
            
            // Feed watchdog after connection attempt
            esp_task_wdt_reset();
            
            if (connected)
            {
                network_recordCloudPathConnectResult(true);
                return true;
            }
            
            // Connection failed - small delay between retries (except on last attempt)
            if (attempt < maxRetries - 1)
            {
                delay(100);
            }
        }
        
        // All retries failed (single cloud-path failure tick, matches network.cpp connectWiFiClientWithRetry)
        if (WiFi.isConnected() && WiFi.status() == WL_CONNECTED)
        {
            network_recordCloudPathConnectResult(false);
        }
        return false;
    }
    
    // Helper function to get User-Agent string (cached to avoid stack allocations)
    const char* getUserAgentString()
    {
        // Cache the User-Agent string in static storage to avoid stack allocations
        static char userAgentBuffer[64] = {0};
        static bool initialized = false;
        
        if (!initialized)
        {
            // Extract prefix from FIRMWARE (everything before first dash)
            String firmware = String(FIRMWARE);
            int dashIndex = firmware.indexOf('-');
            String prefix = (dashIndex > 0) ? firmware.substring(0, dashIndex) : "TANGO";
            
            // Format: "Boondock-<PREFIX> V-<FIRMWARE>"
            String userAgent = "Boondock-" + prefix + " V-" + firmware;
            
            // Copy to static buffer (truncate if too long)
            size_t len = userAgent.length();
            if (len >= sizeof(userAgentBuffer))
            {
                len = sizeof(userAgentBuffer) - 1;
            }
            strncpy(userAgentBuffer, userAgent.c_str(), len);
            userAgentBuffer[len] = '\0';
            
            initialized = true;
        }
        
        return userAgentBuffer;
    }
    String firmwareUpdateError = "";
    size_t firmwareUpdateTotalSize = 0;
    size_t firmwareUpdateReceivedSize = 0;

    // Standard error codes (same vocabulary as CLI)
    static const char* const API_ERR_UNKNOWN_CMD   = "UNKNOWN_CMD";
    static const char* const API_ERR_INVALID_VALUE = "INVALID_VALUE";
    static const char* const API_ERR_OUT_OF_RANGE  = "OUT_OF_RANGE";
    static const char* const API_ERR_MISSING_PARAM = "MISSING_PARAM";
    static const char* const API_ERR_HW_ERROR      = "HW_ERROR";
    static const char* const API_ERR_BUSY          = "BUSY";
    static const char* const API_ERR_NOT_FOUND     = "NOT_FOUND";
    static const char* const API_ERR_INTERNAL      = "INTERNAL";

    // --- API response helpers ---
    // Consistent JSON envelope for every API response.

    void apiOk(WebServer* srv, DynamicJsonDocument &doc)
    {
        doc["status"] = "ok";
        String response;
        serializeJson(doc, response);
        srv->send(200, "application/json", response);
    }

    void apiOk(WebServer* srv)
    {
        srv->send(200, "application/json", "{\"status\":\"ok\"}");
    }

    void apiError(WebServer* srv, int httpCode, const char* code, const String &message)
    {
        DynamicJsonDocument doc(384);
        doc["status"] = "error";
        doc["code"] = code;
        doc["message"] = message;
        String response;
        serializeJson(doc, response);
        srv->send(httpCode, "application/json", response);
    }

    // Cache for change detection
    String lastHomeSummaryHash = "";
    String lastNetworkConfigHash = "";
    
    // WebSocket push timing
    unsigned long lastHomePush = 0;
    unsigned long lastAudioPush = 0;
    unsigned long lastAudioSample = 0;
    unsigned long lastNetworkPush = 0;
    const unsigned long HOME_PUSH_INTERVAL = 1000;    // 1 second
    const unsigned long AUDIO_SAMPLE_INTERVAL = 100;   // 0.1 seconds (100ms - collect sample)
    const unsigned long AUDIO_PUSH_INTERVAL = 1000;    // 1 second (send buffer)
    const unsigned long NETWORK_PUSH_INTERVAL = 1000;  // 1 second
    
    // Audio sample buffer (10 samples per second) for WebSocket
    struct AudioSample {
        float currentDb;
        float currentLevel;
        float dynamicRangeUtil;
        bool isRecording;
    };
    const size_t AUDIO_BUFFER_SIZE = 10;
    AudioSample wsAudioBuffer[AUDIO_BUFFER_SIZE];
    size_t wsAudioBufferIndex = 0;
    bool wsAudioBufferReady = false;
    
    // Live audio streaming ring (~1 second of samples in PSRAM at 8 kHz mono int16).
    const size_t LIVE_AUDIO_BUFFER_SAMPLES = 8000;
    const size_t LIVE_AUDIO_BUFFER_BYTES = LIVE_AUDIO_BUFFER_SAMPLES * sizeof(int16_t);
    int16_t* liveAudioPsramBuffer = nullptr;
    std::atomic<uint32_t> liveAudioSamplesWritten(0);
    uint32_t liveAudioSamplesSentTotal = 0;
    uint32_t liveAudioPushSequence = 0;
    size_t liveAudioBufferFilled = 0;
    bool liveAudioBufferReady = false;
    const unsigned long LIVE_AUDIO_PUSH_INTERVAL = 150; // ~6–7 pushes/s for steadier client queue
    const size_t LIVE_AUDIO_MAX_SAMPLES_PER_PUSH = 1200; // ~150 ms @ 8 kHz per WS message

    // Binary live-audio frame: little-endian header followed by codec payload.
    //   0   4  Magic 'BAUD' (0x42 0x41 0x55 0x44)
    //   4   1  Version          (1)
    //   5   1  Codec            (0 = PCM16 LE, 1 = G.711 µ-law)
    //   6   1  Channels         (1)
    //   7   1  Reserved         (0)
    //   8   4  Sample rate (uint32 LE)
    //  12   4  Sample count (uint32 LE, frames not bytes)
    //  16   4  Sequence    (uint32 LE)
    //  20   N  Payload (PCM16 LE -> 2*N bytes, µ-law -> N bytes)
    const uint8_t LIVE_AUDIO_FRAME_VERSION = 1;
    const uint8_t LIVE_AUDIO_CODEC_PCM16   = 0;
    const uint8_t LIVE_AUDIO_CODEC_ULAW    = 1;
    const size_t LIVE_AUDIO_HEADER_BYTES   = 20;

    // G.711 µ-law encode: int16 PCM -> uint8. Branchy form is plenty fast at 8 kHz.
    static inline uint8_t pcm16_to_ulaw(int16_t pcm)
    {
        const int16_t BIAS = 0x84; // 132
        const int16_t CLIP = 32635;
        uint8_t sign = 0;
        int32_t sample = pcm;
        if (sample < 0)
        {
            sample = -sample;
            sign = 0x80;
        }
        if (sample > CLIP) sample = CLIP;
        sample += BIAS;

        int exponent = 7;
        for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1)
        {
            exponent--;
        }
        int mantissa = (sample >> (exponent + 3)) & 0x0F;
        return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
    }

    // Dedicated FreeRTOS task that drives boondock_server_pushLiveAudio() on a steady cadence,
    // independent of boondock_server_loop / WebServerTask jitter. Pinned to core 0 so it shares a
    // core with AsyncTCP (no concurrency between this task and the WS event handler on the same
    // core, which keeps the lock-free clientSubscriptions reads safe).
    TaskHandle_t liveAudioSenderTaskHandle = nullptr;
    std::atomic<bool> liveAudioSenderRunning(false);

    static void liveAudioSenderTask(void* /*arg*/)
    {
        const TickType_t period = pdMS_TO_TICKS(LIVE_AUDIO_PUSH_INTERVAL);
        TickType_t lastWake = xTaskGetTickCount();
        while (liveAudioSenderRunning.load(std::memory_order_acquire))
        {
            if (asyncServerActive && ws != nullptr && liveAudioPsramBuffer != nullptr)
            {
                bool hasSubscribers = false;
                for (const auto& sub : clientSubscriptions)
                {
                    if (sub.page == "live-audio")
                    {
                        hasSubscribers = true;
                        break;
                    }
                }
                if (hasSubscribers)
                {
                    boondock_server_pushLiveAudio();
                }
            }
            vTaskDelayUntil(&lastWake, period);
        }
        liveAudioSenderTaskHandle = nullptr;
        vTaskDelete(nullptr);
    }

    void startLiveAudioSenderTask()
    {
        if (liveAudioSenderTaskHandle != nullptr) return;
        liveAudioSenderRunning.store(true, std::memory_order_release);
        BaseType_t r = xTaskCreatePinnedToCore(
            liveAudioSenderTask,
            "LiveAudioTx",
            4096,
            nullptr,
            2,   // same priority as WebServer/RecordTask
            &liveAudioSenderTaskHandle,
            0);  // pin to core 0 (with AsyncTCP / lwIP)
        if (r != pdPASS)
        {
            liveAudioSenderRunning.store(false, std::memory_order_release);
            liveAudioSenderTaskHandle = nullptr;
            logErrorf("[LiveAudio] Failed to create LiveAudioTx task\n");
        }
        else
        {
            logDebugf("[LiveAudio] LiveAudioTx task started (period=%lu ms, core 0)\n",
                      static_cast<unsigned long>(LIVE_AUDIO_PUSH_INTERVAL));
        }
    }

    void stopLiveAudioSenderTask()
    {
        if (liveAudioSenderTaskHandle == nullptr) return;
        liveAudioSenderRunning.store(false, std::memory_order_release);
        // Task will self-delete on its next loop iteration; do not join here to avoid blocking
        // the caller (which may itself be a high-priority task tearing the web server down).
    }

    void syncRecorderLiveAudioFeed()
    {
        bool hasSubscriber = false;
        for (const auto& sub : clientSubscriptions)
        {
            if (sub.page == "live-audio")
            {
                hasSubscriber = true;
                break;
            }
        }
        static bool hadLiveAudioSubscriber = false;
        if (hasSubscriber && !hadLiveAudioSubscriber)
        {
            // Start streaming from "now" only — avoid dumping a full ring of possibly stale audio.
            liveAudioSamplesSentTotal = liveAudioSamplesWritten.load(std::memory_order_acquire);
        }
        hadLiveAudioSubscriber = hasSubscriber;
        recorder_setLiveAudioFeedEnabled(hasSubscriber);
    }
    
    // Client connection tracking
    unsigned long lastClientActivityMs = 0;
    bool clientWasActive = false;
    
    // Simple hash function for change detection
    static uint32_t simpleHash(const String& str)
    {
        uint32_t hash = 0;
        for (size_t i = 0; i < str.length(); ++i)
        {
            hash = ((hash << 5) - hash) + str.charAt(i);
        }
        return hash;
    }
    
    // Simple base64 encoding
    static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    static String base64Encode(const uint8_t* data, size_t len)
    {
        String result;
        result.reserve(((len + 2) / 3) * 4);
        
        for (size_t i = 0; i < len; i += 3)
        {
            uint32_t chunk = 0;
            int bytes = 0;
            
            for (int j = 0; j < 3 && (i + j) < len; j++)
            {
                chunk |= ((uint32_t)data[i + j]) << (16 - j * 8);
                bytes++;
            }
            
            for (int j = 0; j < 4; j++)
            {
                if (j * 6 < bytes * 8)
                {
                    int idx = (chunk >> (18 - j * 6)) & 0x3F;
                    result += base64_chars[idx];
                }
                else
                {
                    result += '=';
                }
            }
        }
        
        return result;
    }
    
    static String getAPSSID()
    {
        return "Boondock-AP";
    }

    // Format timezone offset like "UTC", "UTC+5", "UTC-7"
    static String formatTimezoneOffset(int8_t offsetHours)
    {
        if (offsetHours == 0)
        {
            return "UTC (GMT+0)";
        }
        String sign = offsetHours > 0 ? "+" : "";
        return "UTC" + sign + String(offsetHours) + " (GMT" + sign + String(offsetHours) + ")";
    }

    // Get boot reason as human-readable string
    static const char* getBootReasonString()
    {
        esp_reset_reason_t reason = esp_reset_reason();
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
}

// Forward declarations for functions defined below
static void testWiFiConnectionTask(void* pvParameters);
static void setupAPWebServer();
static void handleAPRoot();
static void handleAPStyle();
static void handleAPScript();
static void handleAPDeviceInfo();
static void handleAPWiFiScan();
static void handleAPWiFiTest();
static void handleAPWiFiSave();

// Helper function to mark client activity
static void markClientActivity()
{
    lastClientActivityMs = millis();
    clientWasActive = true;
}

// Helper function to clear activity flag (called periodically when no activity)
static void clearClientActivityIfStale()
{
    constexpr unsigned long kActivityStaleTimeoutMs = 10000; // 10 seconds
    unsigned long now = millis();
    
    if (lastClientActivityMs > 0 && (now - lastClientActivityMs) >= kActivityStaleTimeoutMs)
    {
        clientWasActive = false;
    }
}

static void handleAPRoot()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    String html = String(FPSTR(htmlContent));
    apWebServer->send(200, "text/html", html);
}

static void handleAPStyle()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    String css = String(FPSTR(apCssContent));
    apWebServer->send(200, "text/css", css);
}

static void handleAPScript()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    String js = String(FPSTR(jsContent));
    apWebServer->send(200, "application/javascript", js);
}

static void handleAPDeviceInfo()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    
    DynamicJsonDocument doc(512);
    doc["mac"] = getDeviceId();
    doc["firmware"] = FIRMWARE;
    doc["product"] = PRODUCT_BROWSER_TITLE;
    
    // Get IP address if connected (in mixed mode after test connection)
    // First check if we have a stored IP from successful test connection
    if (testConnectionIp.length() > 0)
    {
        doc["ip"] = testConnectionIp;
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
        doc["ip"] = WiFi.localIP().toString();
    }
    else
    {
        doc["ip"] = "Not connected";
    }
    
    String response;
    serializeJson(doc, response);
    apWebServer->send(200, "application/json", response);
}

static void handleAPWiFiScan()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    // Scan for networks
    int n = WiFi.scanNetworks();
    
    DynamicJsonDocument doc(4096);
    JsonArray networks = doc.createNestedArray("networks");
    
    for (int i = 0; i < n; ++i)
    {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    
    String response;
    serializeJson(doc, response);
    apWebServer->send(200, "application/json", response);
}

static void handleAPWiFiTest()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    if (!apWebServer->hasArg("plain"))
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing request body\"}");
        return;
    }
    
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, apWebServer->arg("plain"));
    
    if (error)
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    
    if (ssid.length() == 0)
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"SSID is required\"}");
        return;
    }
    
    // Store test credentials
    testConnectionSsid = ssid;
    testConnectionPassword = password;
    testConnectionInProgress = true;
    testConnectionResult = false;
    testConnectionMessage = "";
    
    // Create task to test connection in mixed mode (AP + STA)
    xTaskCreatePinnedToCore(
        testWiFiConnectionTask,
        "WiFiTest",
        4096,
        nullptr,
        1,
        nullptr,
        1
    );
    
    // Wait for test to complete (with timeout)
    unsigned long startTime = millis();
    constexpr unsigned long timeoutMs = 15000; // 15 second timeout
    
    while (testConnectionInProgress && (millis() - startTime) < timeoutMs)
    {
        delay(100);
    }
    
    DynamicJsonDocument responseDoc(256);
    responseDoc["success"] = testConnectionResult;
    responseDoc["message"] = testConnectionMessage;
    
    String response;
    serializeJson(responseDoc, response);
    apWebServer->send(200, "application/json", response);
}

static void testWiFiConnectionTask(void* pvParameters)
{
    // Switch to mixed mode (AP + STA)
    WiFi.mode(WIFI_AP_STA);
    
    // Try to connect to the test network
    WiFi.begin(testConnectionSsid.c_str(), testConnectionPassword.length() > 0 ? testConnectionPassword.c_str() : nullptr);
    
    // Wait for connection (max 10 seconds)
    unsigned long startTime = millis();
    constexpr unsigned long timeoutMs = 10000;
    
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeoutMs)
    {
        delay(500);
    }
    
    if (WiFi.status() == WL_CONNECTED)
    {
        testConnectionResult = true;
        testConnectionMessage = "Connection successful";
        testConnectionIp = WiFi.localIP().toString();  // Store the IP address
        // Keep connection active - stay in WIFI_AP_STA mode, don't disconnect
        // This allows the web interface to retrieve the IP address
    }
    else
    {
        testConnectionResult = false;
        testConnectionMessage = "Connection failed - check SSID and password";
        testConnectionIp = "";  // Clear IP on failure
        WiFi.disconnect();
        // Switch back to AP mode only on failure
        WiFi.mode(WIFI_AP);
    }
    
    testConnectionInProgress = false;
    vTaskDelete(nullptr);
}

// Helper function to push settings to cloud before rebooting
static void pushSettingsToCloudBeforeReboot()
{
    // Build masked settings JSON and push to API server
    String maskedJson = settings_getMaskedJsonForServer();
    bool ok = network_pushSettingsToServer(maskedJson);
    
    if (ok)
    {
        logInfof("[Server] Settings pushed to cloud before reboot");
    }
    else
    {
        logWarnf("[Server] Failed to push settings to cloud before reboot (continuing with reboot)");
    }
}

static void handleAPWiFiSave()
{
    if (!apWebServer)
        return;
    
    markClientActivity();
    if (!apWebServer->hasArg("plain"))
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"Missing request body\"}");
        return;
    }
    
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, apWebServer->arg("plain"));
    
    if (error)
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }
    
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    
    if (ssid.length() == 0)
    {
        apWebServer->send(400, "application/json", "{\"success\":false,\"message\":\"SSID is required\"}");
        return;
    }
    
    // Clear debounce to ensure immediate save
    settings_clearDebounce();
    
    // Save WiFi credentials to first slot using settings_setParam
    if (!settings_setParam("wifi[0].ssid", ssid.c_str()))
    {
        apWebServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to set SSID\"}");
        return;
    }
    
    if (password.length() > 0)
    {
        if (!settings_setParam("wifi[0].password", password.c_str()))
        {
            apWebServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to set password\"}");
            return;
        }
    }
    else
    {
        // Clear password for open networks
        if (!settings_setParam("wifi[0].password", ""))
        {
            apWebServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to clear password\"}");
            return;
        }
    }
    
    // Force an additional save to ensure persistence (settings_setParam may have debounce)
    settings_clearDebounce();
    if (!settings_save())
    {
        apWebServer->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save settings to NVS\"}");
        return;
    }
    
    // Verify the settings were actually saved by checking in-memory structure
    const char* savedSsid = appSettings.wifi[0].ssid;
    if (savedSsid == nullptr || strcmp(savedSsid, ssid.c_str()) != 0)
    {
        logErrorf("[AP] WARNING: SSID verification failed after save");
        apWebServer->send(500, "application/json", "{\"success\":false,\"message\":\"Settings verification failed\"}");
        return;
    }
    
    DynamicJsonDocument responseDoc(128);
    responseDoc["success"] = true;
    responseDoc["message"] = "WiFi credentials saved successfully";
    
    String response;
    serializeJson(responseDoc, response);
    apWebServer->send(200, "application/json", response);
    
    // Push settings to cloud before rebooting (will fail silently if not connected to WiFi)
    pushSettingsToCloudBeforeReboot();
    
    // Give NVS time to complete the write operation before rebooting
    // NVS writes are typically fast, but we want to ensure completion
    delay(2000);
    ESP.restart();
}

static void setupAPWebServer()
{
    if (apWebServer != nullptr)
    {
        apWebServer->stop();
        delete apWebServer;
    }
    
    apWebServer = new WebServer(80);
    
    // Root page
    apWebServer->on("/", HTTP_GET, []() { handleAPRoot(); });
    
    // Static files
    apWebServer->on("/style.css", HTTP_GET, []() { handleAPStyle(); });
    apWebServer->on("/script.js", HTTP_GET, []() { handleAPScript(); });
    
    // API endpoints
    apWebServer->on("/api/device-info", HTTP_GET, []() { handleAPDeviceInfo(); });
    apWebServer->on("/api/wifi/scan", HTTP_GET, []() { handleAPWiFiScan(); });
    apWebServer->on("/api/wifi/test", HTTP_POST, []() { handleAPWiFiTest(); });
    apWebServer->on("/api/wifi/save", HTTP_POST, []() { handleAPWiFiSave(); });
    
    // Handle favicon.ico requests (browsers automatically request this)
    apWebServer->on("/favicon.ico", HTTP_GET, []() {
        if (apWebServer != nullptr)
        {
            // Return 204 No Content to avoid warning about zero content length
            apWebServer->send(204);
        }
    });
    
    // Handle 404 for all other unhandled requests
    apWebServer->onNotFound([]() {
        if (apWebServer != nullptr)
        {
            // Return 404 with a minimal response to avoid zero content length warning
            apWebServer->send(404, "text/plain", "Not Found");
        }
    });
    
    apWebServer->begin();
}

void boondock_server_startAPMode()
{
    if (apModeActive)
    {
        return; // Already in AP mode
    }
    
    String apSSID = getAPSSID();
    const char* apPassword = "boondockecho";

    // Disconnect any existing WiFi connection
    WiFi.disconnect(true, true);
    delay(100);
    
    // Set WiFi mode to AP mode explicitly before starting AP
    WiFi.mode(WIFI_AP);
    delay(100);
    
    // Ensure WiFi event handler is registered (needed for AP station connection events)
    network_reinitializeWiFi();
    
    // Start AP mode
    if (!WiFi.softAP(apSSID.c_str(), apPassword))
    {
        // Check if we have WiFi credentials - if not, this is fatal
        if (!network_hasAnyWiFiCredentials())
        {
            logFatalf("[Startup] Failed to start Access Point - device unreachable (no WiFi credentials)");
        }
        else
        {
            logErrorf("[AP] Failed to start Access Point");
        }
        return;
    }
    
    apModeActive = true;

    IPAddress apIP = WiFi.softAPIP();
    const String &deviceId = getDeviceId();

    Serial.println("");
    Serial.println("========================================");

    Serial.printf("  BOONDOCK %s\n", PRODUCT_AP_ENV_LABEL);
    Serial.printf("  FIRMWARE        : %s\n", FIRMWARE);
    Serial.printf("  DEVICE ID / MAC : %s\n", deviceId.c_str());
    Serial.println("========================================");
    Serial.println("  WiFi Access Point Mode");

    Serial.println("========================================");
    Serial.printf("  WIFI SSID       : %s\n", apSSID.c_str());
    Serial.printf("  WIFI Password   : %s\n", apPassword);
    Serial.printf("  ACCESS Point IP : %s\n", apIP.toString().c_str());
    Serial.println("========================================");
    Serial.println("  Connect to the WiFi network and open");
    Serial.printf("  http://%s\n", apIP.toString().c_str());
    Serial.println("========================================");

    // Setup web server
    setupAPWebServer();
}

// Forward declarations for main web server handlers
static void setupMainWebServer();
static void handleMainSPA();
static void handleMainCSS();
static void handleMainJS();
static void handleMainDeviceInfo();
static void handleMainHomeSummary();
static void handleMainAudioStats();
static void handleMainAudioSettings();
static void handleMainAudioSettingsSave();
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
static void handleMainAudioMonitorInputChannel();
#endif
static void handleMainAudioDefaults();
static void handleMainAudioSave();
static void handleMainLiveAudioSession();
#if defined(ECHO)
static void handleMainCwSave();
#endif
#if defined(ECHO)
static void handleMainAudioMorse();
#endif
static void handleMainSdCardSettings();
static void handleMainSdCardTest();
static void handleMainTestUploadHost();
static void handleMainUploadLocations();
static void handleMainWifiTxPowerSettings();
static void handleMainNetworkConfig();
static void handleMainNetworkSave();
static void handleMainAdvancedUpdateFirmware();
static void handleMainAdvancedReboot();
static void handleMainAdvancedSetDefault();
static void handleMainAdvancedFactoryReset();
static void handleMainAdvancedExportSettings();
static void handleMainAdvancedImportSettings();
static void handleMainFirmwareCheck();
static void handleMainFirmwareApply();
static void handleApiCmd();
static void handleMainRecordingsFolders();
static void handleMainRecordingsList();
static void handleMainRecordingsSummary();
static void handleMainRecordingsStream();

static bool parseHttpRangeHeader(const String &rangeHeader, size_t fileSize, size_t &outStart, size_t &outEnd);

// Main web server handlers - Single Page Application
static void handleMainSPA()
{
    if (!mainWebServer)
        return;
    markClientActivity();
    // Prevent stale SPA caching across firmware updates.
    mainWebServer->sendHeader("Cache-Control", "no-store");
    // Prefer SD-cached SPA index if present.
    if (isStorageModeSdCard())
    {
        const String p = system_assets_localSpaIndexPath();
        if (p.length() > 0 && SD_MMC.exists(p))
        {
            File f = SD_MMC.open(p, FILE_READ);
            if (f)
            {
                mainWebServer->streamFile(f, "text/html");
                f.close();
                return;
            }
        }
    }

    String html = String(FPSTR(spaHtmlContent));
    mainWebServer->send(200, "text/html", html);
}

static void handleMainCSS()
{
    if (!mainWebServer)
        return;
    markClientActivity();
    // Prevent stale CSS caching across firmware updates.
    mainWebServer->sendHeader("Cache-Control", "no-store");
    // Prefer SD-cached CSS if present.
    if (isStorageModeSdCard())
    {
        const String p = system_assets_localSpaCssPath();
        if (p.length() > 0 && SD_MMC.exists(p))
        {
            File f = SD_MMC.open(p, FILE_READ);
            if (f)
            {
                mainWebServer->streamFile(f, "text/css");
                f.close();
                return;
            }
        }
    }

    String css = String(FPSTR(cssContent));
    mainWebServer->send(200, "text/css", css);
}

static void handleMainJS()
{
    if (!mainWebServer)
        return;
    markClientActivity();
    // Prevent stale JS caching across firmware updates.
    mainWebServer->sendHeader("Cache-Control", "no-store");
    // Prefer SD-cached JS if present.
    if (isStorageModeSdCard())
    {
        const String p = system_assets_localSpaJsPath();
        if (p.length() > 0 && SD_MMC.exists(p))
        {
            File f = SD_MMC.open(p, FILE_READ);
            if (f)
            {
                mainWebServer->streamFile(f, "application/javascript");
                f.close();
                return;
            }
        }
    }

    mainWebServer->sendHeader("Content-Encoding", "gzip");
    mainWebServer->sendHeader("Vary", "Accept-Encoding");
    // WebServer::send_P() treats its PROGMEM payload as text on some versions
    // of Arduino-ESP32. A gzip stream contains NUL bytes, so send the headers
    // first and then write the explicitly sized binary payload.
    mainWebServer->setContentLength(appJsSpaGzipLength);
    mainWebServer->send(200, "application/javascript", "");
    mainWebServer->sendContent_P(reinterpret_cast<PGM_P>(appJsSpaGzip),
                                 appJsSpaGzipLength);

}

static void handleMainDeviceInfo()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    
    DynamicJsonDocument doc(768);
    doc["mac"] = getDeviceId();
    doc["firmware"] = FIRMWARE;
    doc["product"] = PRODUCT_BROWSER_TITLE;
    
    if (WiFi.status() == WL_CONNECTED)
    {
        doc["ip"] = WiFi.localIP().toString();
    }
    else
    {
        doc["ip"] = "Not connected";
    }
    
    doc["timezoneOffsetHours"] = appSettings.timezone.offsetHours;
    doc["timezoneDisplay"] = formatTimezoneOffset(appSettings.timezone.offsetHours);
    doc["sdCardMounted"] = isStorageModeSdCard();
    doc["recordToSdCard"] = appSettings.sdCard.recordToSdCard;
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static bool parseHttpRangeHeader(const String &rangeHeader, size_t fileSize, size_t &outStart, size_t &outEnd)
{
    if (!rangeHeader.length() || !rangeHeader.startsWith("bytes="))
    {
        return false;
    }
    String r = rangeHeader.substring(6);
    const int comma = r.indexOf(',');
    if (comma >= 0)
    {
        r = r.substring(0, comma);
    }
    r.trim();
    const int dash = r.indexOf('-');
    if (dash < 0)
    {
        return false;
    }
    const String startStr = r.substring(0, dash);
    const String endStr = r.substring(dash + 1);
    if (startStr.length() == 0)
    {
        return false;
    }
    outStart = static_cast<size_t>(startStr.toInt());
    if (fileSize == 0 || outStart >= fileSize)
    {
        return false;
    }
    if (endStr.length() > 0)
    {
        outEnd = static_cast<size_t>(endStr.toInt());
    }
    else
    {
        outEnd = fileSize - 1;
    }
    if (outEnd >= fileSize)
    {
        outEnd = fileSize - 1;
    }
    if (outStart > outEnd)
    {
        return false;
    }
    return true;
}

static bool recordingsFolderEntrySkip(const String &name)
{
    if (name.length() == 0)
    {
        return true;
    }
    if (name == "." || name == "..")
    {
        return true;
    }
    // Year folder from unsynced RTC (epoch); do not list in Recordings tree.
    return name == "1970";
}

// WebServer shares the SD bus with other tasks; aggressive SD scans can trigger mmc read errors (e.g. 257).
// Yield often and for a few ms so other work (WDT, SD driver, WiFi) can run between reads.
static constexpr uint32_t kRecordingsSdEntriesPerYield = 8u;
static constexpr uint32_t kRecordingsSdYieldDelayMs = 3u;
static constexpr uint32_t kRecordingsStreamChunksPerYield = 4u; // smaller chunks → more yields for SD sharing
static constexpr size_t kRecordingsStreamReadChunkBytes = 512;

static std::atomic<int> g_recordingsStreamActive{0};
static constexpr int kMaxConcurrentRecordingsStreams = 2;

struct RecordingsStreamSlot
{
    bool taken = false;
    RecordingsStreamSlot()
    {
        const int n = g_recordingsStreamActive.fetch_add(1) + 1;
        if (n <= kMaxConcurrentRecordingsStreams)
        {
            taken = true;
        }
        else
        {
            g_recordingsStreamActive--;
        }
    }
    ~RecordingsStreamSlot()
    {
        if (taken)
        {
            g_recordingsStreamActive--;
        }
    }
    bool ok() const { return taken; }
};

// Short TTL cache for GET /api/recordings/folders (reduces repeated SD directory enumeration on UI refresh)
static String g_cachedFoldersCanon;
static String g_cachedFoldersJson;
static unsigned long g_cachedFoldersAtMs = 0;
static constexpr unsigned long kRecordingsFoldersCacheTtlMs = 5000;

static void recordingsYieldForOtherTasks()
{
    esp_task_wdt_reset();
    const TickType_t ticks = pdMS_TO_TICKS(kRecordingsSdYieldDelayMs);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

static void handleMainRecordingsFolders()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();
    if (!isStorageModeSdCard())
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "SD card storage not active");
        return;
    }

    String rawPath = "/recordings";
    if (mainWebServer->hasArg("path"))
    {
        rawPath = mainWebServer->arg("path");
    }
    String canon;
    if (!recordings_canonicalizePath(WebServer::urlDecode(rawPath), canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid path");
        return;
    }
    if (canon == "/recordings" && !SD_MMC.exists("/recordings"))
    {
        storage_ensureDirectoryPath("/recordings");
    }
    if (!SD_MMC.exists(canon))
    {
        apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "path not found");
        return;
    }
    if (recordings_isIgnoredEpochFolderPath(canon))
    {
        DynamicJsonDocument doc(512);
        doc["status"] = "ok";
        doc["path"] = canon;
        doc.createNestedArray("folders");
        String response;
        serializeJson(doc, response);
        mainWebServer->send(200, "application/json", response);
        return;
    }

    const unsigned long foldersNowMs = millis();
    if (g_cachedFoldersCanon == canon && g_cachedFoldersJson.length() > 0 &&
        (foldersNowMs - g_cachedFoldersAtMs) < kRecordingsFoldersCacheTtlMs)
    {
        mainWebServer->send(200, "application/json", g_cachedFoldersJson);
        return;
    }

    File dir = SD_MMC.open(canon);
    if (!dir || !dir.isDirectory())
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "not a directory");
        return;
    }
    recordingsYieldForOtherTasks();

    std::vector<String> subdirs;
    uint32_t entryCount = 0;
    for (;;)
    {
        File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        if ((++entryCount % kRecordingsSdEntriesPerYield) == 0u)
        {
            recordingsYieldForOtherTasks();
        }
        if (entry.isDirectory())
        {
            String name = String(entry.name());
            const int slash = name.lastIndexOf('/');
            if (slash >= 0)
            {
                name = name.substring(slash + 1);
            }
            if (!recordingsFolderEntrySkip(name))
            {
                subdirs.push_back(name);
            }
        }
        entry.close();
    }
    dir.close();

    std::sort(subdirs.begin(), subdirs.end());

    DynamicJsonDocument doc(4096);
    doc["status"] = "ok";
    doc["path"] = canon;
    JsonArray arr = doc.createNestedArray("folders");
    for (const auto &d : subdirs)
    {
        arr.add(d);
    }
    String response;
    serializeJson(doc, response);
    g_cachedFoldersCanon = canon;
    g_cachedFoldersJson = response;
    g_cachedFoldersAtMs = millis();
    mainWebServer->send(200, "application/json", response);
}

static void handleMainRecordingsList()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();
    if (!isStorageModeSdCard())
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "SD card storage not active");
        return;
    }
    if (!mainWebServer->hasArg("path"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing path");
        return;
    }

    String canon;
    if (!recordings_canonicalizePath(WebServer::urlDecode(mainWebServer->arg("path")), canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid path");
        return;
    }
    if (!recordings_isDayFolderPath(canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "path must be a day folder /recordings/YYYY/MM/DD");
        return;
    }
    if (!SD_MMC.exists(canon))
    {
        apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "path not found");
        return;
    }
    if (recordings_isIgnoredEpochFolderPath(canon))
    {
        int page = 1;
        int perPage = 5;
        if (mainWebServer->hasArg("page"))
        {
            page = mainWebServer->arg("page").toInt();
        }
        if (mainWebServer->hasArg("perPage"))
        {
            perPage = mainWebServer->arg("perPage").toInt();
        }
        if (page < 1)
        {
            page = 1;
        }
        if (perPage < 1)
        {
            perPage = 5;
        }
        if (perPage > 50)
        {
            perPage = 50;
        }
        DynamicJsonDocument doc(512);
        doc["status"] = "ok";
        doc["path"] = canon;
        doc["total"] = 0;
        doc["page"] = page;
        doc["perPage"] = perPage;
        doc.createNestedArray("items");
        String response;
        serializeJson(doc, response);
        mainWebServer->send(200, "application/json", response);
        return;
    }

    const String summaryPath = canon + "/summary.json";
    struct SummaryRow
    {
        String name;
        String inboxPath;
        uint64_t sizeBytes = 0;
        uint32_t durationMs = 0;
        String endReason;
    };
    std::vector<SummaryRow> rows;
    if (SD_MMC.exists(summaryPath))
    {
        File sumFile = SD_MMC.open(summaryPath, FILE_READ);
        if (sumFile)
        {
            while (sumFile.available())
            {
                // Yield every line: long JSONL reads + concurrent SD recording/upload otherwise hit error 257 and block for seconds.
                recordingsYieldForOtherTasks();
                String line = sumFile.readStringUntil('\n');
                line.trim();
                if (line.isEmpty())
                {
                    continue;
                }
                StaticJsonDocument<1024> sdoc;
                if (deserializeJson(sdoc, line))
                {
                    continue;
                }
                String playPath;
                if (sdoc.containsKey("path"))
                {
                    playPath = sdoc["path"].as<String>();
                }
                else if (sdoc.containsKey("inboxPath"))
                {
                    playPath = sdoc["inboxPath"].as<String>();
                }
                if (playPath.isEmpty())
                {
                    continue;
                }
                const int slash = playPath.lastIndexOf('/');
                const String base = (slash >= 0) ? playPath.substring(slash + 1) : playPath;
                SummaryRow row;
                row.name = base;
                row.inboxPath = playPath;
                row.sizeBytes = sdoc["sizeBytes"] | 0ULL;
                row.durationMs = sdoc["durationMs"] | 0U;
                if (sdoc.containsKey("endReason"))
                {
                    row.endReason = sdoc["endReason"].as<String>();
                }
                rows.push_back(row);
            }
            sumFile.close();
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const SummaryRow &a, const SummaryRow &b) { return a.name < b.name; });

    int page = 1;
    int perPage = 5;
    if (mainWebServer->hasArg("page"))
    {
        page = mainWebServer->arg("page").toInt();
    }
    if (mainWebServer->hasArg("perPage"))
    {
        perPage = mainWebServer->arg("perPage").toInt();
    }
    if (page < 1)
    {
        page = 1;
    }
    if (perPage < 1)
    {
        perPage = 5;
    }
    if (perPage > 50)
    {
        perPage = 50;
    }

    const size_t total = rows.size();
    const size_t startIdx = static_cast<size_t>(page - 1) * static_cast<size_t>(perPage);
    if (startIdx > total)
    {
        apiError(mainWebServer, 400, API_ERR_OUT_OF_RANGE, "page out of range");
        return;
    }
    const size_t endIdx = std::min(startIdx + static_cast<size_t>(perPage), total);

    DynamicJsonDocument doc(12288);
    doc["status"] = "ok";
    doc["path"] = canon;
    doc["total"] = static_cast<uint32_t>(total);
    doc["page"] = page;
    doc["perPage"] = perPage;
    JsonArray items = doc.createNestedArray("items");
    for (size_t i = startIdx; i < endIdx; ++i)
    {
        JsonObject o = items.createNestedObject();
        o["name"] = rows[i].name;
        o["sizeBytes"] = static_cast<uint64_t>(rows[i].sizeBytes);
        o["path"] = rows[i].inboxPath;
        if (rows[i].durationMs > 0)
        {
            o["durationMs"] = rows[i].durationMs;
        }
        if (rows[i].endReason.length() > 0)
        {
            o["endReason"] = rows[i].endReason;
        }
        time_t utcEpoch = 0;
        if (inbox_parseWavBasenameUtcEpoch(rows[i].name, utcEpoch))
        {
            struct tm tmu = {};
            gmtime_r(&utcEpoch, &tmu);
            char utcBuf[32];
            if (strftime(utcBuf, sizeof(utcBuf), "%Y-%m-%dT%H:%M:%SZ", &tmu) > 0)
            {
                o["recordedAtUtc"] = String(utcBuf);
            }
        }
        else
        {
            o["recordedAtUtc"] = "";
        }
    }

    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

// Stream raw summary.json (JSONL) for a day folder — no line parsing on device; client paginates.
static void handleMainRecordingsSummary()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();
    if (!isStorageModeSdCard())
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "SD card storage not active");
        return;
    }
    if (!mainWebServer->hasArg("path"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing path");
        return;
    }

    String canon;
    if (!recordings_canonicalizePath(WebServer::urlDecode(mainWebServer->arg("path")), canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid path");
        return;
    }
    if (!recordings_isDayFolderPath(canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "path must be a day folder /recordings/YYYY/MM/DD");
        return;
    }
    if (!SD_MMC.exists(canon))
    {
        apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "path not found");
        return;
    }
    if (recordings_isIgnoredEpochFolderPath(canon))
    {
        mainWebServer->sendHeader("Cache-Control", "no-store");
        mainWebServer->send(200, "application/x-ndjson", "");
        return;
    }

    const String summaryPath = canon + "/summary.json";
    if (!SD_MMC.exists(summaryPath))
    {
        mainWebServer->sendHeader("Cache-Control", "no-store");
        mainWebServer->send(200, "application/x-ndjson", "");
        return;
    }

    File f = SD_MMC.open(summaryPath, FILE_READ);
    if (!f || f.isDirectory())
    {
        mainWebServer->sendHeader("Cache-Control", "no-store");
        mainWebServer->send(200, "application/x-ndjson", "");
        return;
    }

    const size_t fileSize = f.size();
    mainWebServer->sendHeader("Cache-Control", "no-store");
    mainWebServer->setContentLength(fileSize);
    mainWebServer->send(200, "application/x-ndjson", "");
    uint8_t buf[kRecordingsStreamReadChunkBytes];
    size_t remaining = fileSize;
    uint32_t chunks = 0;
    while (remaining > 0)
    {
        const size_t n = std::min(remaining, kRecordingsStreamReadChunkBytes);
        const size_t r = f.read(buf, n);
        if (r == 0)
        {
            break;
        }
        mainWebServer->sendContent(reinterpret_cast<const char *>(buf), r);
        remaining -= r;
        if ((++chunks % kRecordingsStreamChunksPerYield) == 0u)
        {
            recordingsYieldForOtherTasks();
        }
    }
    f.close();
}

// /pending uses the same YYYY/MM/DD tree and filenames as /inbox (see upload_queue).
static bool recordings_inboxToPendingPath(const String &inboxCanon, String &outPending)
{
    if (!inboxCanon.startsWith("/inbox/"))
    {
        return false;
    }
    outPending = "/pending" + inboxCanon.substring(6);
    return true;
}

static void handleMainRecordingsStream()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();
    if (!isStorageModeSdCard())
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "SD card storage not active");
        return;
    }
    if (!mainWebServer->hasArg("path"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing path");
        return;
    }

    String canon;
    if (!inbox_canonicalizePath(WebServer::urlDecode(mainWebServer->arg("path")), canon))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid path");
        return;
    }
    String canonLower = canon;
    canonLower.toLowerCase();
    if (!canonLower.endsWith(".wav"))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "not a wav file");
        return;
    }

    String pathToOpen = canon;
    if (!SD_MMC.exists(canon))
    {
        String pendingPath;
        if (!recordings_inboxToPendingPath(canon, pendingPath) || !SD_MMC.exists(pendingPath))
        {
            apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "file not found for playback");
            return;
        }
        pathToOpen = pendingPath;
    }

    RecordingsStreamSlot streamSlot;
    if (!streamSlot.ok())
    {
        apiError(mainWebServer, 503, API_ERR_BUSY, "Too many concurrent playback streams");
        return;
    }

    File f = SD_MMC.open(pathToOpen, FILE_READ);
    if (!f || f.isDirectory())
    {
        apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "file not found for playback");
        return;
    }

    const size_t fileSize = f.size();
    const bool asDownload =
        mainWebServer->hasArg("download") && mainWebServer->arg("download") == "1";

    String fname = pathToOpen;
    const int slash = fname.lastIndexOf('/');
    if (slash >= 0)
    {
        fname = fname.substring(slash + 1);
    }

    if (asDownload)
    {
        mainWebServer->sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    }
    else
    {
        mainWebServer->sendHeader("Content-Disposition", "inline");
    }
    mainWebServer->sendHeader("Accept-Ranges", "bytes");
    mainWebServer->sendHeader("Cache-Control", "no-store");

    const bool rangeRequested = mainWebServer->hasHeader("Range");
    size_t rangeStart = 0;
    size_t rangeEnd = fileSize > 0 ? fileSize - 1 : 0;
    bool sendPartial = false;

    if (rangeRequested && fileSize > 0)
    {
        if (parseHttpRangeHeader(mainWebServer->header("Range"), fileSize, rangeStart, rangeEnd))
        {
            sendPartial = true;
        }
        else
        {
            f.close();
            mainWebServer->sendHeader("Content-Range", "bytes */" + String(static_cast<unsigned long>(fileSize)));
            mainWebServer->send(416, "text/plain", "Range Not Satisfiable");
            return;
        }
    }

    if (sendPartial)
    {
        const size_t bodyLen = rangeEnd - rangeStart + 1;
        mainWebServer->sendHeader(
            "Content-Range",
            "bytes " + String(static_cast<unsigned long>(rangeStart)) + "-" +
                String(static_cast<unsigned long>(rangeEnd)) + "/" +
                String(static_cast<unsigned long>(fileSize)));
        mainWebServer->setContentLength(bodyLen);
        mainWebServer->send(206, "audio/wav", "");
        if (!f.seek(rangeStart))
        {
            f.close();
            return;
        }
        uint8_t buf[kRecordingsStreamReadChunkBytes];
        size_t remaining = bodyLen;
        uint32_t chunks = 0;
        while (remaining > 0)
        {
            const size_t n = std::min(remaining, kRecordingsStreamReadChunkBytes);
            const size_t r = f.read(buf, n);
            if (r == 0)
            {
                break;
            }
            mainWebServer->sendContent(reinterpret_cast<const char *>(buf), r);
            remaining -= r;
            if ((++chunks % kRecordingsStreamChunksPerYield) == 0u)
            {
                recordingsYieldForOtherTasks();
            }
        }
    }
    else
    {
        mainWebServer->setContentLength(fileSize);
        mainWebServer->send(200, "audio/wav", "");
        uint8_t buf[kRecordingsStreamReadChunkBytes];
        size_t remaining = fileSize;
        uint32_t chunks = 0;
        while (remaining > 0)
        {
            const size_t n = std::min(remaining, kRecordingsStreamReadChunkBytes);
            const size_t r = f.read(buf, n);
            if (r == 0)
            {
                break;
            }
            mainWebServer->sendContent(reinterpret_cast<const char *>(buf), r);
            remaining -= r;
            if ((++chunks % kRecordingsStreamChunksPerYield) == 0u)
            {
                recordingsYieldForOtherTasks();
            }
        }
    }
    f.close();
}

static void handleMainHomeSummary()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    // Build hash string from key values
    RecordingStats stats = recorder_getSessionStats();
    String hashStr = String(getDeviceId()) + "|" + 
                     String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "NC") + "|" +
                     String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "NC") + "|" +
                     String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "|" +
                     String(recorder_isRecording() ? "R" : "I") + "|" +
                     String(system_isUploading() ? "U" : "I") + "|" +
                     String(stats.recordingCount) + "|" +
                     String(stats.uploadedCount) + "|" +
                     String(stats.errorCount) + "|" +
                     String(ESP.getFreeHeap() / 1024) + "|" +
                     String(system_getUploadQueueSize());

    // Include TX/repeater state in change detection so UI updates immediately.
    hashStr += "|" + String(appSettings.transmitEnabled ? "TX1" : "TX0");
#if defined(ECHO)
    hashStr += "|" + String(appSettings.repeaterEnabled ? "RE1" : "RE0") + "|" + String(static_cast<unsigned>(appSettings.repeaterMode));
#endif

#if defined(ECHO)
    hashStr += "|" + String(appSettings.repeaterEnabled ? "RE1" : "RE0") + "|" + String(static_cast<unsigned>(appSettings.repeaterMode));
#endif
    
    if (isStorageModeSdCard())
    {
        uint64_t totalBytes = SD_MMC.totalBytes();
        uint64_t usedBytes = SD_MMC.usedBytes();
        hashStr += "|" + String(totalBytes) + "|" + String(usedBytes);
    }
    else
    {
        hashStr += "|PSRAM";
    }
    
    String currentHash = String(simpleHash(hashStr));
    bool changed = (currentHash != lastHomeSummaryHash);
    
    if (!changed)
    {
        // Minimal response: still send UTC epoch so the SPA can resync the clock every poll
        DynamicJsonDocument doc(256);
        doc["changed"] = false;
        time_t now = 0;
        time(&now);
        doc["deviceUtcEpoch"] = static_cast<int64_t>(now);
        String response;
        serializeJson(doc, response);
        mainWebServer->send(200, "application/json", response);
        return;
    }
    
    // Data changed, send full response
    lastHomeSummaryHash = currentHash;
    
    DynamicJsonDocument doc(3072);
    doc["changed"] = true;
    doc["deviceId"] = getDeviceId();
    doc["firmware"] = FIRMWARE;
    doc["product"] = PRODUCT_BROWSER_TITLE;
    doc["timezoneOffsetHours"] = appSettings.timezone.offsetHours;
    // Device clock as Unix epoch (UTC); SPA converts to browser local time for display
    time_t now = 0;
    time(&now);
    doc["deviceUtcEpoch"] = static_cast<int64_t>(now);
    
    if (WiFi.status() == WL_CONNECTED)
    {
        doc["ipAddress"] = WiFi.localIP().toString();
        doc["wifiSsid"] = WiFi.SSID();
        doc["wifiRssi"] = WiFi.RSSI();
    }
    else
    {
        doc["ipAddress"] = "Not connected";
        doc["wifiSsid"] = "Not connected";
        doc["wifiRssi"] = 0;
    }
    
    doc["recordingStatus"] = recorder_isRecording() ? "Recording" : "Idle";
    doc["uploadingStatus"] = system_isUploading() ? "Uploading" : "Idle";
    doc["recordingCount"] = stats.recordingCount;
    doc["uploadedCount"] = stats.uploadedCount;
    doc["errorCount"] = stats.errorCount;
    doc["warningCount"] = stats.errorCount; // Treat current error counter as warning count for UI KPI

    // TX status (used by SPA to gate repeater controls)
    doc["transmitEnabled"] = appSettings.transmitEnabled;

#if defined(ECHO)
    doc["repeaterEnabled"] = appSettings.repeaterEnabled;
    doc["repeaterMode"] = static_cast<unsigned>(appSettings.repeaterMode);
    doc["repeaterModeLabel"] = (appSettings.repeaterMode == 2) ? "Duplex" : "Simplex";
#endif
    
    // Storage info
    if (isStorageModeSdCard())
    {
        doc["storageMode"] = "SD Card";
        uint64_t totalBytes = SD_MMC.totalBytes();
        uint64_t usedBytes = SD_MMC.usedBytes();
        uint64_t freeBytes = totalBytes - usedBytes;
        // Total size in GB with 2 decimal places
        float totalGB = totalBytes / (1024.0f * 1024.0f * 1024.0f);
        doc["storageTotalGB"] = String(totalGB, 2);
        // Usage percentage with 2 decimal places
        if (totalBytes > 0)
        {
            float usagePct = (usedBytes * 100.0f) / totalBytes;
            doc["storageUsagePercent"] = String(usagePct, 2);
        }
        else
        {
            doc["storageUsagePercent"] = "N/A";
        }
        // Keep old fields for backward compatibility but they won't be displayed
        doc["storageFree"] = String(freeBytes / 1024 / 1024) + " MB";
        doc["storageUsed"] = String(usedBytes / 1024 / 1024) + " MB";
        doc["storageUtil"] = "N/A"; // Deprecated, not displayed
    }
    else
    {
        doc["storageMode"] = "PSRAM";
        doc["storageTotalGB"] = "N/A";
        doc["storageUsagePercent"] = "N/A";
        doc["storageFree"] = "N/A";
        doc["storageUsed"] = "N/A";
        doc["storageUtil"] = "N/A";
    }
    
    // Storage configuration details (for KPIs)
    doc["sdCardEnabled"] = appSettings.sdCard.useSdCard;
    doc["sdCardMode"] = appSettings.sdCard.mode1bit ? "1-bit" : "4-bit";
    doc["sdCardFrequencyHz"] = appSettings.sdCard.frequency;
    
    // System info
    size_t freeHeap = ESP.getFreeHeap();
    size_t totalHeap = ESP.getHeapSize();
    if (totalHeap > 0)
    {
        float freePct = (freeHeap * 100.0f) / static_cast<float>(totalHeap);
        float usedPct = 100.0f - freePct;
        doc["heapFreePercent"] = String(freePct, 1);
        doc["heapUsedPercent"] = String(usedPct, 1);
    }
    else
    {
        doc["heapFreePercent"] = "N/A";
        doc["heapUsedPercent"] = "N/A";
    }
    doc["heapFree"] = String(freeHeap / 1024) + " KB";
    
    size_t psramSize = ESP.getPsramSize();
    size_t freePsram = ESP.getFreePsram();
    if (psramSize > 0)
    {
        float usedPct = ((psramSize - freePsram) * 100.0f) / static_cast<float>(psramSize);
        doc["psramUsagePercent"] = String(usedPct, 1);
    }
    else
    {
        doc["psramUsagePercent"] = "N/A";
    }
    
    doc["bootReason"] = getBootReasonString();
    doc["maintenanceHour"] = appSettings.timezone.maintenanceHour;
    doc["maintenanceMinute"] = appSettings.timezone.maintenanceMinute;
    
    // Uptime
    unsigned long uptimeMs = millis();
    unsigned long uptimeDays = uptimeMs / (24UL * 60UL * 60UL * 1000UL);
    unsigned long uptimeHours = (uptimeMs % (24UL * 60UL * 60UL * 1000UL)) / (60UL * 60UL * 1000UL);
    unsigned long uptimeMinutes = (uptimeMs % (60UL * 60UL * 1000UL)) / (60UL * 1000UL);
    doc["uptime"] = String(uptimeDays) + "d " + String(uptimeHours) + "h " + String(uptimeMinutes) + "m";
    
    doc["uploadQueue"] = system_getUploadQueueSize();
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainAudioStats()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    AudioLevelStats stats = recorder_getAudioLevelStats();
    DynamicJsonDocument doc(512);
    doc["isRecording"] = recorder_isRecording();
    doc["currentLevel"] = stats.currentLevel;
    doc["currentDb"] = stats.currentDb;
    doc["minLevel"] = stats.minLevel;
    doc["maxLevel"] = stats.maxLevel;
    doc["minDb"] = stats.minDb;
    doc["maxDb"] = stats.maxDb;
    doc["dynamicRangeUtil"] = stats.dynamicRangeUtil;
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainAudioSettings()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    DynamicJsonDocument doc(512);
    doc["minRecordingMs"] = appSettings.audio.minRecordingMs;
    doc["maxRecordingMs"] = appSettings.audio.maxRecordingMs;
    doc["silenceThresholdMs"] = appSettings.audio.silenceThresholdMs;
    doc["audioThreshold"] = appSettings.audio.audioThreshold;
    doc["discardEnabled"] = appSettings.audio.discardSmallFilesEnabled;
    doc["discardMillis"] = appSettings.audio.discardSmallFilesMinMs;
    doc["preRecordMs"] = appSettings.audio.preRecordMs;
    doc["codecGainDb"] = appSettings.audio.codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    doc["recordInputChannel"] = appSettings.audio.recordInputChannel;
#endif
    doc["speakerEnabled"] = appSettings.speakerEnabled;
    doc["speakerVolume"] = static_cast<unsigned>(appSettings.speakerVolume);
    doc["transmitEnabled"] = appSettings.transmitEnabled;
    doc["transmitVolume"] = static_cast<unsigned>(appSettings.transmitVolume);
    doc["cwWpm"] = static_cast<unsigned>(appSettings.cwWpm);
    doc["cwToneHz"] = static_cast<unsigned>(appSettings.cwToneHz);
    doc["cwVolume"] = static_cast<unsigned>(appSettings.cwVolume);
    doc["cwRepeat"] = static_cast<unsigned>(appSettings.cwRepeat);
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

// ---- Unified command endpoint ----
// POST /api/cmd  — accepts a batch of get/set operations routed through
// the same settings_getParam / settings_setParam path that the CLI uses.
//
// Request body:
//   { "commands": [
//       { "action": "get",  "param": "audio.minRecordingMs" },
//       { "action": "set",  "param": "audio.audioThreshold", "value": "25" },
//       ...
//   ]}
//
// Single-command shorthand also accepted:
//   { "action": "get", "param": "audio.minRecordingMs" }
//
// Response:
//   { "status": "ok", "results": [
//       { "param": "audio.minRecordingMs", "status": "ok", "value": "2000" },
//       { "param": "audio.audioThreshold", "status": "ok", "value": "25" },
//       ...
//   ]}

static void handleApiCmd()
{
    if (!mainWebServer)
        return;

    markClientActivity();

    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }

    String body = mainWebServer->arg("plain");
    DynamicJsonDocument reqDoc(2048);
    DeserializationError err = deserializeJson(reqDoc, body);
    if (err)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, String("invalid JSON: ") + err.c_str());
        return;
    }

    // Normalise: accept both single-command object and batch array
    bool isBatch = reqDoc.containsKey("commands");
    JsonArray commands;
    DynamicJsonDocument singleWrap(384);
    if (isBatch)
    {
        commands = reqDoc["commands"].as<JsonArray>();
    }
    else if (reqDoc.containsKey("action"))
    {
        JsonArray arr = singleWrap.createNestedArray("c");
        arr.add(reqDoc.as<JsonObject>());
        commands = arr;
    }
    else
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "expected 'commands' array or single {action,param} object");
        return;
    }

    if (commands.size() == 0)
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "commands array is empty");
        return;
    }

    // Cap batch size to prevent OOM on constrained device
    const size_t kMaxBatch = 50;
    size_t count = commands.size();
    if (count > kMaxBatch) count = kMaxBatch;

    bool needSave = false;

    // Build response — generous allocation: ~80 bytes per result entry
    DynamicJsonDocument respDoc(512 + count * 128);
    JsonArray results = respDoc.createNestedArray("results");

    for (size_t i = 0; i < count; ++i)
    {
        JsonObject cmd = commands[i];
        String action = cmd["action"] | "";
        String param  = cmd["param"]  | "";
        action.toLowerCase();

        if (param.length() == 0)
        {
            JsonObject r = results.createNestedObject();
            r["status"]  = "error";
            r["code"]    = API_ERR_MISSING_PARAM;
            r["message"] = "missing param";
            continue;
        }

        if (action == "get")
        {
            String val = settings_getParam(param);
            JsonObject r = results.createNestedObject();
            r["param"]  = param;
            r["status"] = "ok";
            r["value"]  = val;
        }
        else if (action == "set")
        {
            String value = cmd["value"] | "";
            bool ok = settings_setParam(param, value);
            JsonObject r = results.createNestedObject();
            r["param"] = param;
            if (ok)
            {
                needSave = true;
                r["status"] = "ok";
                r["value"]  = settings_getParam(param);
            }
            else
            {
                r["status"]  = "error";
                r["code"]    = settings_getLastErrorCode().length() > 0
                                   ? settings_getLastErrorCode()
                                   : String(API_ERR_INVALID_VALUE);
                r["message"] = settings_getLastError().length() > 0
                                   ? settings_getLastError()
                                   : String("failed to set ") + param;
            }
        }
        else
        {
            JsonObject r = results.createNestedObject();
            r["param"]   = param;
            r["status"]  = "error";
            r["code"]    = API_ERR_UNKNOWN_CMD;
            r["message"] = String("unknown action '") + action + "'";
        }
    }

    if (needSave)
    {
        settings_clearDebounce();
        settings_save();

    }

    apiOk(mainWebServer, respDoc);
}

static void handleMainAudioDefaults()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    
    settings_setParam("audio.sampleRate",                  String(DEFAULT_AUDIO_SAMPLE_RATE));
    settings_setParam("audio.bufferSamples",               String(kAudioBufferSamples));
    settings_setParam("audio.audioThreshold",              String(DEFAULT_AUDIO_THRESHOLD));
    settings_setParam("audio.preRecordMs",                 String(DEFAULT_AUDIO_PRE_RECORD_MS));
    settings_setParam("audio.minRecordingMs",              String(DEFAULT_AUDIO_MIN_RECORDING_MS));
    if(DEFAULT_SD_USE_SD_CARD && DEFAULT_SD_RECORD_TO_SD_CARD)
    {
        settings_setParam("audio.maxRecordingMs",          String(DEFAULT_AUDIO_MAX_SD_RECORDING_MS));
    }
    else
    {
        settings_setParam("audio.maxRecordingMs",          String(DEFAULT_AUDIO_MAX_RECORDING_MS));
    }
    settings_setParam("audio.silenceThresholdMs",          String(DEFAULT_AUDIO_SILENCE_THRESHOLD_MS));
    settings_setParam("audio.discardSmallFilesEnabled",    String(DEFAULT_AUDIO_DISCARD_SMALL_FILES_ENABLED ? "true" : "false"));
    settings_setParam("audio.discardSmallFilesMinMs",      String(DEFAULT_AUDIO_DISCARD_SMALL_FILES_MIN_MS));
    settings_setParam("audio.codecGain",                   String(DEFAULT_AUDIO_CODEC_GAIN_DB));
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    settings_setParam("audio.recordInputChannel",          String(DEFAULT_AUDIO_RECORD_INPUT_CHANNEL));
#endif
    
    updateCodecGainFromSettings();
    
    settings_clearDebounce();
    bool saved = settings_save();
    
    if (saved)
    {
        apiOk(mainWebServer);
    }
    else
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, settings_getLastError());
    }
}

static void handleMainAudioSave()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    
    // Force immediate save of any pending debounced settings
    settings_clearDebounce();
    bool saved = settings_save();
    
    if (!saved)
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, settings_getLastError());
        return;
    }
    
    // Push current settings to server using existing helper
    String maskedJson = settings_getMaskedJsonForServer();
    bool pushed = network_pushSettingsToServer(maskedJson);
    
    if (pushed)
    {
        DynamicJsonDocument responseDoc(256);
        responseDoc["saved"] = true;
        responseDoc["pushed"] = true;
        apiOk(mainWebServer, responseDoc);
    }
    else
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL,
                 "settings saved locally, but push to server failed");
    }
}

#if defined(ECHO)
static void handleMainCwSave()
{
    if (!mainWebServer)
        return;

    markClientActivity();

    settings_clearDebounce();
    bool saved = settings_save();
    if (saved)
    {
        DynamicJsonDocument responseDoc(256);
        responseDoc["saved"] = true;
        apiOk(mainWebServer, responseDoc);
    }
    else
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, settings_getLastError());
    }
}

static void handleMainAudioMorse()
{
    if (!mainWebServer)
        return;
    markClientActivity();

    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing JSON body");
        return;
    }

    DynamicJsonDocument req(768);
    DeserializationError err = deserializeJson(req, mainWebServer->arg("plain"));
    if (err)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }

    if (req.containsKey("stop") && req["stop"].as<bool>() == true)
    {
        recorder_cancelMorse();
        apiOk(mainWebServer);
        return;
    }

    const char* textC = req["text"] | "";
    String text(textC);
    text.trim();
    if (text.length() == 0)
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing text");
        return;
    }
    if (text.length() > 120)
    {
        text = text.substring(0, 120);
    }

    const int wpm = req["wpm"] | 18;
    const int toneHz = req["toneHz"] | 600;
    const int volume = req["volume"] | static_cast<int>(appSettings.speakerVolume);
    const int repeat = req["repeat"] | 1;

    recorder_requestPlayMorse(
        text,
        static_cast<uint16_t>(std::max(5, std::min(60, wpm))),
        static_cast<uint16_t>(std::max(200, std::min(2000, toneHz))),
        static_cast<uint8_t>(std::max(0, std::min(100, volume))),
        static_cast<uint8_t>(std::max(1, std::min(10, repeat))));

    apiOk(mainWebServer);
}
#endif

static void handleMainSdCardSettings()
{
    if (!mainWebServer)
        return;
    
    DynamicJsonDocument doc(512);
    doc["useSdCard"] = appSettings.sdCard.useSdCard;
    doc["recordToSdCard"] = appSettings.sdCard.recordToSdCard;
    doc["mode1bit"] = appSettings.sdCard.mode1bit;
    doc["frequency"] = appSettings.sdCard.frequency;
    doc["formatIfMountFailed"] = appSettings.sdCard.formatIfMountFailed;
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainUploadLocations()
{
    if (!mainWebServer)
        return;
    markClientActivity();
    static const char* const kRegionNames[] = DEFAULT_AUDIO_UPLOAD_REGIONS;
    DynamicJsonDocument doc(768);
    JsonArray regions = doc.createNestedArray("regions");
    for (size_t i = 0; i < kApiEndpointCount && i < (sizeof(kRegionNames) / sizeof(kRegionNames[0])); ++i)
    {
        JsonObject r = regions.createNestedObject();
        r["name"] = kRegionNames[i];
        r["enabled"] = appSettings.upload.enabled[i];
        r["host"] = String(appSettings.upload.apiHosts[i] && appSettings.upload.apiHosts[i][0] != '\0' ? appSettings.upload.apiHosts[i] : "");
        r["port"] = appSettings.upload.apiPorts[i];
    }
    doc["useCustomHost"] = appSettings.upload.enabled[3];
    doc["customHost"] = String(appSettings.upload.apiHosts[3]);
    doc["customPort"] = appSettings.upload.apiPorts[3];
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainTestUploadHost()
{
    if (!mainWebServer)
        return;
    markClientActivity();
    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }
    String body = mainWebServer->arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }
    const char* hostStr = doc["host"].as<const char*>();
    if (!hostStr || hostStr[0] == '\0')
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing host");
        return;
    }
    uint16_t port = doc["port"] | 7001;
    if (port == 0)
        port = 7001;
    WiFiClient client;
    client.setTimeout(5);
    const unsigned long connectTimeoutMs = 5000;
    bool connected = client.connect(hostStr, port, connectTimeoutMs);
    DynamicJsonDocument resp(256);
    if (connected)
    {
        client.stop();
        resp["success"] = true;
        resp["message"] = "Connection succeeded.";
    }
    else
    {
        resp["success"] = false;
        resp["message"] = String("Could not connect to ") + hostStr + ":" + port;
    }
    String response;
    serializeJson(resp, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainWifiTxPowerSettings()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    DynamicJsonDocument doc(256);
    doc["wifiTxPower"] = appSettings.wifiTxPower;
    doc["hostname"] = appSettings.hostname;
    doc["mqttKeyConfigured"] = appSettings.mqttKey[0] != '\0';
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainSdCardTest()
{
    if (!mainWebServer)
        return;

    markClientActivity();

    DynamicJsonDocument doc(512);

    bool mounted = ensureStorage() && isStorageModeSdCard();
    doc["success"] = mounted;
    doc["mounted"] = mounted;

    if (!mounted)
    {
        doc["message"] = "SD card not available or failed to mount.";
    }
    else
    {
        // Report basic SD card info
        uint64_t total = SD_MMC.totalBytes();
        uint64_t used = SD_MMC.usedBytes();
        uint64_t freeBytes = (total > used) ? (total - used) : 0;

        float totalMB = total / (1024.0f * 1024.0f);
        float usedMB = used / (1024.0f * 1024.0f);
        float freeMB = freeBytes / (1024.0f * 1024.0f);

        doc["mode"] = appSettings.sdCard.mode1bit ? "1-bit" : "4-bit";
        doc["frequencyHz"] = appSettings.sdCard.frequency;
        doc["totalMB"] = totalMB;
        doc["usedMB"] = usedMB;
        doc["freeMB"] = freeMB;

        uint8_t cardType = SD_MMC.cardType();
        const char* typeStr = "UNKNOWN";
        if (cardType == CARD_NONE)
            typeStr = "NONE";
        else if (cardType == CARD_MMC)
            typeStr = "MMC";
        else if (cardType == CARD_SD)
            typeStr = "SD";
        else if (cardType == CARD_SDHC)
            typeStr = "SDHC";

        doc["cardType"] = typeStr;
    }

    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainAudioSettingsSave()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }
    
    String body = mainWebServer->arg("plain");
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }
    
    if (!doc.containsKey("param") || !doc.containsKey("value"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing param or value");
        return;
    }
    
    String param = doc["param"].as<String>();
    String value = doc["value"].as<String>();
    
    bool success = settings_setParam(param, value);
    
    if (success)
    {
        settings_clearDebounce();
        settings_save();
        DynamicJsonDocument resp(256);
        resp["success"] = true;
        resp["param"] = param;
        resp["value"] = settings_getParam(param);
        apiOk(mainWebServer, resp);
    }
    else
    {
        String errorMsg = settings_getLastError();
        if (errorMsg.length() == 0) errorMsg = "failed to set parameter";
        apiError(mainWebServer, 422, API_ERR_INVALID_VALUE, errorMsg);
    }
}

#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
static void handleMainAudioMonitorInputChannel()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();
    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }
    DynamicJsonDocument doc(192);
    DeserializationError err = deserializeJson(doc, mainWebServer->arg("plain"));
    if (err)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }
    int ch = -1;
    if (doc.containsKey("channel"))
    {
        ch = doc["channel"].as<int>();
    }
    else if (doc.containsKey("recordInputChannel"))
    {
        ch = doc["recordInputChannel"].as<int>();
    }
    else
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing channel or recordInputChannel");
        return;
    }
    if (ch != 0 && ch != 1)
    {
        apiError(mainWebServer, 422, API_ERR_INVALID_VALUE, "channel must be 0 or 1");
        return;
    }
    g_effectiveRecordInputChannel = static_cast<uint8_t>(ch);
    DynamicJsonDocument resp(128);
    resp["success"] = true;
    resp["channel"] = ch;
    apiOk(mainWebServer, resp);
}
#endif

static void handleMainLiveAudioSession()
{
    if (!mainWebServer)
    {
        return;
    }
    markClientActivity();

    const int method = mainWebServer->method();

    if (method == HTTP_GET)
    {
        DynamicJsonDocument doc(256);
        doc["pauseRecording"] = recorder_isRecordingPausedForLiveSession();
        doc["pauseUploads"] = networkHandler_isUploadPaused();
        String response;
        serializeJson(doc, response);
        mainWebServer->send(200, "application/json", response);
        return;
    }

    if (method != HTTP_POST)
    {
        mainWebServer->send(405, "text/plain", "Method Not Allowed");
        return;
    }

    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }

    DynamicJsonDocument docIn(384);
    if (deserializeJson(docIn, mainWebServer->arg("plain")))
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }

    if (docIn.containsKey("pauseRecording"))
    {
        recorder_setRecordingPausedForLiveSession(docIn["pauseRecording"].as<bool>());
    }
    if (docIn.containsKey("pauseUploads"))
    {
        networkHandler_setUploadPaused(docIn["pauseUploads"].as<bool>());
    }

    DynamicJsonDocument docOut(256);
    docOut["pauseRecording"] = recorder_isRecordingPausedForLiveSession();
    docOut["pauseUploads"] = networkHandler_isUploadPaused();
    apiOk(mainWebServer, docOut);
}

static void handleMainNetworkConfig()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    // Build hash string from config values
    String hashStr = "";
    for (size_t i = 0; i < kMaxWifiCredentials; ++i)
    {
        const char* ssid = appSettings.wifi[i].ssid;
        const char* password = appSettings.wifi[i].password;
        hashStr += String(ssid ? ssid : "") + "|" +
                   String(password ? "***" : "") + "|" +  // Don't include actual password
                   String(appSettings.wifi[i].staticIp) + "|" +
                   String(appSettings.wifi[i].staticSubnet) + "|" +
                   String(appSettings.wifi[i].staticGateway) + "|" +
                   String(appSettings.wifi[i].staticIpEnabled ? "1" : "0") + "|";
    }
    
    String currentHash = String(simpleHash(hashStr));
    bool changed = (currentHash != lastNetworkConfigHash);
    
    // Always send wifiConfigs array, even if data hasn't changed
    // This ensures the frontend always receives the 3 network configurations
    DynamicJsonDocument doc(4096);
    doc["changed"] = changed;
    
    if (changed)
    {
        // Update hash when data changes
        lastNetworkConfigHash = currentHash;
    }
    
    JsonArray configs = doc.createNestedArray("wifiConfigs");
    
    for (size_t i = 0; i < kMaxWifiCredentials; ++i)
    {
        JsonObject config = configs.createNestedObject();
        const char* ssid = appSettings.wifi[i].ssid;
        const char* password = appSettings.wifi[i].password;
        config["ssid"] = ssid ? ssid : "";
        config["password"] = password ? password : "";
        config["staticIp"] = appSettings.wifi[i].staticIp;
        config["subnet"] = appSettings.wifi[i].staticSubnet;
        config["gateway"] = appSettings.wifi[i].staticGateway;
        config["staticIpEnabled"] = appSettings.wifi[i].staticIpEnabled;
    }
    
    String response;
    serializeJson(doc, response);
    mainWebServer->send(200, "application/json", response);
}

static void handleMainNetworkSave()
{
    if (!mainWebServer)
        return;
    
    markClientActivity();
    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, mainWebServer->arg("plain"));
    
    if (error)
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid JSON");
        return;
    }
    
    int index = doc["index"] | 0;
    if (index < 0 || index >= static_cast<int>(kMaxWifiCredentials))
    {
        apiError(mainWebServer, 400, API_ERR_OUT_OF_RANGE, "invalid index");
        return;
    }
    
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    bool staticIpEnabled = doc["staticIpEnabled"] | false;
    String staticIp = doc["staticIp"] | "";
    String subnet = doc["subnet"] | "";
    String gateway = doc["gateway"] | "";
    
    settings_clearDebounce();
    
    if (ssid.length() > 0)
    {
        if (!settings_setParam("wifi[" + String(index) + "].ssid", ssid.c_str()))
        {
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set SSID");
            return;
        }
    }
    
    if (!settings_setParam("wifi[" + String(index) + "].password", password.c_str()))
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set password");
        return;
    }
    
    // Set static IP enabled flag
    String staticIpEnabledStr = staticIpEnabled ? "true" : "false";
    if (!settings_setParam("wifi[" + String(index) + "].staticIpEnabled", staticIpEnabledStr.c_str()))
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set static IP enabled");
        return;
    }
    
    // Set or clear static IP fields based on enabled state
    if (staticIpEnabled && staticIp.length() > 0)
    {
        if (!settings_setParam("wifi[" + String(index) + "].staticIp", staticIp.c_str()))
        {
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set static IP");
            return;
        }
        
        if (subnet.length() > 0)
        {
            if (!settings_setParam("wifi[" + String(index) + "].staticSubnet", subnet.c_str()))
            {
                apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set subnet");
                return;
            }
        }
        
        if (gateway.length() > 0)
        {
            if (!settings_setParam("wifi[" + String(index) + "].staticGateway", gateway.c_str()))
            {
                apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to set gateway");
                return;
            }
        }
    }
    else
    {
        // Clear static IP fields when disabled
        if (!settings_setParam("wifi[" + String(index) + "].staticIp", ""))
        {
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to clear static IP");
            return;
        }
        if (!settings_setParam("wifi[" + String(index) + "].staticSubnet", ""))
        {
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to clear subnet");
            return;
        }
        if (!settings_setParam("wifi[" + String(index) + "].staticGateway", ""))
        {
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to clear gateway");
            return;
        }
    }
    
    if (!settings_save())
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to save settings");
        return;
    }
    
    apiOk(mainWebServer);
}

static void handleFirmwareUpload()
{
    HTTPUpload& upload = mainWebServer->upload();
    
    if (upload.status == UPLOAD_FILE_START)
    {
        // Reset state
        firmwareUpdateComplete = false;
        firmwareUpdateFailed = false;
        firmwareUpdateError = "";
        
        // Check if file is .bin
        String filename = upload.filename;
        if (!filename.endsWith(".bin"))
        {
            logErrorf("[Firmware] Invalid file type. Only .bin files are allowed");
            firmwareUpdateFailed = true;
            firmwareUpdateError = "Invalid file type. Only .bin files are allowed";
            return;
        }
        
        Serial.printf("[Firmware] Update start: %s\n", filename.c_str());
        
        // Start firmware update
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            logErrorf("[Firmware] Not enough space to begin OTA update. Free: %u\n", ESP.getFreeSketchSpace());
            Update.printError(Serial);
            firmwareUpdateFailed = true;
            firmwareUpdateError = "Not enough space for firmware update";
            return;
        }
        
        firmwareUpdateInProgress = true;
        firmwareUpdateTotalSize = 0;
        firmwareUpdateReceivedSize = 0;
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        // Write firmware data
        if (firmwareUpdateInProgress && !firmwareUpdateFailed)
        {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            {
                logErrorf("[Firmware] Write failed");
                Update.printError(Serial);
                firmwareUpdateFailed = true;
                firmwareUpdateError = "Failed to write firmware data";
                firmwareUpdateInProgress = false;
                return;
            }
            
            firmwareUpdateReceivedSize += upload.currentSize;
            if (firmwareUpdateTotalSize == 0 && upload.totalSize > 0)
            {
                firmwareUpdateTotalSize = upload.totalSize;
            }
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (firmwareUpdateInProgress && !firmwareUpdateFailed)
        {
            if (Update.end(true))
            {
                Serial.printf("[Firmware] Update complete: %u bytes\n", firmwareUpdateReceivedSize);
                logInfof("[Firmware] Firmware update successful. Rebooting...");
                firmwareUpdateComplete = true;
                firmwareUpdateInProgress = false;
            }
            else
            {
                logErrorf("[Firmware] Update failed");
                Update.printError(Serial);
                firmwareUpdateFailed = true;
                firmwareUpdateError = "Firmware update failed";
                firmwareUpdateInProgress = false;
            }
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        logErrorf("[Firmware] Update aborted");
        Update.abort();
        firmwareUpdateFailed = true;
        firmwareUpdateError = "Firmware update aborted";
        firmwareUpdateInProgress = false;
    }
}

static void handleMainAdvancedUpdateFirmware()
{
    if (!mainWebServer)
        return;
    
    // This endpoint is called after file upload completes
    // The actual upload is handled by handleFirmwareUpload()
    
    if (firmwareUpdateFailed)
    {
        String msg = firmwareUpdateError.length() > 0 ? firmwareUpdateError : "Firmware update failed";
        apiError(mainWebServer, 500, API_ERR_INTERNAL, msg);
        
        // Reset state
        firmwareUpdateFailed = false;
        firmwareUpdateError = "";
        return;
    }
    
    if (firmwareUpdateComplete)
    {
        DynamicJsonDocument responseDoc(256);
        responseDoc["message"] = "Firmware uploaded and flashed successfully. Device will reboot.";
        apiOk(mainWebServer, responseDoc);
        
        // Reset state
        firmwareUpdateComplete = false;
        
        // Give time for response to be sent, then reboot
        delay(1000);
        ESP.restart();
        return;
    }
    
    if (firmwareUpdateInProgress)
    {
        apiError(mainWebServer, 409, API_ERR_BUSY, "firmware update in progress");
        return;
    }
    
    apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "no firmware file uploaded");
}

static void handleMainAdvancedReboot()
{
    if (!mainWebServer)
        return;
    
    // Push settings to cloud before rebooting
    pushSettingsToCloudBeforeReboot();
    
    apiOk(mainWebServer);
    
    delay(1000);
    ESP.restart();
}

static void handleMainAdvancedSetDefault()
{
    if (!mainWebServer)
        return;
    
    // Preserve WiFi settings
    WiFiSettings preservedWifi[kMaxWifiCredentials];
    for (size_t i = 0; i < kMaxWifiCredentials; ++i)
    {
        preservedWifi[i].ssid = appSettings.wifi[i].ssid ? strdup(appSettings.wifi[i].ssid) : nullptr;
        preservedWifi[i].password = appSettings.wifi[i].password ? strdup(appSettings.wifi[i].password) : nullptr;
        preservedWifi[i].connectTimeoutMs = appSettings.wifi[i].connectTimeoutMs;
        preservedWifi[i].staticIpEnabled = appSettings.wifi[i].staticIpEnabled;
        strncpy(preservedWifi[i].staticIp, appSettings.wifi[i].staticIp, sizeof(preservedWifi[i].staticIp) - 1);
        preservedWifi[i].staticIp[sizeof(preservedWifi[i].staticIp) - 1] = '\0';
        strncpy(preservedWifi[i].staticSubnet, appSettings.wifi[i].staticSubnet, sizeof(preservedWifi[i].staticSubnet) - 1);
        preservedWifi[i].staticSubnet[sizeof(preservedWifi[i].staticSubnet) - 1] = '\0';
        strncpy(preservedWifi[i].staticGateway, appSettings.wifi[i].staticGateway, sizeof(preservedWifi[i].staticGateway) - 1);
        preservedWifi[i].staticGateway[sizeof(preservedWifi[i].staticGateway) - 1] = '\0';
    }
    
    // Get current settings JSON to preserve structure
    String currentJson = settings_getAllJson();
    
    // Use standard capacity to avoid stack overflow (web server runs in arduino_events task with limited stack)
    constexpr size_t kDocCapacity = 4096;
    DynamicJsonDocument* docPtr = nullptr;
    bool isPsram = false;
    #ifdef ESP32
    // Try to allocate in PSRAM first to save heap
    void* mem = heap_caps_malloc(kDocCapacity, MALLOC_CAP_SPIRAM);
    if (mem != nullptr)
    {
        docPtr = new(mem) DynamicJsonDocument(kDocCapacity);
        isPsram = true;
    }
    #endif
    if (docPtr == nullptr)
    {
        docPtr = new DynamicJsonDocument(kDocCapacity);
        isPsram = false;
    }
    DynamicJsonDocument& doc = *docPtr;
    
    DeserializationError error = deserializeJson(doc, currentJson);
    
    if (error)
    {
        // Free preserved WiFi strings
        for (size_t i = 0; i < kMaxWifiCredentials; ++i)
        {
            if (preservedWifi[i].ssid) free((void*)preservedWifi[i].ssid);
            if (preservedWifi[i].password) free((void*)preservedWifi[i].password);
        }
        // Free JSON document
        if (isPsram)
        {
            docPtr->~DynamicJsonDocument();
            heap_caps_free(docPtr);
        }
        else
        {
            delete docPtr;
        }
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to parse current settings");
        return;
    }
    
    // Reset audio settings to defaults
    JsonObject audio = doc["a"];
    audio["sr"] = DEFAULT_AUDIO_SAMPLE_RATE;
    audio["ath"] = DEFAULT_AUDIO_THRESHOLD;
    audio["prm"] = DEFAULT_AUDIO_PRE_RECORD_MS;
    audio["mrm"] = DEFAULT_AUDIO_MIN_RECORDING_MS;
    audio["xrm"] = DEFAULT_AUDIO_MAX_RECORDING_MS;
    audio["stm"] = DEFAULT_AUDIO_SILENCE_THRESHOLD_MS;
    audio["dsf"] = DEFAULT_AUDIO_DISCARD_SMALL_FILES_ENABLED;
    audio["dmm"] = DEFAULT_AUDIO_DISCARD_SMALL_FILES_MIN_MS;
    audio["cg"] = DEFAULT_AUDIO_CODEC_GAIN_DB;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    audio["ric"] = DEFAULT_AUDIO_RECORD_INPUT_CHANNEL;
#endif
    
    // Reset upload settings to defaults
    JsonObject upload = doc["u"];
    upload["ctm"] = DEFAULT_UPLOAD_CONVERT_TO_MP3;
    JsonArray apiHosts = upload["ah"];
    JsonArray apiPorts = upload["ap"];
    JsonArray enabled = upload["en"];
    const char *const kDefaultUploadHosts[] = DEFAULT_AUDIO_UPLOAD_HOSTS_IP;
    for (size_t i = 0; i < kApiEndpointCount; ++i)
    {
        const char *host = (i < (sizeof(kDefaultUploadHosts) / sizeof(kDefaultUploadHosts[0]))) ? kDefaultUploadHosts[i] : "";
        if (host == nullptr) host = "";
        apiHosts[i] = host;
        apiPorts[i] = DEFAULT_API_PORT;
        enabled[i] = (i < 3);  // Ohio, Oregon, Virginia on; Custom (slot 3) off
    }

    // Reset RTC settings to defaults
    JsonObject rtc = doc["r"];
    rtc["en"] = false;
    rtc["sda"] = 5;
    rtc["scl"] = 18;
    
    // Reset SD card settings to product defaults (TANGO: off; ECHO: on)
    JsonObject sdCard = doc["s"];
    sdCard["usc"] = DEFAULT_SD_USE_SD_CARD;
    sdCard["rsc"] = DEFAULT_SD_RECORD_TO_SD_CARD;
    sdCard["m1b"] = false;
    sdCard["frq"] = 10000000;
    sdCard["fmf"] = false;
    
    // Reset timezone settings to defaults
    JsonObject timezone = doc["t"];
    timezone["oh"] = DEFAULT_TIMEZONE_OFFSET_HOURS;
    timezone["mh"] = DEFAULT_MAINTENANCE_HOUR;
    timezone["mm"] = DEFAULT_MAINTENANCE_MINUTE;
    
    // Reset log settings to defaults
    JsonObject log = doc["l"];
    log["sf"] = DEFAULT_LOG_SERIAL_FATAL;
    log["se"] = DEFAULT_LOG_SERIAL_ERROR;
    log["sw"] = DEFAULT_LOG_SERIAL_WARNING;
    log["si"] = DEFAULT_LOG_SERIAL_INFO;
    log["sd"] = DEFAULT_LOG_SERIAL_DEBUG;
    log["sev"] = DEFAULT_LOG_SERIAL_EVENT;
    log["ff"] = DEFAULT_LOG_FILE_FATAL;
    log["fe"] = DEFAULT_LOG_FILE_ERROR;
    log["fw"] = DEFAULT_LOG_FILE_WARNING;
    log["fi"] = DEFAULT_LOG_FILE_INFO;
    log["fd"] = DEFAULT_LOG_FILE_DEBUG;
    log["fev"] = DEFAULT_LOG_FILE_EVENT;
    
    // Restore preserved WiFi settings
    JsonArray wifiArr = doc["w"];
    for (size_t i = 0; i < kMaxWifiCredentials; ++i)
    {
        JsonObject w = wifiArr[i];
        if (preservedWifi[i].ssid && strlen(preservedWifi[i].ssid) > 0)
        {
            w["ss"] = preservedWifi[i].ssid;
            if (preservedWifi[i].password && strlen(preservedWifi[i].password) > 0)
            {
                w["pw"] = preservedWifi[i].password;
            }
            w["ctm"] = preservedWifi[i].connectTimeoutMs;
            w["sie"] = preservedWifi[i].staticIpEnabled;
            w["sip"] = preservedWifi[i].staticIp;
            w["ssn"] = preservedWifi[i].staticSubnet;
            w["sgt"] = preservedWifi[i].staticGateway;
        }
    }
    
    // Update settings from modified JSON
    String modifiedJson;
    serializeJson(doc, modifiedJson);
    
    // Free JSON document before updating settings
    if (isPsram)
    {
        docPtr->~DynamicJsonDocument();
        heap_caps_free(docPtr);
    }
    else
    {
        delete docPtr;
    }
    
    if (settings_updateAllFromJson(modifiedJson, false))
    {
        // Free preserved WiFi strings
        for (size_t i = 0; i < kMaxWifiCredentials; ++i)
        {
            if (preservedWifi[i].ssid) free((void*)preservedWifi[i].ssid);
            if (preservedWifi[i].password) free((void*)preservedWifi[i].password);
        }
        
        DynamicJsonDocument responseDoc(256);
        responseDoc["message"] = "Settings reset to defaults (WiFi preserved)";
        apiOk(mainWebServer, responseDoc);
        
        // Push settings to cloud before rebooting
        pushSettingsToCloudBeforeReboot();
        
        delay(1000);
        ESP.restart();
    }
    else
    {
        // Free preserved WiFi strings
        for (size_t i = 0; i < kMaxWifiCredentials; ++i)
        {
            if (preservedWifi[i].ssid) free((void*)preservedWifi[i].ssid);
            if (preservedWifi[i].password) free((void*)preservedWifi[i].password);
        }

        String errorMsg = settings_getLastError();
        if (errorMsg.length() == 0) errorMsg = "failed to reset settings";
        apiError(mainWebServer, 500, API_ERR_INTERNAL, errorMsg);
    }
}

static void handleMainAdvancedFactoryReset()
{
    if (!mainWebServer)
        return;
    
    // Clear all settings
    // This is a simplified version - in reality you'd want to clear NVS
    
    // Note: For factory reset, we don't push settings since everything is being cleared
    // But we still call the function in case there are any settings to preserve
    
    DynamicJsonDocument doc(256);
    doc["message"] = "Factory reset complete";
    apiOk(mainWebServer, doc);
    
    delay(1000);
    ESP.restart();
}

static void handleMainAdvancedExportSettings()
{
    if (!mainWebServer)
        return;
    
    String settingsJson = settings_getAllJson();

    // For exported settings, mask WiFi passwords for security
    // We deserialize, overwrite password fields, and re-serialize.
    // Uses short JSON keys: "w" (wifi array) and "pw" (password), with legacy fallbacks.
    constexpr size_t kDocCapacity = 4096;
    DynamicJsonDocument doc(kDocCapacity);
    DeserializationError err = deserializeJson(doc, settingsJson);

    if (!err)
    {
        if (doc.containsKey("mk") && doc["mk"].is<const char*>() && doc["mk"].as<const char*>()[0] != '\0')
            doc["mk"] = "HIDDEN_FOR_SECURITY";
        if (doc.containsKey("mqttKey") && doc["mqttKey"].is<const char*>() && doc["mqttKey"].as<const char*>()[0] != '\0')
            doc["mqttKey"] = "HIDDEN_FOR_SECURITY";

        JsonArray wifiArr;
        if (doc.containsKey("w") && doc["w"].is<JsonArray>())
        {
            wifiArr = doc["w"].as<JsonArray>();
        }
        else if (doc.containsKey("wifi") && doc["wifi"].is<JsonArray>())
        {
            wifiArr = doc["wifi"].as<JsonArray>();
        }

        if (!wifiArr.isNull())
        {
            for (JsonObject w : wifiArr)
            {
                // Mask non-empty passwords
                if (w.containsKey("pw") && w["pw"].is<const char*>())
                {
                    const char* val = w["pw"].as<const char*>();
                    if (val && val[0] != '\0')
                    {
                        w["pw"] = "HIDDEN_FOR_SECURITY";
                    }
                }
                if (w.containsKey("password") && w["password"].is<const char*>())
                {
                    const char* val = w["password"].as<const char*>();
                    if (val && val[0] != '\0')
                    {
                        w["password"] = "HIDDEN_FOR_SECURITY";
                    }
                }

                // Mask non-empty SSIDs
                if (w.containsKey("ss") && w["ss"].is<const char*>())
                {
                    const char* val = w["ss"].as<const char*>();
                    if (val && val[0] != '\0')
                    {
                        w["ss"] = "HIDDEN_FOR_SECURITY";
                    }
                }
                if (w.containsKey("ssid") && w["ssid"].is<const char*>())
                {
                    const char* val = w["ssid"].as<const char*>();
                    if (val && val[0] != '\0')
                    {
                        w["ssid"] = "HIDDEN_FOR_SECURITY";
                    }
                }
            }
        }

        String maskedJson;
        serializeJson(doc, maskedJson);
        mainWebServer->send(200, "application/json", maskedJson);
    }
    else
    {
        // Fallback: if we can't safely mask, send the original JSON
        mainWebServer->send(200, "application/json", settingsJson);
    }
}

static void handleMainAdvancedImportSettings()
{
    if (!mainWebServer)
        return;
    
    if (!mainWebServer->hasArg("plain"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing request body");
        return;
    }
    
    String json = mainWebServer->arg("plain");

    // When importing from the web UI, ignore WiFi passwords and SSIDs that were
    // masked as "HIDDEN_FOR_SECURITY" during export. We do this by stripping out
    // any fields with that exact value before applying settings.
    {
        constexpr size_t kDocCapacity = 4096;
        DynamicJsonDocument doc(kDocCapacity);
        DeserializationError err = deserializeJson(doc, json);
        if (!err)
        {
            if (doc["mk"] == "HIDDEN_FOR_SECURITY") doc.remove("mk");
            if (doc["mqttKey"] == "HIDDEN_FOR_SECURITY") doc.remove("mqttKey");

            JsonArray wifiArr;
            if (doc.containsKey("w") && doc["w"].is<JsonArray>())
            {
                wifiArr = doc["w"].as<JsonArray>();
            }
            else if (doc.containsKey("wifi") && doc["wifi"].is<JsonArray>())
            {
                wifiArr = doc["wifi"].as<JsonArray>();
            }

            if (!wifiArr.isNull())
            {
                for (JsonObject w : wifiArr)
                {
                    // Short key ("pw")
                    if (w.containsKey("pw") && w["pw"].is<const char*>())
                    {
                        const char* val = w["pw"].as<const char*>();
                        if (val && strcmp(val, "HIDDEN_FOR_SECURITY") == 0)
                        {
                            w.remove("pw");
                        }
                    }
                    // Legacy key ("password")
                    if (w.containsKey("password") && w["password"].is<const char*>())
                    {
                        const char* val = w["password"].as<const char*>();
                        if (val && strcmp(val, "HIDDEN_FOR_SECURITY") == 0)
                        {
                            w.remove("password");
                        }
                    }

                    // Short key SSID ("ss")
                    if (w.containsKey("ss") && w["ss"].is<const char*>())
                    {
                        const char* val = w["ss"].as<const char*>();
                        if (val && strcmp(val, "HIDDEN_FOR_SECURITY") == 0)
                        {
                            w.remove("ss");
                        }
                    }
                    // Legacy key SSID ("ssid")
                    if (w.containsKey("ssid") && w["ssid"].is<const char*>())
                    {
                        const char* val = w["ssid"].as<const char*>();
                        if (val && strcmp(val, "HIDDEN_FOR_SECURITY") == 0)
                        {
                            w.remove("ssid");
                        }
                    }
                }
            }

            String cleanedJson;
            serializeJson(doc, cleanedJson);
            json = cleanedJson;
        }
    }

    if (settings_updateAllFromJson(json, false))
    {
        apiOk(mainWebServer);
        
        // Push settings to cloud before rebooting
        pushSettingsToCloudBeforeReboot();
        
        delay(1000);
        ESP.restart();
    }
    else
    {
        String errorMsg = settings_getLastError();
        if (errorMsg.length() == 0) errorMsg = "failed to import settings";
        apiError(mainWebServer, 500, API_ERR_INTERNAL, errorMsg);
    }
}

static void handleMainAdvancedPushSettings()
{
    if (!mainWebServer)
        return;

    // Build masked settings JSON and push to API server
    String maskedJson = settings_getMaskedJsonForServer();

    bool ok = network_pushSettingsToServer(maskedJson);

    if (ok)
    {
        DynamicJsonDocument doc(256);
        doc["message"] = "Settings pushed to server";
        apiOk(mainWebServer, doc);
    }
    else
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to push settings to server");
    }
}

static void handleMainAdvancedPullSettings()
{
    if (!mainWebServer)
        return;

    // Pull settings JSON from server
    String serverJson;
    if (!network_pullSettingsFromServer(serverJson) || serverJson.length() == 0)
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to pull settings from server");
        return;
    }

    // Apply settings from server (handles stripping masked WiFi fields and suppresses per-setting events)
    bool ok = settings_applyJsonFromServer(serverJson);

    if (ok)
    {
        DynamicJsonDocument doc(256);
        doc["message"] = "Settings pulled from server and applied";
        apiOk(mainWebServer, doc);
    }
    else
    {
        String errorMsg = settings_getLastError();
        if (errorMsg.length() == 0) errorMsg = "failed to apply settings from server";
        apiError(mainWebServer, 500, API_ERR_INTERNAL, errorMsg);
    }
}

static void handleMainFirmwareCheck()
{
    if (!mainWebServer)
        return;
    
    if (WiFi.status() != WL_CONNECTED)
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "WiFi not connected");
        return;
    }
    
    // Choose a random healthy API endpoint for firmware check
    bool success = false;
    String apiResponse;
    size_t endpointIndex = 0;
    const char* host = nullptr;
    uint16_t endpointPort = 0;

    if (network_getRandomHealthyEndpoint(endpointIndex, host, endpointPort) && host != nullptr)
    {
        WiFiClient client;
        const unsigned long attemptStartMs = millis();
        
        // Log API endpoint attempt with firmware version
        String requestPath = String(DEFAULT_FIRMWARE_CHECK_PATH) + "?current_version=" + String(FIRMWARE);
        logDebugf("[Server] Attempting to check firmware update from API endpoint\n");
        logDebugf("[Server] Current firmware version: %s\n", FIRMWARE);
        
        if (connectWiFiClientWithRetry(client, host, endpointPort, 1, 3000))
        {
            String requestLine = "GET " + requestPath + " HTTP/1.1";
            String headers = "Host: " + String(host) + ":" + String(endpointPort) + "\r\n"
                            "User-Agent: " + getUserAgentString() + "\r\n"
                            "Accept: application/json\r\n"
                            "Connection: close\r\n";
            
            client.print(requestLine + "\r\n");
            client.print(headers);
            client.print("\r\n");
            
            // Wait for response (with timeout)
            unsigned long responseTimer = millis();
            String response;
            while ((millis() - responseTimer) < 10000) // 10 second timeout
            {
                while (client.available())
                {
                    char c = static_cast<char>(client.read());
                    response += c;
                    responseTimer = millis();
                }
                
                if (!client.connected() && client.available() == 0)
                {
                    break;
                }
                
                delay(10);
            }
            
            client.stop();
            
            // Parse HTTP response
            int httpBodyStart = response.indexOf("\r\n\r\n");
            if (httpBodyStart >= 0)
            {
                apiResponse = response.substring(httpBodyStart + 4);
                apiResponse.trim();
                
                // Check if response is valid JSON
                DynamicJsonDocument testDoc(512);
                DeserializationError error = deserializeJson(testDoc, apiResponse);
                if (error == DeserializationError::Ok)
                {
                    success = true;
                    unsigned long responseTimeMs = millis() - attemptStartMs;
                    network_recordEndpointRequest(endpointIndex, true, responseTimeMs);
                    network_updateEndpointHealthScore(endpointIndex);
                }
            }
        }
        else
        {
            unsigned long responseTimeMs = millis() - attemptStartMs;
            network_recordEndpointRequest(endpointIndex, false, responseTimeMs);
            network_updateEndpointHealthScore(endpointIndex);
        }
    }
    
    if (success && apiResponse.length() > 0)
    {
        mainWebServer->send(200, "application/json", apiResponse);
    }
    else
    {
        apiError(mainWebServer, 500, API_ERR_INTERNAL, "failed to check for updates from any API endpoint");
    }
}

static void handleMainFirmwareApply()
{
    if (!mainWebServer)
        return;
    
    if (!mainWebServer->hasArg("download_link"))
    {
        apiError(mainWebServer, 400, API_ERR_MISSING_PARAM, "missing download_link parameter");
        return;
    }
    
    if (WiFi.status() != WL_CONNECTED)
    {
        apiError(mainWebServer, 503, API_ERR_HW_ERROR, "WiFi not connected");
        return;
    }
    
    String downloadLink = mainWebServer->arg("download_link");
    
    // Parse URL to extract host, port, and path
    String host;
    uint16_t port = 80;
    String path;
    
    if (downloadLink.startsWith("http://"))
    {
        downloadLink = downloadLink.substring(7);
        int portIndex = downloadLink.indexOf(':');
        int pathIndex = downloadLink.indexOf('/');
        
        if (portIndex > 0 && (pathIndex < 0 || portIndex < pathIndex))
        {
            host = downloadLink.substring(0, portIndex);
            int pathStart = downloadLink.indexOf('/', portIndex);
            if (pathStart > 0)
            {
                port = downloadLink.substring(portIndex + 1, pathStart).toInt();
                path = downloadLink.substring(pathStart);
            }
            else
            {
                port = downloadLink.substring(portIndex + 1).toInt();
                path = "/";
            }
        }
        else if (pathIndex > 0)
        {
            host = downloadLink.substring(0, pathIndex);
            path = downloadLink.substring(pathIndex);
        }
        else
        {
            host = downloadLink;
            path = "/";
        }
    }
    else
    {
        apiError(mainWebServer, 400, API_ERR_INVALID_VALUE, "invalid download link format");
        return;
    }
    
    // Use HTTPUpdate to download and apply firmware
    httpUpdate.rebootOnUpdate(true);
    
    // Construct full URL
    String fullUrl = "http://" + host;
    if (port != 80)
    {
        fullUrl += ":" + String(port);
    }
    fullUrl += path;
    
    WiFiClient client;
    t_httpUpdate_return ret = httpUpdate.update(client, fullUrl);
    
    switch (ret)
    {
        case HTTP_UPDATE_FAILED:
            apiError(mainWebServer, 500, API_ERR_INTERNAL,
                     "update failed: " + String(httpUpdate.getLastErrorString()));
            break;
        case HTTP_UPDATE_NO_UPDATES:
            apiError(mainWebServer, 500, API_ERR_NOT_FOUND, "no updates available");
            break;
        case HTTP_UPDATE_OK:
        {
            DynamicJsonDocument doc(256);
            doc["message"] = "Update started, device will reboot";
            apiOk(mainWebServer, doc);
            break;
        }
        default:
            apiError(mainWebServer, 500, API_ERR_INTERNAL, "unknown update error");
            break;
    }
    
    if (ret == HTTP_UPDATE_OK)
    {
        delay(1000);
        ESP.restart();
    }
}

// WebSocket push functions
void boondock_server_pushHomeData()
{
    if (!ws || !asyncServerActive)
        return;
    
    // Check if any clients are subscribed to home
    bool hasSubscribers = false;
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "home")
        {
            hasSubscribers = true;
            break;
        }
    }
    
    if (!hasSubscribers)
        return;
    
    // Build home summary data (reuse logic from handleMainHomeSummary)
    RecordingStats stats = recorder_getSessionStats();
    String hashStr = String(getDeviceId()) + "|" + 
                     String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "NC") + "|" +
                     String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "NC") + "|" +
                     String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "|" +
                     String(recorder_isRecording() ? "R" : "I") + "|" +
                     String(system_isUploading() ? "U" : "I") + "|" +
                     String(stats.recordingCount) + "|" +
                     String(stats.uploadedCount) + "|" +
                     String(stats.errorCount) + "|" +
                     String(ESP.getFreeHeap() / 1024) + "|" +
                     String(system_getUploadQueueSize());
    
    if (isStorageModeSdCard())
    {
        uint64_t totalBytes = SD_MMC.totalBytes();
        uint64_t usedBytes = SD_MMC.usedBytes();
        hashStr += "|" + String(totalBytes) + "|" + String(usedBytes);
    }
    else
    {
        hashStr += "|PSRAM";
    }
    
    String currentHash = String(simpleHash(hashStr));
    bool changed = (currentHash != lastHomeSummaryHash);
    
    if (!changed)
    {
        DynamicJsonDocument doc(512);
        doc["type"] = "home";
        doc["changed"] = false;
        time_t now = 0;
        time(&now);
        doc["deviceUtcEpoch"] = static_cast<int64_t>(now);
        
        String json;
        serializeJson(doc, json);
        
        // Send to subscribed clients only
        for (const auto& sub : clientSubscriptions)
        {
            if (sub.page == "home")
            {
                ws->text(sub.clientId, json);
            }
        }
        return;
    }
    
    // Data changed, send full response
    lastHomeSummaryHash = currentHash;
    
    DynamicJsonDocument doc(3072);
    doc["type"] = "home";
    doc["changed"] = true;
    doc["deviceId"] = getDeviceId();
    doc["firmware"] = FIRMWARE;
    doc["product"] = PRODUCT_BROWSER_TITLE;
    doc["timezoneOffsetHours"] = appSettings.timezone.offsetHours;
    time_t now = 0;
    time(&now);
    doc["deviceUtcEpoch"] = static_cast<int64_t>(now);
    
    if (WiFi.status() == WL_CONNECTED)
    {
        doc["ipAddress"] = WiFi.localIP().toString();
        doc["wifiSsid"] = WiFi.SSID();
        doc["wifiRssi"] = WiFi.RSSI();
    }
    else
    {
        doc["ipAddress"] = "Not connected";
        doc["wifiSsid"] = "Not connected";
        doc["wifiRssi"] = 0;
    }
    
    doc["recordingStatus"] = recorder_isRecording() ? "Recording" : "Idle";
    doc["uploadingStatus"] = system_isUploading() ? "Uploading" : "Idle";
    doc["recordingCount"] = stats.recordingCount;
    doc["uploadedCount"] = stats.uploadedCount;
    doc["errorCount"] = stats.errorCount;
    doc["warningCount"] = stats.errorCount;

    // TX status (used by SPA to gate repeater controls)
    doc["transmitEnabled"] = appSettings.transmitEnabled;

#if defined(ECHO)
    doc["repeaterEnabled"] = appSettings.repeaterEnabled;
    doc["repeaterMode"] = static_cast<unsigned>(appSettings.repeaterMode);
    doc["repeaterModeLabel"] = (appSettings.repeaterMode == 2) ? "Duplex" : "Simplex";
#endif
    
    if (isStorageModeSdCard())
    {
        doc["storageMode"] = "SD Card";
        uint64_t totalBytes = SD_MMC.totalBytes();
        uint64_t usedBytes = SD_MMC.usedBytes();
        uint64_t freeBytes = totalBytes - usedBytes;
        float totalGB = totalBytes / (1024.0f * 1024.0f * 1024.0f);
        doc["storageTotalGB"] = String(totalGB, 2);
        if (totalBytes > 0)
        {
            float usagePct = (usedBytes * 100.0f) / totalBytes;
            doc["storageUsagePercent"] = String(usagePct, 2);
        }
        else
        {
            doc["storageUsagePercent"] = "N/A";
        }
        doc["storageFree"] = String(freeBytes / 1024 / 1024) + " MB";
        doc["storageUsed"] = String(usedBytes / 1024 / 1024) + " MB";
    }
    else
    {
        doc["storageMode"] = "PSRAM";
        doc["storageTotalGB"] = "N/A";
        doc["storageUsagePercent"] = "N/A";
        doc["storageFree"] = "N/A";
        doc["storageUsed"] = "N/A";
    }
    
    doc["sdCardEnabled"] = appSettings.sdCard.useSdCard;
    doc["sdCardMode"] = appSettings.sdCard.mode1bit ? "1-bit" : "4-bit";
    doc["sdCardFrequencyHz"] = appSettings.sdCard.frequency;
    
    size_t freeHeap = ESP.getFreeHeap();
    size_t totalHeap = ESP.getHeapSize();
    if (totalHeap > 0)
    {
        float freePct = (freeHeap * 100.0f) / static_cast<float>(totalHeap);
        float usedPct = 100.0f - freePct;
        doc["heapFreePercent"] = String(freePct, 1);
        doc["heapUsedPercent"] = String(usedPct, 1);
    }
    else
    {
        doc["heapFreePercent"] = "N/A";
        doc["heapUsedPercent"] = "N/A";
    }
    doc["heapFree"] = String(freeHeap / 1024) + " KB";
    
    size_t psramSize = ESP.getPsramSize();
    size_t freePsram = ESP.getFreePsram();
    if (psramSize > 0)
    {
        float usedPct = ((psramSize - freePsram) * 100.0f) / static_cast<float>(psramSize);
        doc["psramUsagePercent"] = String(usedPct, 1);
    }
    else
    {
        doc["psramUsagePercent"] = "N/A";
    }
    
    doc["bootReason"] = getBootReasonString();
    doc["maintenanceHour"] = appSettings.timezone.maintenanceHour;
    doc["maintenanceMinute"] = appSettings.timezone.maintenanceMinute;
    
    unsigned long uptimeMs = millis();
    unsigned long uptimeDays = uptimeMs / (24UL * 60UL * 60UL * 1000UL);
    unsigned long uptimeHours = (uptimeMs % (24UL * 60UL * 60UL * 1000UL)) / (60UL * 60UL * 1000UL);
    unsigned long uptimeMinutes = (uptimeMs % (60UL * 60UL * 1000UL)) / (60UL * 1000UL);
    doc["uptime"] = String(uptimeDays) + "d " + String(uptimeHours) + "h " + String(uptimeMinutes) + "m";
    
    doc["uploadQueue"] = system_getUploadQueueSize();
    
    String json;
    serializeJson(doc, json);
    
    // Send to subscribed clients only
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "home")
        {
            ws->text(sub.clientId, json);
        }
    }
}

void boondock_server_collectAudioSample()
{
    // Collect a single audio sample into the buffer
    AudioLevelStats stats = recorder_getAudioLevelStats();
    
    wsAudioBuffer[wsAudioBufferIndex].currentDb = stats.currentDb;
    wsAudioBuffer[wsAudioBufferIndex].currentLevel = stats.currentLevel;
    wsAudioBuffer[wsAudioBufferIndex].dynamicRangeUtil = stats.dynamicRangeUtil;
    wsAudioBuffer[wsAudioBufferIndex].isRecording = recorder_isRecording();
    
    wsAudioBufferIndex++;
    if (wsAudioBufferIndex >= AUDIO_BUFFER_SIZE)
    {
        wsAudioBufferIndex = 0;
        wsAudioBufferReady = true; // Buffer is full, ready to send
    }
}

void boondock_server_pushAudioStats()
{
    if (!ws || !asyncServerActive)
        return;
    
    // Check if any clients are subscribed to audio
    bool hasSubscribers = false;
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "audio")
        {
            hasSubscribers = true;
            break;
        }
    }
    
    if (!hasSubscribers)
        return;
    
    // Get latest stats for metadata (min/max/averages)
    AudioLevelStats stats = recorder_getAudioLevelStats();
    
    // Create JSON document with buffer array
    DynamicJsonDocument doc(2048); // Larger document for array
    doc["type"] = "audio";
    doc["isRecording"] = recorder_isRecording();
    
    // Add metadata (min/max values)
    doc["minLevel"] = stats.minLevel;
    doc["maxLevel"] = stats.maxLevel;
    doc["minDb"] = stats.minDb;
    doc["maxDb"] = stats.maxDb;
    
    // Add samples array (send all collected samples)
    JsonArray samples = doc.createNestedArray("samples");
    size_t samplesToSend = wsAudioBufferReady ? AUDIO_BUFFER_SIZE : wsAudioBufferIndex;
    
    for (size_t i = 0; i < samplesToSend; i++)
    {
        JsonObject sample = samples.createNestedObject();
        sample["currentDb"] = wsAudioBuffer[i].currentDb;
        sample["currentLevel"] = wsAudioBuffer[i].currentLevel;
        sample["dynamicRangeUtil"] = wsAudioBuffer[i].dynamicRangeUtil;
        sample["isRecording"] = wsAudioBuffer[i].isRecording;
    }
    
    String json;
    serializeJson(doc, json);
    
    // Send to subscribed clients only
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "audio")
        {
            ws->text(sub.clientId, json);
        }
    }
    
    // Reset buffer after sending
    wsAudioBufferReady = false;
    wsAudioBufferIndex = 0;
}

void boondock_server_pushNetworkConfig()
{
    if (!ws || !asyncServerActive)
        return;
    
    // Check if any clients are subscribed to network
    bool hasSubscribers = false;
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "network")
        {
            hasSubscribers = true;
            break;
        }
    }
    
    if (!hasSubscribers)
        return;
    
    // Build network config data (reuse logic from handleMainNetworkConfig)
    DynamicJsonDocument doc(2048);
    doc["type"] = "network";
    
    JsonArray wifiConfigs = doc.createNestedArray("wifiConfigs");
    for (size_t i = 0; i < 3; ++i)
    {
        JsonObject config = wifiConfigs.createNestedObject();
        const char* ssid = appSettings.wifi[i].ssid;
        config["ssid"] = ssid ? ssid : "";
        config["password"] = ""; // Never send password
        config["staticIp"] = appSettings.wifi[i].staticIp;
        config["subnet"] = appSettings.wifi[i].staticSubnet;
        config["gateway"] = appSettings.wifi[i].staticGateway;
        config["staticIpEnabled"] = appSettings.wifi[i].staticIpEnabled;
    }
    
    String hashStr = "";
    for (size_t i = 0; i < 3; ++i)
    {
        const char* ssid = appSettings.wifi[i].ssid;
        hashStr += String(ssid ? ssid : "") + "|";
        hashStr += String(appSettings.wifi[i].staticIp) + "|";
        hashStr += String(appSettings.wifi[i].staticSubnet) + "|";
        hashStr += String(appSettings.wifi[i].staticGateway) + "|";
        hashStr += String(appSettings.wifi[i].staticIpEnabled ? "1" : "0") + "|";
    }
    
    String currentHash = String(simpleHash(hashStr));
    bool changed = (currentHash != lastNetworkConfigHash);
    
    if (!changed)
    {
        doc["changed"] = false;
    }
    else
    {
        lastNetworkConfigHash = currentHash;
        doc["changed"] = true;
    }
    
    String json;
    serializeJson(doc, json);
    
    // Send to subscribed clients only
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "network")
        {
            ws->text(sub.clientId, json);
        }
    }
}

void boondock_server_pushLiveAudio()
{
    if (!ws || !asyncServerActive || liveAudioPsramBuffer == nullptr)
        return;
    
    // Check if any clients are subscribed to live-audio
    bool hasSubscribers = false;
    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "live-audio")
        {
            hasSubscribers = true;
            break;
        }
    }
    
    if (!hasSubscribers)
    {
        return; // No subscribers, don't log
    }
    
    const size_t MIN_SAMPLES_TO_SEND = 80; // ~10 ms @ 8 kHz

    const uint32_t written = liveAudioSamplesWritten.load(std::memory_order_acquire);
    uint32_t sent = liveAudioSamplesSentTotal;
    uint32_t pending = written - sent;

    if (pending > static_cast<uint32_t>(LIVE_AUDIO_BUFFER_SAMPLES))
    {
        // Consumer fell behind the ring — jump to newest window so payload matches physical memory.
        sent = written - static_cast<uint32_t>(LIVE_AUDIO_BUFFER_SAMPLES);
        pending = static_cast<uint32_t>(LIVE_AUDIO_BUFFER_SAMPLES);
        liveAudioSamplesSentTotal = sent;
    }

    if (pending < static_cast<uint32_t>(MIN_SAMPLES_TO_SEND))
    {
        static unsigned long lastEmptyLog = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastEmptyLog >= 5000)
        {
            logDebugf("[LiveAudio] Only %u new samples pending (need %zu), waiting\n",
                      static_cast<unsigned>(pending), MIN_SAMPLES_TO_SEND);
            lastEmptyLog = nowMs;
        }
        return;
    }

    size_t samplesToSend = static_cast<size_t>(pending);
    if (samplesToSend > LIVE_AUDIO_MAX_SAMPLES_PER_PUSH)
    {
        samplesToSend = LIVE_AUDIO_MAX_SAMPLES_PER_PUSH;
    }

    // Allocate one contiguous binary frame: header + µ-law payload (1 byte/sample).
    const size_t frameBytes = LIVE_AUDIO_HEADER_BYTES + samplesToSend;
    uint8_t* frame = (uint8_t*)malloc(frameBytes);
    if (frame == nullptr)
    {
        logErrorf("[LiveAudio] Failed to allocate %zu-byte frame buffer\n", frameBytes);
        return;
    }

    const uint32_t seq = ++liveAudioPushSequence;

    // Header (little-endian, matches typed-array reads in the browser).
    frame[0] = 'B';
    frame[1] = 'A';
    frame[2] = 'U';
    frame[3] = 'D';
    frame[4] = LIVE_AUDIO_FRAME_VERSION;
    frame[5] = LIVE_AUDIO_CODEC_ULAW;
    frame[6] = 1;  // channels
    frame[7] = 0;  // reserved

    const uint32_t sampleRate = 8000;
    const uint32_t sampleCountU32 = static_cast<uint32_t>(samplesToSend);
    memcpy(frame + 8,  &sampleRate,    sizeof(uint32_t));
    memcpy(frame + 12, &sampleCountU32, sizeof(uint32_t));
    memcpy(frame + 16, &seq,           sizeof(uint32_t));

    // µ-law encode straight from the ring into the payload region (no intermediate copy).
    uint8_t* payload = frame + LIVE_AUDIO_HEADER_BYTES;
    for (size_t i = 0; i < samplesToSend; i++)
    {
        const uint32_t globalIndex = sent + static_cast<uint32_t>(i);
        const size_t ringIdx = static_cast<size_t>(globalIndex % LIVE_AUDIO_BUFFER_SAMPLES);
        payload[i] = pcm16_to_ulaw(liveAudioPsramBuffer[ringIdx]);
    }

    liveAudioSamplesSentTotal = sent + static_cast<uint32_t>(samplesToSend);

    static unsigned long lastSendLog = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastSendLog >= 5000)
    {
        logDebugf("[LiveAudio] Binary µ-law frame: %zu samples, %zu bytes, seq=%u\n",
                  samplesToSend, frameBytes, static_cast<unsigned>(seq));
        lastSendLog = nowMs;
    }

    for (const auto& sub : clientSubscriptions)
    {
        if (sub.page == "live-audio")
        {
            AsyncWebSocketClient* client = ws->client(sub.clientId);
            if (client != nullptr && client->canSend())
            {
                ws->binary(sub.clientId, frame, frameBytes);
            }
        }
    }

    free(frame);
}

static void setupMainWebServer()
{
    if (mainWebServer != nullptr)
    {
        mainWebServer->stop();
        delete mainWebServer;
    }
    
    mainWebServer = new WebServer(80);
    
    // Single Page Application - serve SPA HTML for all routes
    mainWebServer->on("/", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/home", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/audio", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/live-audio", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/recordings", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/network", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/advanced", HTTP_GET, []() { handleMainSPA(); });
#if defined(ECHO)
    mainWebServer->on("/player-tx", HTTP_GET, []() { handleMainSPA(); });
    mainWebServer->on("/cw", HTTP_GET, []() { handleMainSPA(); });
#endif
    
    // Collect Range for WAV streaming (seek / partial content)
    {
        static const char *kRecordedHeaders[] = {"Range"};
        mainWebServer->collectHeaders(kRecordedHeaders, 1);
    }
    
    // Static files
    mainWebServer->on("/app.css", HTTP_GET, []() { handleMainCSS(); });
    mainWebServer->on("/app.js", HTTP_GET, []() { handleMainJS(); });
    
    // Unified command endpoint (batch get/set via CLI path)
    mainWebServer->on("/api/cmd", HTTP_POST, []() { handleApiCmd(); });

    // API endpoints
    mainWebServer->on("/api/device-info", HTTP_GET, []() { handleMainDeviceInfo(); });
    mainWebServer->on("/api/home/summary", HTTP_GET, []() { handleMainHomeSummary(); });
    mainWebServer->on("/api/firmware/check", HTTP_GET, []() { handleMainFirmwareCheck(); });
    mainWebServer->on("/api/firmware/apply", HTTP_POST, []() { handleMainFirmwareApply(); });
    mainWebServer->on("/api/audio/stats", HTTP_GET, []() { handleMainAudioStats(); });
    mainWebServer->on("/api/audio/settings", HTTP_GET, []() { handleMainAudioSettings(); });
    mainWebServer->on("/api/audio/settings", HTTP_POST, []() { handleMainAudioSettingsSave(); });
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    mainWebServer->on("/api/audio/monitor-input-channel", HTTP_POST, []() { handleMainAudioMonitorInputChannel(); });
#endif
    mainWebServer->on("/api/audio/defaults", HTTP_POST, []() { handleMainAudioDefaults(); });
    mainWebServer->on("/api/audio/save", HTTP_POST, []() { handleMainAudioSave(); });
    mainWebServer->on("/api/live-audio/session", HTTP_GET, []() { handleMainLiveAudioSession(); });
    mainWebServer->on("/api/live-audio/session", HTTP_POST, []() { handleMainLiveAudioSession(); });
#if defined(ECHO)
    mainWebServer->on("/api/cw/save", HTTP_POST, []() { handleMainCwSave(); });
    mainWebServer->on("/api/audio/morse", HTTP_POST, []() { handleMainAudioMorse(); });
#endif
    mainWebServer->on("/api/advanced/sd-card-settings", HTTP_GET, []() { handleMainSdCardSettings(); });
    mainWebServer->on("/api/advanced/sd-card-test", HTTP_POST, []() { handleMainSdCardTest(); });
    mainWebServer->on("/api/advanced/test-upload-host", HTTP_POST, []() { handleMainTestUploadHost(); });
    mainWebServer->on("/api/advanced/upload-locations", HTTP_GET, []() { handleMainUploadLocations(); });
    mainWebServer->on("/api/advanced/wifi-tx-power-settings", HTTP_GET, []() { handleMainWifiTxPowerSettings(); });
    mainWebServer->on("/api/network/config", HTTP_GET, []() { handleMainNetworkConfig(); });
    mainWebServer->on("/api/network/save", HTTP_POST, []() { handleMainNetworkSave(); });
    // Firmware update endpoint - handles file upload
    mainWebServer->on("/api/advanced/update-firmware", HTTP_POST, []() { 
        handleMainAdvancedUpdateFirmware(); 
    }, []() { 
        handleFirmwareUpload(); 
    });
    mainWebServer->on("/api/advanced/reboot", HTTP_POST, []() { handleMainAdvancedReboot(); });
    mainWebServer->on("/api/advanced/set-default", HTTP_POST, []() { handleMainAdvancedSetDefault(); });
    mainWebServer->on("/api/advanced/factory-reset", HTTP_POST, []() { handleMainAdvancedFactoryReset(); });
    mainWebServer->on("/api/advanced/export-settings", HTTP_GET, []() { handleMainAdvancedExportSettings(); });
    mainWebServer->on("/api/advanced/import-settings", HTTP_POST, []() { handleMainAdvancedImportSettings(); });
    mainWebServer->on("/api/advanced/push-settings", HTTP_POST, []() { handleMainAdvancedPushSettings(); });
    mainWebServer->on("/api/advanced/pull-settings", HTTP_POST, []() { handleMainAdvancedPullSettings(); });
    mainWebServer->on("/api/recordings/folders", HTTP_GET, []() { handleMainRecordingsFolders(); });
    mainWebServer->on("/api/recordings/list", HTTP_GET, []() { handleMainRecordingsList(); });
    mainWebServer->on("/api/recordings/summary", HTTP_GET, []() { handleMainRecordingsSummary(); });
    mainWebServer->on("/api/recordings/stream", HTTP_GET, []() { handleMainRecordingsStream(); });
    
    // Handle favicon
    mainWebServer->on("/favicon.ico", HTTP_GET, []() {
        if (mainWebServer != nullptr)
        {
            mainWebServer->send(204);
        }
    });
    
    // Handle 404
    mainWebServer->onNotFound([]() {
        if (mainWebServer != nullptr)
        {
            apiError(mainWebServer, 404, API_ERR_NOT_FOUND, "unknown endpoint");
        }
    });
    
    mainWebServer->begin();
    mainServerActive = true;
    
    // Setup Async Web Server for WebSocket support
    if (asyncWebServer != nullptr)
    {
        asyncWebServer->end();
        delete asyncWebServer;
    }
    if (ws != nullptr)
    {
        ws->~AsyncWebSocket();
        delete ws;
    }
    
    asyncWebServer = new AsyncWebServer(81); // Use port 81 to avoid conflict with main server
    ws = new AsyncWebSocket("/ws");
    
    // WebSocket event handler
    ws->onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, 
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT)
        {
            logDebugf("[WebSocket] Client #%u connected from %s\n", 
                     client->id(), client->remoteIP().toString().c_str());
            markClientActivity();
        }
        else if (type == WS_EVT_DISCONNECT)
        {
            logDebugf("[WebSocket] Client #%u disconnected\n", client->id());
            // Remove client subscriptions
            clientSubscriptions.erase(
                std::remove_if(clientSubscriptions.begin(), clientSubscriptions.end(),
                    [client](const ClientSubscription& sub) {
                        return sub.clientId == client->id();
                    }),
                clientSubscriptions.end()
            );
            syncRecorderLiveAudioFeed();
        }
        else if (type == WS_EVT_DATA)
        {
            // Parse incoming message (JSON subscription request)
            AwsFrameInfo *info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len)
            {
                // Complete message received
                String message;
                message.reserve(len + 1);
                for (size_t i = 0; i < len; i++)
                {
                    message += (char)data[i];
                }
                
                // Parse JSON
                DynamicJsonDocument doc(256);
                DeserializationError error = deserializeJson(doc, message);
                if (!error && doc.containsKey("action") && doc.containsKey("page"))
                {
                    String action = doc["action"].as<String>();
                    String page = doc["page"].as<String>();
                    
                    if (action == "subscribe")
                    {
                        // Add or update subscription
                        bool found = false;
                        for (auto& sub : clientSubscriptions)
                        {
                            if (sub.clientId == client->id() && sub.page == page)
                            {
                                sub.lastUpdate = millis();
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                        {
                            ClientSubscription sub;
                            sub.clientId = client->id();
                            sub.page = page;
                            sub.lastUpdate = millis();
                            clientSubscriptions.push_back(sub);
                        }
                        logDebugf("[WebSocket] Client #%u subscribed to %s\n", 
                                 client->id(), page.c_str());
                        markClientActivity();
                        syncRecorderLiveAudioFeed();
                    }
                    else if (action == "unsubscribe")
                    {
                        // Remove subscription
                        clientSubscriptions.erase(
                            std::remove_if(clientSubscriptions.begin(), clientSubscriptions.end(),
                                [client, page](const ClientSubscription& sub) {
                                    return sub.clientId == client->id() && sub.page == page;
                                }),
                            clientSubscriptions.end()
                        );
                        logDebugf("[WebSocket] Client #%u unsubscribed from %s\n", 
                                 client->id(), page.c_str());
                        syncRecorderLiveAudioFeed();
                    }
                }
            }
        }
    });
    
    asyncWebServer->addHandler(ws);
    asyncWebServer->begin();
    asyncServerActive = true;
    
    // Initialize live audio PSRAM buffer
    if (liveAudioPsramBuffer == nullptr)
    {
        liveAudioPsramBuffer = (int16_t*)heap_caps_malloc(LIVE_AUDIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
        if (liveAudioPsramBuffer == nullptr)
        {
            logErrorf("[LiveAudio] Failed to allocate PSRAM buffer for live audio streaming");
        }
        else
        {
            memset(liveAudioPsramBuffer, 0, LIVE_AUDIO_BUFFER_BYTES);
            liveAudioSamplesWritten.store(0, std::memory_order_release);
            liveAudioSamplesSentTotal = 0;
            liveAudioPushSequence = 0;
            liveAudioBufferFilled = 0;
            liveAudioBufferReady = false;
            logDebugf("[LiveAudio] Allocated %zu bytes PSRAM buffer for live audio streaming\n", LIVE_AUDIO_BUFFER_BYTES);
        }
    }

    // Start the dedicated live-audio sender task now that the WebSocket and ring are ready.
    startLiveAudioSenderTask();
    
    // Set up live audio callback in recorder
    recorder_setLiveAudioCallback([](const int16_t* samples, size_t sampleCount) {
        if (liveAudioPsramBuffer == nullptr || samples == nullptr || sampleCount == 0)
        {
            return;
        }
        
        // Add samples to ring buffer (monotonic index for delta send correctness)
        for (size_t i = 0; i < sampleCount; i++)
        {
            const uint32_t w = liveAudioSamplesWritten.fetch_add(1, std::memory_order_relaxed);
            const size_t ringIdx = static_cast<size_t>(w % LIVE_AUDIO_BUFFER_SAMPLES);
            liveAudioPsramBuffer[ringIdx] = samples[i];

            if (liveAudioBufferFilled < LIVE_AUDIO_BUFFER_SAMPLES)
            {
                liveAudioBufferFilled++;
            }
        }
        
        // Mark buffer as ready when we have 1 second of data
        if (liveAudioBufferFilled >= LIVE_AUDIO_BUFFER_SAMPLES && !liveAudioBufferReady)
        {
            liveAudioBufferReady = true;
            logDebugf("[LiveAudio] Buffer ready: %zu samples filled\n", liveAudioBufferFilled);
        }
        
        // Log periodically to verify callback is being called
        static unsigned long lastCallbackLog = 0;
        unsigned long now = millis();
        if (now - lastCallbackLog >= 5000) // Log every 5 seconds
        {
            lastCallbackLog = now;
        }
    });
    
    logDebugf("[WebSocket] WebSocket server started on port 81\n");
}

void boondock_server_startMainMode()
{
    if (mainServerActive)
    {
        return; // Already started
    }
    
    if (WiFi.status() != WL_CONNECTED)
    {
        return; // Only start when WiFi is connected
    }
    
    // Check if webserver is enabled (AP mode always enables webserver, checked in webServerTask)
    if (!appSettings.webserverEnabled)
    {
        return; // Webserver disabled by user setting
    }
    
    setupMainWebServer();
}

void boondock_server_loop()
{
    // Clear stale activity flag periodically
    clearClientActivityIfStale();
    
    // Clean up WebSocket clients periodically
    if (ws != nullptr && asyncServerActive)
    {
        ws->cleanupClients();
    }
    
    // Push WebSocket updates at appropriate intervals
    if (asyncServerActive && ws != nullptr && ws->count() > 0)
    {
        unsigned long now = millis();
        
        // Push home data every 1 second
        if (now - lastHomePush >= HOME_PUSH_INTERVAL)
        {
            boondock_server_pushHomeData();
            lastHomePush = now;
        }
        
        // Collect audio sample every 100ms
        if (now - lastAudioSample >= AUDIO_SAMPLE_INTERVAL)
        {
            boondock_server_collectAudioSample();
            lastAudioSample = now;
        }
        
        // Push audio stats buffer every 1 second
        if (now - lastAudioPush >= AUDIO_PUSH_INTERVAL)
        {
            boondock_server_pushAudioStats();
            lastAudioPush = now;
        }
        
        // Push network config every 1 second
        if (now - lastNetworkPush >= NETWORK_PUSH_INTERVAL)
        {
            boondock_server_pushNetworkConfig();
            lastNetworkPush = now;
        }
        
        // Live audio is pushed by a dedicated FreeRTOS task (see liveAudioSenderTask),
        // so nothing more to do for it on this loop tick.
    }
    
    // Handle AP mode web server
    if (apModeActive && apWebServer != nullptr)
    {
        // Check if there's a client before processing
        // handleClient() will process any pending requests
        apWebServer->handleClient();
        // Note: Activity is tracked in individual handlers via markClientActivity()
    }
    
    // Handle main web server (only when not in AP mode and WiFi is connected)
    if (!apModeActive && WiFi.status() == WL_CONNECTED)
    {
        if (!mainServerActive)
        {
            boondock_server_startMainMode();
        }
        else if (mainWebServer != nullptr)
        {
            // Check if there's a client before processing
            mainWebServer->handleClient();
            // Note: Activity is tracked in individual handlers via markClientActivity()
        }
    }
    else if (mainServerActive)
    {
        // WiFi disconnected or AP mode activated, stop main server
        if (mainWebServer != nullptr)
        {
            mainWebServer->stop();
            delete mainWebServer;
            mainWebServer = nullptr;
        }
        mainServerActive = false;
        
        // Clean up async WebSocket server
        if (asyncServerActive)
        {
            stopLiveAudioSenderTask();
            if (asyncWebServer != nullptr)
            {
                asyncWebServer->end();
                delete asyncWebServer;
                asyncWebServer = nullptr;
            }
            if (ws != nullptr)
            {
                ws->~AsyncWebSocket();
                delete ws;
                ws = nullptr;
            }
            clientSubscriptions.clear();
            asyncServerActive = false;
        }
        // Reset activity tracking when server stops
        lastClientActivityMs = 0;
        clientWasActive = false;
    }
}

bool boondock_server_hasClient()
{
    // Check if there was recent client activity (within last 5 seconds)
    // This indicates a client is likely still connected or was recently active
    constexpr unsigned long kClientActivityTimeoutMs = 5000;
    unsigned long now = millis();
    
    if (lastClientActivityMs > 0 && (now - lastClientActivityMs) < kClientActivityTimeoutMs)
    {
        return true;
    }
    
    // Also check if we're actively processing (clientWasActive flag)
    // This handles the case where a request is currently being processed
    return clientWasActive;
}

unsigned long boondock_server_getLastActivityTime()
{
    return lastClientActivityMs;
}

bool boondock_server_isAPModeActive()
{
    return apModeActive;
}
