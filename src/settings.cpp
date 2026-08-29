#include "settings.h"
#include "main.h"
#include "common.h"
#include "recorder.h"
#include "network.h"
#include "timekeeper.h"
#include "nvs.h"
#include "config.h"
#include "health.h"
#include "system_assets.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#ifdef ESP32
#include <esp_wifi.h>
#endif
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include "logger.h"
#if defined(ECHO)
#include "mqtt_task.h"
#endif

// Serial mutex to prevent interleaving of CLI output with periodic status messages
static SemaphoreHandle_t g_serialMutex = nullptr;
static constexpr TickType_t SERIAL_MUTEX_TIMEOUT_MS = pdMS_TO_TICKS(1000);

// Initialize Serial mutex
static void ensureSerialMutex()
{
    if (g_serialMutex == nullptr)
    {
        g_serialMutex = xSemaphoreCreateMutex();
        if (g_serialMutex == nullptr)
        {
        }
    }
}

// Acquire Serial mutex (returns true if acquired)
static bool acquireSerialMutex()
{
    ensureSerialMutex();
    if (g_serialMutex == nullptr) return false;
    
    BaseType_t result = xSemaphoreTake(g_serialMutex, SERIAL_MUTEX_TIMEOUT_MS);
    if (result != pdTRUE)
    {
        // Don't log here to avoid recursion - just fail silently
        return false;
    }
    return true;
}

// Release Serial mutex
static void releaseSerialMutex()
{
    if (g_serialMutex != nullptr)
    {
        xSemaphoreGive(g_serialMutex);
    }
}

// Get Serial mutex for use in other files (e.g., main.cpp)
SemaphoreHandle_t settings_getSerialMutex()
{
    ensureSerialMutex();
    return g_serialMutex;
}
#include <FS.h>
#include <SD_MMC.h>
#include <time.h>
#include <sys/time.h>
#ifdef ESP32
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

static constexpr uint32_t kAllowedSampleRates[] = {8000};
static constexpr size_t kMinBufferSamples = 512;
static constexpr size_t kMaxBufferSamples = 4096;
static constexpr size_t kSettingsJsonDocCapacity = 4096;

// NVS settings namespace
static constexpr const char* kSettingsNamespace = "settings";

// NVS keys for two-slot atomic writes
static constexpr const char* kKeySlot0 = "slot0";  // Primary slot
static constexpr const char* kKeySlot1 = "slot1";  // Secondary slot
static constexpr const char* kKeyActiveSlot = "active";  // Which slot is active (0 or 1)

// Maximum JSON size (NVS can handle larger values, but we keep it reasonable)
static constexpr size_t kMaxJsonSize = 3500;  // Leave room for CRC and metadata

// Short JSON key names (2-4 characters) to reduce storage size
namespace JsonKeys {
    // Top-level keys
    constexpr const char* FIRMWARE_KEY = "fw";
    constexpr const char* CONFIG_VERSION_KEY = "cv";
    constexpr const char* WIFI = "w";
    constexpr const char* AUDIO = "a";
    constexpr const char* UPLOAD = "u";
    constexpr const char* RTC = "r";
    constexpr const char* SD_CARD = "s";
    constexpr const char* TIMEZONE = "t";
    constexpr const char* LOG = "l";
    constexpr const char* CLI = "c";
    constexpr const char* WIFI_TX_POWER = "wtp";
    constexpr const char* HOSTNAME = "hn";
    constexpr const char* MQTT_KEY = "mk";
    constexpr const char* WEBSERVER_ENABLED = "wse";
    // Legacy LED compatibility settings
    constexpr const char* LED_STYLE = "ls";
    constexpr const char* STARTUP_MODE = "sm";
    constexpr const char* OFFLINE_MODE = "of";
    constexpr const char* MAC_ADDRESS = "mac";
    constexpr const char* CONNECTED_WIFI_SSID = "cws";
    constexpr const char* CONNECTED_WIFI_INDEX = "cwi";
    constexpr const char* RUNTIME = "rt";
    constexpr const char* CURRENT_IP = "cip";
    
    // WiFi fields
    constexpr const char* SSID = "ss";
    constexpr const char* PASSWORD = "pw";
    constexpr const char* CONNECT_TIMEOUT_MS = "ctm";
    constexpr const char* STATIC_IP_ENABLED = "sie";
    constexpr const char* STATIC_IP = "sip";
    constexpr const char* STATIC_SUBNET = "ssn";
    constexpr const char* STATIC_GATEWAY = "sgt";
    constexpr const char* STATIC_DNS1 = "sd1";
    constexpr const char* STATIC_DNS2 = "sd2";
    
    // Audio fields
    constexpr const char* SAMPLE_RATE = "sr";
    constexpr const char* BUFFER_SAMPLES = "bs";
    constexpr const char* AUDIO_THRESHOLD = "ath";
    constexpr const char* PRE_RECORD_MS = "prm";
    constexpr const char* MIN_RECORDING_MS = "mrm";
    constexpr const char* MAX_RECORDING_MS = "xrm";
    constexpr const char* SILENCE_THRESHOLD_MS = "stm";
    constexpr const char* DISCARD_SMALL_FILES_ENABLED = "dsf";
    constexpr const char* DISCARD_SMALL_FILES_MIN_MS = "dmm";
    constexpr const char* CODEC_GAIN = "cg";
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    constexpr const char* RECORD_INPUT_CHANNEL = "ric";
#endif

    // Speaker fields
    constexpr const char* SPEAKER_ENABLED = "se";
    constexpr const char* SPEAKER_VOLUME = "sv";

    // Transmit (TX) fields (legacy placeholder)
    constexpr const char* TRANSMIT_ENABLED = "te";
    constexpr const char* TRANSMIT_VOLUME = "tv";

#if defined(ECHO)
    // Repeater fields (ECHO-only; stored under audio object for compactness)
    constexpr const char* REPEATER_ENABLED = "re";
    constexpr const char* REPEATER_MODE = "rm"; // 1=simplex, 2=duplex
#endif

    // CW (Morse) fields (stored under audio object)
    constexpr const char* CW_WPM = "cww";
    constexpr const char* CW_TONE_HZ = "cwt";
    constexpr const char* CW_VOLUME = "cwv";
    constexpr const char* CW_REPEAT = "cwr";
    
    // Upload fields
    constexpr const char* QUEUE_DEPTH = "qd";
    constexpr const char* CONVERT_TO_MP3 = "ctm";
    constexpr const char* API_HOSTS = "ah";
    constexpr const char* API_PORTS = "ap";
    constexpr const char* ENABLED = "en";  // Used for both upload.enabled array and rtc.enabled
    constexpr const char* USE_CUSTOM_HOST = "uch";
    constexpr const char* CUSTOM_HOST = "ch";
    constexpr const char* CUSTOM_PORT = "cp";

    // RTC fields (ENABLED shared above)
    constexpr const char* SDA_PIN = "sda";
    constexpr const char* SCL_PIN = "scl";
    
    // SD Card fields
    constexpr const char* USE_SD_CARD = "usc";
    constexpr const char* RECORD_TO_SD_CARD = "rsc";
    constexpr const char* MODE_1BIT = "m1b";
    constexpr const char* FREQUENCY = "frq";
    constexpr const char* FORMAT_IF_MOUNT_FAILED = "fmf";
    
    // Timezone fields
    constexpr const char* OFFSET_HOURS = "oh";
    constexpr const char* MAINTENANCE_HOUR = "mh";
    constexpr const char* MAINTENANCE_MINUTE = "mm";
    
    // Log fields
    constexpr const char* SERIAL_FATAL = "sf";
    constexpr const char* SERIAL_ERROR = "se";
    constexpr const char* SERIAL_WARNING = "sw";
    constexpr const char* SERIAL_INFO = "si";
    constexpr const char* SERIAL_DEBUG = "sd";
    constexpr const char* SERIAL_EVENT = "sev";
    constexpr const char* FILE_FATAL = "ff";
    constexpr const char* FILE_ERROR = "fe";
    constexpr const char* FILE_WARNING = "fw";
    constexpr const char* FILE_INFO = "fi";
    constexpr const char* FILE_DEBUG = "fd";
    constexpr const char* FILE_EVENT = "fev";
    
    
    // Runtime fields
    constexpr const char* MAC_ADDRESS_RT = "mac";
    constexpr const char* WIFI_CONNECTED = "wc";
    constexpr const char* CONNECTED_SSID = "css";
    constexpr const char* RSSI = "rss";
    constexpr const char* CONNECTED_INDEX = "ci";
    constexpr const char* IP = "ip";
    constexpr const char* SUBNET = "sub";
    constexpr const char* GATEWAY = "gw";
    constexpr const char* DNS1 = "d1";
    constexpr const char* DNS2 = "d2";
}

#ifdef ESP32
// Helper: Allocate DynamicJsonDocument in PSRAM to save heap
// Returns nullptr if allocation fails (fallback to heap)
static DynamicJsonDocument* allocateJsonDocInPsram(size_t capacity)
{
    void* mem = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (mem == nullptr)
    {
        return nullptr; // Fallback to heap
    }
    return new(mem) DynamicJsonDocument(capacity);
}

// Helper: Free PSRAM-allocated JSON document
static void freeJsonDocFromPsram(DynamicJsonDocument* doc, size_t capacity)
{
    if (doc == nullptr) return;
    doc->~DynamicJsonDocument();
    heap_caps_free(doc);
}

#endif

static String g_lastSettingsError;
static String g_lastSettingsErrorCode;

// Standard error codes (shared vocabulary for CLI and API)
static const char* const ERR_UNKNOWN_CMD   = "UNKNOWN_CMD";
static const char* const ERR_INVALID_VALUE = "INVALID_VALUE";
static const char* const ERR_OUT_OF_RANGE  = "OUT_OF_RANGE";
static const char* const ERR_MISSING_PARAM = "MISSING_PARAM";
static const char* const ERR_HW_ERROR      = "HW_ERROR";
static const char* const ERR_BUSY          = "BUSY";

static void setSettingsError(const String &message)
{
    g_lastSettingsError = message;
}

static void setSettingsErrorWithCode(const char* code, const String &message)
{
    g_lastSettingsError = message;
    g_lastSettingsErrorCode = code;
}

static void clearSettingsError()
{
    g_lastSettingsError = String();
    g_lastSettingsErrorCode = String();
}

static bool isValidInteger(const String &s)
{
    if (s.length() == 0) return false;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') start = 1;
    if (start >= s.length()) return false;
    for (size_t i = start; i < s.length(); i++)
    {
        if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// --- CLI response helpers ------------------------------------------------
// All CLI responses are JSON. Every command produces exactly one JSON line
// so the test harness can parse it deterministically.

static void cliOk()
{
    Serial.println("{\"status\":\"ok\"}");
}

static void cliOk(const String &key, const String &val)
{
    DynamicJsonDocument d(256);
    d["status"] = "ok";
    d[key] = val;
    String out;
    serializeJson(d, out);
    Serial.println(out);
}

static void cliError(const char* code, const String &message)
{
    DynamicJsonDocument d(384);
    d["status"] = "error";
    d["code"] = code;
    d["message"] = message;
    String out;
    serializeJson(d, out);
    Serial.println(out);
}

static void cliJsonResponse(DynamicJsonDocument &doc)
{
    doc["status"] = "ok";
    String output;
    serializeJson(doc, output);
    output += "\n";
    Serial.write(reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
    Serial.flush();
}

// NVS settings mutex for thread safety (separate from global NVS mutex)
static SemaphoreHandle_t g_settingsMutex = nullptr;
static constexpr TickType_t SETTINGS_MUTEX_TIMEOUT_MS = pdMS_TO_TICKS(5000);

// NVS settings health monitoring
struct SettingsHealthMetrics {
    unsigned long totalWrites = 0;
    unsigned long successfulWrites = 0;
    unsigned long failedWrites = 0;
    unsigned long totalReads = 0;
    unsigned long successfulReads = 0;
    unsigned long failedReads = 0;
    unsigned long corruptionDetected = 0;
    unsigned long lastWriteMs = 0;
    unsigned long lastReadMs = 0;
    unsigned long lastFailureMs = 0;
};

static SettingsHealthMetrics g_settingsHealth = {};

// Last known good settings backup (in memory)
static String g_lastKnownGoodJson;
static bool g_hasLastKnownGood = false;

// Settings change tracking for debouncing
static unsigned long g_lastSettingsChangeMs = 0;
static bool g_pendingSettingsSave = false;
static constexpr unsigned long SETTINGS_DEBOUNCE_MS = 1000;  // Wait 1 second before saving

// Structure to track setting changes for CONFIG command
struct SettingChange {
    String key;
    String oldValue;
    String newValue;
    bool sensitive;
};

// Static list to track changes during CONFIG command
static std::vector<SettingChange> g_configChanges;
static bool g_trackingConfigChanges = false;

// When true, per-setting change events will NOT be sent to the server.
// Used to avoid event storms/loops when applying bulk updates from server.
static bool g_suppressSettingChangeEvents = false;

void settings_setChangeEventsSuppressed(bool suppressed)
{
    g_suppressSettingChangeEvents = suppressed;
}

// Forward declaration for per-setting change event sender
static void sendSettingChangeEvent(const char *key,
                                   const String &oldValue,
                                   const String &newValue,
                                   bool sensitive);

static void startTrackingConfigChanges()
{
    g_configChanges.clear();
    g_trackingConfigChanges = true;
}

static void stopTrackingConfigChanges()
{
    g_trackingConfigChanges = false;
}

static int8_t sanitizeCodecGainDb(int value)
{
    static const int8_t allowed[] = {-3, 0, 3, 6, 9, 12, 15, 18, 21, 24};
    int8_t best = allowed[0];
    int bestDiff = value - allowed[0];
    if (bestDiff < 0)
        bestDiff = -bestDiff;

    for (size_t i = 1; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
    {
        int diff = value - allowed[i];
        if (diff < 0)
            diff = -diff;
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = allowed[i];
        }
    }
    return best;
}

static uint8_t sanitizeSpeakerVolume(int value)
{
    //Edge Case: If value is 0 set voulme 0
    if (value == 0)
        return (uint8_t)value;

    // Round to nearest multiple of 10
    int remainder = value % 10;

    if (remainder >= 5)
        value += (10 - remainder); // round up
    else
        value -= remainder; // round down

    // Clamp again just in case (e.g., value < 10 edge case)
    if (value < 10)
        value = 10;

    return (uint8_t)value;
}

static uint16_t sanitizeSilenceThresholdMs(long value)
{
    // Round to nearest multiple of 100ms
    uint16_t reminder = value % 100;
    if (reminder >= 50)
        value += (100 - reminder); // round up
    else
        value -= reminder; // round down

    return static_cast<uint16_t>(value);
}

static uint32_t sanitizeSampleRate(uint32_t value)
{
    for (uint32_t rate : kAllowedSampleRates)
    {
        if (value == rate)
        {
            return rate;
        }
    }
    return DEFAULT_AUDIO_SAMPLE_RATE;
}

static size_t sanitizeBufferSamples(long value)
{
    if (value < static_cast<long>(kMinBufferSamples))
    {
        return kMinBufferSamples;
    }
    if (value > static_cast<long>(kMaxBufferSamples))
    {
        return kMaxBufferSamples;
    }
    return static_cast<size_t>(value);
}

static uint32_t sanitizePreRecordMs(long value)
{
    if (value < DEFAULT_AUDIO_MIN_PRE_RECORD_MS)
    {
        return DEFAULT_AUDIO_MIN_PRE_RECORD_MS;
    }
    else if (value > DEFAULT_AUDIO_MAX_PRE_RECORD_MS)
    {
       return DEFAULT_AUDIO_MAX_PRE_RECORD_MS;
    }

    // Round to nearest 500ms
    uint32_t remainder = value % 500;
    if (remainder >= 250)
        value += (500 - remainder); // round up
    else
        value -= remainder; // round down

    return static_cast<uint32_t>(value);
}

static uint8_t sanitizeQueueDepth(unsigned long value)
{
    // Queue depth is legacy - filesystem-based queue doesn't use this
    constexpr uint8_t kMinDepth = 4;
    constexpr uint8_t kMaxDepth = 32;
    if (value < kMinDepth)
    {
        return kMinDepth;
    }
    if (value > kMaxDepth)
    {
        return kMaxDepth;
    }
    return static_cast<uint8_t>(value);
}

static void assignUploadHost(size_t idx, const char *value)
{
    if (idx >= kApiEndpointCount)
    {
        return;
    }

    char *dest = appSettings.upload.apiHosts[idx];
    if (dest == nullptr)
    {
        return;
    }

    if (value == nullptr)
    {
        value = "";
    }

    std::strncpy(dest, value, kMaxApiHostLength);
    dest[kMaxApiHostLength] = '\0';
}

static String boolToString(bool value)
{
    return value ? String("true") : String("false");
}

// Helper function to get JSON value with backward compatibility (checks both short and long keys)
template<typename T>
static bool getJsonValue(const JsonObject& obj, const char* shortKey, const char* longKey, T& outValue)
{
    if (obj.containsKey(shortKey))
    {
        outValue = obj[shortKey].as<T>();
        return true;
    }
    else if (obj.containsKey(longKey))
    {
        outValue = obj[longKey].as<T>();
        return true;
    }
    return false;
}

static void logSettingChange(const char *key, const String &oldValue, const String &newValue, bool sensitive = false)
{
    if (oldValue == newValue)
    {
        return;
    }

    const char *name = key ? key : "(unknown)";
    if (sensitive)
    {
    }
    else
    {
    }
    
    // Track changes for CONFIG command
    if (g_trackingConfigChanges)
    {
        SettingChange change;
        change.key = String(name);
        change.oldValue = oldValue;
        change.newValue = newValue;
        change.sensitive = sensitive;
        g_configChanges.push_back(change);
    }
    
    // Notify main system that settings have changed (triggers config message after delay)
    system_notifySettingsChanged();

    // Send per-setting change event to server (unless suppressed)
    sendSettingChangeEvent(key, oldValue, newValue, sensitive);
}

static void assignStringField(const char *&target, const char *value)
{
    char *copy = strdup(value ? value : "");
    if (copy == nullptr)
    {
        return;
    }

    if (target != nullptr)
    {
        free(const_cast<char *>(target));
    }

    target = copy;
}

static void copyToCharBuffer(char *destination, size_t capacity, const String &value)
{
    if (destination == nullptr || capacity == 0)
    {
        return;
    }

    if (value.length() == 0)
    {
        destination[0] = '\0';
        return;
    }

    std::strncpy(destination, value.c_str(), capacity - 1);
    destination[capacity - 1] = '\0';
}

// Calculate CRC32 of a string (simple implementation)
static uint32_t calculateCrc32(const String& data)
{
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.c_str());
    size_t len = data.length();
    
    static const uint32_t crc_table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t byte = bytes[i];
        crc = crc_table[(crc ^ byte) & 0x0F] ^ (crc >> 4);
        crc = crc_table[(crc ^ (byte >> 4)) & 0x0F] ^ (crc >> 4);
    }
    
    return crc ^ 0xFFFFFFFF;
}

// Validate JSON structure (checks for matching braces)
static bool validateJsonStructure(const String& json)
{
    if (json.length() == 0) return false;
    if (!json.startsWith("{") || !json.endsWith("}")) return false;
    
    int braceCount = 0;
    bool inString = false;
    char prevChar = 0;
    
    for (size_t i = 0; i < json.length(); ++i)
    {
        char c = json[i];
        
        // Track string boundaries (ignore braces inside strings)
        if (c == '"' && prevChar != '\\')
        {
            inString = !inString;
        }
        else if (!inString)
        {
            if (c == '{') braceCount++;
            else if (c == '}') braceCount--;
            
            // If we go negative, malformed JSON
            if (braceCount < 0) return false;
        }
        
        prevChar = c;
    }
    
    // Should end with balanced braces
    return braceCount == 0;
}

// Initialize settings mutex
static void ensureSettingsMutex()
{
    if (g_settingsMutex == nullptr)
    {
        g_settingsMutex = xSemaphoreCreateMutex();
        if (g_settingsMutex == nullptr)
        {
        }
    }
}

// Acquire settings mutex (returns true if acquired)
static bool acquireSettingsMutex()
{
    ensureSettingsMutex();
    if (g_settingsMutex == nullptr) return false;
    
    BaseType_t result = xSemaphoreTake(g_settingsMutex, SETTINGS_MUTEX_TIMEOUT_MS);
    if (result != pdTRUE)
    {
        return false;
    }
    return true;
}

// Release settings mutex
static void releaseSettingsMutex()
{
    if (g_settingsMutex != nullptr)
    {
        xSemaphoreGive(g_settingsMutex);
    }
}

// Read JSON from a specific NVS slot
static String readSettingsJsonFromSlot(const char* slotKey)
{
    Preferences prefs;
    if (!nvs_openNamespace(kSettingsNamespace, true, prefs))  // Read-only
    {
        return String();
    }
    
    g_settingsHealth.totalReads++;
    
    // Read the slot data (format: [crc:4][json:string])
    // Use dynamic allocation to avoid stack overflow (max 3500+ bytes)
    size_t bufferSize = kMaxJsonSize + 4;
    uint8_t* buffer = new uint8_t[bufferSize];
    if (buffer == nullptr)
    {
        nvs_closeNamespace(prefs);
        g_settingsHealth.failedReads++;
        g_settingsHealth.lastFailureMs = millis();
        return String();
    }
    
    NvsResult result = nvs_readBytes(prefs, slotKey, buffer, bufferSize);
    nvs_closeNamespace(prefs);
    
    String json;
    
    if (result != NvsResult::SUCCESS || bufferSize < 5)  // Need at least CRC (4) + 1 char
    {
        delete[] buffer;
        g_settingsHealth.failedReads++;
        g_settingsHealth.lastFailureMs = millis();
        return String();
    }
    
    // Extract CRC (first 4 bytes, little-endian)
    uint32_t storedCrc = 0;
    for (int i = 0; i < 4; ++i)
    {
        storedCrc |= (static_cast<uint32_t>(buffer[i]) << (i * 8));
    }
    
    // Extract JSON (rest of buffer)
    json = String(reinterpret_cast<const char*>(buffer + 4), bufferSize - 4);
    
    delete[] buffer;
    
    // Validate CRC
    uint32_t calculatedCrc = calculateCrc32(json);
    if (calculatedCrc != storedCrc)
    {
        g_settingsHealth.corruptionDetected++;
        g_settingsHealth.lastFailureMs = millis();
        g_settingsHealth.failedReads++;
        return String();  // Corrupted
    }
    
    // Validate JSON structure
    if (!validateJsonStructure(json))
    {
        g_settingsHealth.corruptionDetected++;
        g_settingsHealth.lastFailureMs = millis();
        g_settingsHealth.failedReads++;
        return String();
    }
    
    g_settingsHealth.successfulReads++;
    g_settingsHealth.lastReadMs = millis();
    return json;
}

// Check if any settings keys exist in NVS (to distinguish "blank" from "read error")
// Returns: true if NVS is readable and has settings keys, false if blank or error
static bool checkSettingsKeysExist()
{
    Preferences prefs;
    if (!nvs_openNamespace(kSettingsNamespace, true, prefs))  // Read-only
    {
        return false;  // NVS not accessible
    }
    
    // Check if any of the settings keys exist
    bool slot0Exists = false;
    bool slot1Exists = false;
    bool activeSlotExists = false;
    
    nvs_keyExists(prefs, kKeySlot0, slot0Exists);
    nvs_keyExists(prefs, kKeySlot1, slot1Exists);
    nvs_keyExists(prefs, kKeyActiveSlot, activeSlotExists);
    
    nvs_closeNamespace(prefs);
    
    bool hasKeys = slot0Exists || slot1Exists || activeSlotExists;
    
    if (hasKeys)
    {
    }
    else
    {
    }
    
    return hasKeys;
}

// Find which slot has valid data (returns slot key, or nullptr if none)
static const char* findValidSettingsSlot()
{
    // Check active slot indicator first
    Preferences prefs;
    if (nvs_openNamespace(kSettingsNamespace, true, prefs))  // Read-only
    {
        uint8_t activeSlot = 255;
        nvs_readUChar(prefs, kKeyActiveSlot, 255, activeSlot);
        nvs_closeNamespace(prefs);
        
        if (activeSlot == 0)
        {
            String json0 = readSettingsJsonFromSlot(kKeySlot0);
            if (json0.length() > 0)
            {
                return kKeySlot0;
            }
        }
        else if (activeSlot == 1)
        {
            String json1 = readSettingsJsonFromSlot(kKeySlot1);
            if (json1.length() > 0)
            {
                return kKeySlot1;
            }
        }
    }
    
    // Fallback: try both slots
    String json0 = readSettingsJsonFromSlot(kKeySlot0);
    if (json0.length() > 0)
    {
        return kKeySlot0;
    }
    
    String json1 = readSettingsJsonFromSlot(kKeySlot1);
    if (json1.length() > 0)
    {
        return kKeySlot1;
    }
    
    return nullptr;  // No valid slot found
}

// Write JSON to a specific NVS slot
static bool writeSettingsJsonToSlot(const String& json, const char* slotKey)
{
    if (json.length() > kMaxJsonSize)
    {
        setSettingsError("JSON too large for NVS");
        return false;
    }
    
    if (!validateJsonStructure(json))
    {
        setSettingsError("Invalid JSON format");
        return false;
    }
    
    // Calculate CRC
    uint32_t crc = calculateCrc32(json);
    
    Preferences prefs;
    if (!nvs_openNamespace(kSettingsNamespace, false, prefs))  // Read-write
    {
        setSettingsError("Failed to open NVS namespace");
        g_settingsHealth.failedWrites++;
        g_settingsHealth.lastFailureMs = millis();
        return false;
    }
    
    g_settingsHealth.totalWrites++;
    
    // Prepare buffer: [CRC:4 bytes][JSON:string]
    size_t bufferSize = 4 + json.length();
    uint8_t* buffer = new uint8_t[bufferSize];
    if (buffer == nullptr)
    {
        setSettingsError("Out of memory");
        nvs_closeNamespace(prefs);
        g_settingsHealth.failedWrites++;
        g_settingsHealth.lastFailureMs = millis();
        return false;
    }
    
    // Write CRC (4 bytes, little-endian)
    for (int i = 0; i < 4; ++i)
    {
        buffer[i] = (crc >> (i * 8)) & 0xFF;
    }
    
    // Write JSON
    std::memcpy(buffer + 4, json.c_str(), json.length());
    
    // Write to NVS (NVS writes are atomic, no commit needed)
    NvsResult result = nvs_writeBytes(prefs, slotKey, buffer, bufferSize);
    
    delete[] buffer;
    nvs_closeNamespace(prefs);
    
    if (result != NvsResult::SUCCESS)
    {
        setSettingsError("NVS write failed");
        g_settingsHealth.failedWrites++;
        g_settingsHealth.lastFailureMs = millis();
        return false;
    }
    
    // Skip verification read to avoid stack overflow (NVS writes are reliable)
    // If verification is needed, it can be done on next read
    
    g_settingsHealth.successfulWrites++;
    g_settingsHealth.lastWriteMs = millis();
    return true;
}

static void applyWifiSettings(size_t idx, const JsonObject &w)
{
    if (idx >= kMaxWifiCredentials)
    {
        return;
    }

    WiFiSettings &wifi = appSettings.wifi[idx];

    if (w.containsKey(JsonKeys::SSID) && w[JsonKeys::SSID].is<const char *>())
    {
        String newValue = String(w[JsonKeys::SSID].as<const char *>());
        String oldValue = wifi.ssid ? String(wifi.ssid) : String();
        String key = String("wifi[") + idx + "].ssid";
        logSettingChange(key.c_str(), oldValue, newValue);
        assignStringField(wifi.ssid, newValue.c_str());
    }

    if (w.containsKey(JsonKeys::PASSWORD) && w[JsonKeys::PASSWORD].is<const char *>())
    {
        String newValue = String(w[JsonKeys::PASSWORD].as<const char *>());
        String oldValue = wifi.password ? String(wifi.password) : String();
        String key = String("wifi[") + idx + "].password";
        logSettingChange(key.c_str(), oldValue, newValue, true);
        assignStringField(wifi.password, newValue.c_str());
    }

    if (w.containsKey(JsonKeys::CONNECT_TIMEOUT_MS))
    {
        unsigned long newValue = w[JsonKeys::CONNECT_TIMEOUT_MS].as<unsigned long>();
        unsigned long oldValue = wifi.connectTimeoutMs;
        if (oldValue != newValue)
        {
            String key = String("wifi[") + idx + "].connectTimeoutMs";
            logSettingChange(key.c_str(), String(oldValue), String(newValue));
            wifi.connectTimeoutMs = newValue;
        }
    }

    if (w.containsKey(JsonKeys::STATIC_IP_ENABLED))
    {
        bool newValue = w[JsonKeys::STATIC_IP_ENABLED].as<bool>();
        if (wifi.staticIpEnabled != newValue)
        {
            String key = String("wifi[") + idx + "].staticIpEnabled";
            logSettingChange(key.c_str(), boolToString(wifi.staticIpEnabled), boolToString(newValue));
            wifi.staticIpEnabled = newValue;
        }
    }

    auto assignCharArray = [](char *destination, size_t capacity, const char *source)
    {
        if (capacity == 0)
        {
            return;
        }
        if (source == nullptr)
        {
            destination[0] = '\0';
            return;
        }
        std::strncpy(destination, source, capacity - 1);
        destination[capacity - 1] = '\0';
    };

    const struct
    {
        const char *jsonKey;
        char *field;
        size_t size;
        const char *name;
    } charFields[] = {
        {JsonKeys::STATIC_IP, wifi.staticIp, sizeof(wifi.staticIp), "staticIp"},
        {JsonKeys::STATIC_SUBNET, wifi.staticSubnet, sizeof(wifi.staticSubnet), "staticSubnet"},
        {JsonKeys::STATIC_GATEWAY, wifi.staticGateway, sizeof(wifi.staticGateway), "staticGateway"},
        {JsonKeys::STATIC_DNS1, wifi.staticDns1, sizeof(wifi.staticDns1), "staticDns1"},
        {JsonKeys::STATIC_DNS2, wifi.staticDns2, sizeof(wifi.staticDns2), "staticDns2"},
    };

    for (const auto &entry : charFields)
    {
        if (w.containsKey(entry.jsonKey) && w[entry.jsonKey].is<const char *>())
        {
            const char *raw = w[entry.jsonKey].as<const char *>();
            String newValue = String(raw);
            String oldValue = String(entry.field);
            String key = String("wifi[") + idx + "]." + entry.name;
            logSettingChange(key.c_str(), oldValue, newValue);
            assignCharArray(entry.field, entry.size, raw);
        }
    }
}

// Read settings JSON from NVS
// Returns: JSON string if successful, empty string if failed or blank
// Note: Empty string can mean either "NVS is blank" or "read failed"
// Use checkSettingsKeysExist() to distinguish between these cases
static String readSettingsJson()
{
    if (!acquireSettingsMutex())
    {
        return String();
    }
    
    String json;
    
    // Find valid slot
    const char* validSlot = findValidSettingsSlot();
    if (validSlot != nullptr)
    {
        json = readSettingsJsonFromSlot(validSlot);
    }
    
    releaseSettingsMutex();
    
    if (json.length() > 0)
    {
        // Backup as last known good
        g_lastKnownGoodJson = json;
        g_hasLastKnownGood = true;
    }
    else
    {
        // Try to restore from backup if available (only on read failure, not blank NVS)
        // Note: We check keys exist before attempting restore to avoid restoring on blank NVS
        if (g_hasLastKnownGood && g_lastKnownGoodJson.length() > 0)
        {
            // Check if keys exist without logging (to avoid recursion issues)
            Preferences prefsCheck;
            bool keysExist = false;
            if (nvs_openNamespace(kSettingsNamespace, true, prefsCheck))
            {
                bool slot0Exists = false;
                bool slot1Exists = false;
                bool activeSlotExists = false;
                nvs_keyExists(prefsCheck, kKeySlot0, slot0Exists);
                nvs_keyExists(prefsCheck, kKeySlot1, slot1Exists);
                nvs_keyExists(prefsCheck, kKeyActiveSlot, activeSlotExists);
                keysExist = slot0Exists || slot1Exists || activeSlotExists;
                nvs_closeNamespace(prefsCheck);
            }
            
            // Only restore if keys exist (meaning it's a read failure, not blank NVS)
            if (keysExist)
            {
                json = g_lastKnownGoodJson;
            }
        }
    }
    
    return json;
}

static bool writeSettingsJson(const String& json)
{
    if (!acquireSettingsMutex())
    {
        setSettingsError("Failed to acquire settings mutex");
        return false;
    }
    
    bool success = false;
    
    // Find current valid slot (write to the other one for atomic operation)
    const char* currentSlot = findValidSettingsSlot();
    const char* targetSlot;
    
    if (currentSlot == kKeySlot0)
    {
        targetSlot = kKeySlot1;  // Write to slot 1
    }
    else
    {
        targetSlot = kKeySlot0;  // Write to slot 0 (or if no valid slot, use 0)
    }
    
    // Write to target slot
    success = writeSettingsJsonToSlot(json, targetSlot);
    
    if (success)
    {
        // Update active slot indicator
        Preferences prefs;
        if (nvs_openNamespace(kSettingsNamespace, false, prefs))
        {
            uint8_t activeSlotValue = (targetSlot == kKeySlot0) ? 0 : 1;
            NvsResult result = nvs_writeUChar(prefs, kKeyActiveSlot, activeSlotValue);
            
            if (result != NvsResult::SUCCESS)
            {
            }
            
            // Remove old slot if it exists (cleanup)
            if (currentSlot != nullptr)
            {
                nvs_removeKey(prefs, currentSlot);
            }
            
            nvs_closeNamespace(prefs);
        }
        else
        {
        }
        
        // Update backup
        g_lastKnownGoodJson = json;
        g_hasLastKnownGood = true;
    }
    else
    {
        String errorMsg = settings_getLastError();
        if (errorMsg.length() > 0)
        {
        }
    }
    
    releaseSettingsMutex();
    
    return success;
}

// Helper function to merge JSON objects: preserves existing values, adds missing defaults
static void mergeJsonObjects(JsonObject target, const JsonObject& source) {
    for (JsonPair pair : source) {
        const char* key = pair.key().c_str();
        JsonVariant value = pair.value();
        
        // Skip version fields - they will be updated separately
        if (strcmp(key, "firmware") == 0 || strcmp(key, "configVersion") == 0) {
            continue;
        }
        
        if (!target.containsKey(key)) {
            // Key doesn't exist in target, add it from source (default value)
            if (value.is<JsonObject>()) {
                JsonObject nestedTarget = target.createNestedObject(key);
                JsonObject sourceObj = value.as<JsonObject>();
                // Copy all properties from source to nested target
                for (JsonPair nestedPair : sourceObj) {
                    nestedTarget[nestedPair.key().c_str()] = nestedPair.value();
                }
            } else if (value.is<JsonArray>()) {
                // For arrays, copy the entire array from defaults
                JsonArray sourceArray = value.as<JsonArray>();
                JsonArray targetArray = target.createNestedArray(key);
                for (JsonVariant item : sourceArray) {
                    targetArray.add(item);
                }
            } else {
                // Primitive value - copy as-is
                target[key] = value;
            }
        } else {
            // Key exists - recursively merge if both are objects
            JsonVariant targetValue = target[key];
            if (value.is<JsonObject>() && targetValue.is<JsonObject>()) {
                mergeJsonObjects(targetValue.as<JsonObject>(), value.as<JsonObject>());
            }
            // For arrays and primitives, keep existing value (don't overwrite)
        }
    }
}

static void appSettingsToJson(DynamicJsonDocument &doc)
{
    // Add firmware version and config version
    doc[JsonKeys::FIRMWARE_KEY] = FIRMWARE;
    doc[JsonKeys::CONFIG_VERSION_KEY] = CONFIG_VERSION;
    
    // wifi - expose as an array of credential objects (backwards compatible: older code
    // may have a single object; we always write an array)
    JsonArray wifiArr = doc.createNestedArray(JsonKeys::WIFI);
    for (size_t i = 0; i < kMaxWifiCredentials; ++i)
    {
        JsonObject w = wifiArr.createNestedObject();
        const char *ssid = appSettings.wifi[i].ssid ? appSettings.wifi[i].ssid : "";
        const char *pass = appSettings.wifi[i].password ? appSettings.wifi[i].password : "";
        w[JsonKeys::SSID] = ssid;
        w[JsonKeys::PASSWORD] = pass;
        w[JsonKeys::CONNECT_TIMEOUT_MS] = appSettings.wifi[i].connectTimeoutMs;
        w[JsonKeys::STATIC_IP_ENABLED] = appSettings.wifi[i].staticIpEnabled;
        w[JsonKeys::STATIC_IP] = appSettings.wifi[i].staticIp;
        w[JsonKeys::STATIC_SUBNET] = appSettings.wifi[i].staticSubnet;
        w[JsonKeys::STATIC_GATEWAY] = appSettings.wifi[i].staticGateway;
        w[JsonKeys::STATIC_DNS1] = appSettings.wifi[i].staticDns1;
        w[JsonKeys::STATIC_DNS2] = appSettings.wifi[i].staticDns2;
    }

    // audio
    JsonObject audio = doc.createNestedObject(JsonKeys::AUDIO);
    audio[JsonKeys::SAMPLE_RATE] = appSettings.audio.sampleRate;
    audio[JsonKeys::BUFFER_SAMPLES] = appSettings.audio.bufferSamples;
    audio[JsonKeys::AUDIO_THRESHOLD] = appSettings.audio.audioThreshold;
    audio[JsonKeys::PRE_RECORD_MS] = appSettings.audio.preRecordMs;
    audio[JsonKeys::MIN_RECORDING_MS] = appSettings.audio.minRecordingMs;
    audio[JsonKeys::MAX_RECORDING_MS] = appSettings.audio.maxRecordingMs;
    audio[JsonKeys::SILENCE_THRESHOLD_MS] = appSettings.audio.silenceThresholdMs;
    audio[JsonKeys::DISCARD_SMALL_FILES_ENABLED] = appSettings.audio.discardSmallFilesEnabled;
    audio[JsonKeys::DISCARD_SMALL_FILES_MIN_MS] = appSettings.audio.discardSmallFilesMinMs;
    audio[JsonKeys::CODEC_GAIN] = appSettings.audio.codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    audio[JsonKeys::RECORD_INPUT_CHANNEL] = appSettings.audio.recordInputChannel;
#endif
    audio[JsonKeys::SPEAKER_ENABLED] = appSettings.speakerEnabled;
    audio[JsonKeys::SPEAKER_VOLUME] = static_cast<unsigned>(appSettings.speakerVolume);
    audio[JsonKeys::TRANSMIT_ENABLED] = appSettings.transmitEnabled;
    audio[JsonKeys::TRANSMIT_VOLUME] = static_cast<unsigned>(appSettings.transmitVolume);
#if defined(ECHO)
    audio[JsonKeys::REPEATER_ENABLED] = appSettings.repeaterEnabled;
    audio[JsonKeys::REPEATER_MODE] = static_cast<unsigned>(appSettings.repeaterMode);
#endif
    audio[JsonKeys::CW_WPM] = static_cast<unsigned>(appSettings.cwWpm);
    audio[JsonKeys::CW_TONE_HZ] = static_cast<unsigned>(appSettings.cwToneHz);
    audio[JsonKeys::CW_VOLUME] = static_cast<unsigned>(appSettings.cwVolume);
    audio[JsonKeys::CW_REPEAT] = static_cast<unsigned>(appSettings.cwRepeat);

    // upload
    JsonObject upload = doc.createNestedObject(JsonKeys::UPLOAD);
    upload[JsonKeys::QUEUE_DEPTH] = appSettings.upload.queueDepth;
    upload[JsonKeys::CONVERT_TO_MP3] = appSettings.upload.convertToMp3;  // Legacy
    JsonArray apiHosts = upload.createNestedArray(JsonKeys::API_HOSTS);
    JsonArray apiPorts = upload.createNestedArray(JsonKeys::API_PORTS);
    JsonArray enabled = upload.createNestedArray(JsonKeys::ENABLED);
    for (size_t i = 0; i < kApiEndpointCount; ++i)
    {
        apiHosts.add(String(appSettings.upload.apiHosts[i]));
        apiPorts.add(appSettings.upload.apiPorts[i]);
        enabled.add(appSettings.upload.enabled[i]);
    }

    // rtc
    JsonObject rtc = doc.createNestedObject(JsonKeys::RTC);
    rtc[JsonKeys::ENABLED] = appSettings.rtc.enabled;
    rtc[JsonKeys::SDA_PIN] = appSettings.rtc.sdaPin;
    rtc[JsonKeys::SCL_PIN] = appSettings.rtc.sclPin;

    // sdCard
    JsonObject sdCard = doc.createNestedObject(JsonKeys::SD_CARD);
    sdCard[JsonKeys::USE_SD_CARD] = appSettings.sdCard.useSdCard;
    sdCard[JsonKeys::RECORD_TO_SD_CARD] = appSettings.sdCard.recordToSdCard;
    sdCard[JsonKeys::MODE_1BIT] = appSettings.sdCard.mode1bit;
    sdCard[JsonKeys::FREQUENCY] = appSettings.sdCard.frequency;
    sdCard[JsonKeys::FORMAT_IF_MOUNT_FAILED] = appSettings.sdCard.formatIfMountFailed;

    // timezone
    JsonObject timezone = doc.createNestedObject(JsonKeys::TIMEZONE);
    timezone[JsonKeys::OFFSET_HOURS] = appSettings.timezone.offsetHours;
    timezone[JsonKeys::MAINTENANCE_HOUR] = appSettings.timezone.maintenanceHour;
    timezone[JsonKeys::MAINTENANCE_MINUTE] = appSettings.timezone.maintenanceMinute;

    // log
    JsonObject log = doc.createNestedObject(JsonKeys::LOG);
    log[JsonKeys::SERIAL_FATAL] = appSettings.log.serialFatal;
    log[JsonKeys::SERIAL_ERROR] = appSettings.log.serialError;
    log[JsonKeys::SERIAL_WARNING] = appSettings.log.serialWarning;
    log[JsonKeys::SERIAL_INFO] = appSettings.log.serialInfo;
    log[JsonKeys::SERIAL_DEBUG] = appSettings.log.serialDebug;
    log[JsonKeys::SERIAL_EVENT] = appSettings.log.serialEvent;
    log[JsonKeys::FILE_FATAL] = appSettings.log.fileFatal;
    log[JsonKeys::FILE_ERROR] = appSettings.log.fileError;
    log[JsonKeys::FILE_WARNING] = appSettings.log.fileWarning;
    log[JsonKeys::FILE_INFO] = appSettings.log.fileInfo;
    log[JsonKeys::FILE_DEBUG] = appSettings.log.fileDebug;
    log[JsonKeys::FILE_EVENT] = appSettings.log.fileEvent;
    
    
    doc[JsonKeys::HOSTNAME] = appSettings.hostname;
    doc[JsonKeys::MQTT_KEY] = appSettings.mqttKey;

    // wifiTxPower
    doc[JsonKeys::WIFI_TX_POWER] = appSettings.wifiTxPower;
    
    // webserverEnabled
    doc[JsonKeys::WEBSERVER_ENABLED] = appSettings.webserverEnabled;

    // Legacy LED compatibility settings
    doc[JsonKeys::LED_STYLE] = static_cast<unsigned>(appSettings.ledStyle);
    doc[JsonKeys::STARTUP_MODE] = static_cast<unsigned>(appSettings.startupMode);
    doc[JsonKeys::OFFLINE_MODE] = appSettings.offlineMode;

    // Runtime metadata exposed for web UI consumption
    JsonObject runtime = doc.createNestedObject(JsonKeys::RUNTIME);

    const String &deviceId = getDeviceId();
    runtime[JsonKeys::MAC_ADDRESS_RT] = deviceId;
    doc[JsonKeys::MAC_ADDRESS] = deviceId;

    const bool wifiConnected = WiFi.isConnected();
    runtime[JsonKeys::WIFI_CONNECTED] = wifiConnected;

    if (wifiConnected)
    {
        const String connectedSsid = WiFi.SSID();
        if (connectedSsid.length() > 0)
        {
            runtime[JsonKeys::CONNECTED_SSID] = connectedSsid;
            doc[JsonKeys::CONNECTED_WIFI_SSID] = connectedSsid;
        }

        runtime[JsonKeys::RSSI] = WiFi.RSSI();

        int connectedIndex = -1;
        String normalizedConnected = connectedSsid;
        normalizedConnected.trim();
        String lowerConnected = normalizedConnected;
        lowerConnected.toLowerCase();

        for (size_t i = 0; i < kMaxWifiCredentials; ++i)
        {
            const char *candidate = appSettings.wifi[i].ssid;
            if (candidate == nullptr || std::strlen(candidate) == 0)
            {
                continue;
            }

            String entrySsid = String(candidate);
            entrySsid.trim();
            String lowerEntry = entrySsid;
            lowerEntry.toLowerCase();

            if (lowerEntry == lowerConnected)
            {
                connectedIndex = static_cast<int>(i);
                break;
            }
        }

        runtime[JsonKeys::CONNECTED_INDEX] = connectedIndex;
        doc[JsonKeys::CONNECTED_WIFI_INDEX] = connectedIndex;

        JsonObject currentIp = doc.createNestedObject(JsonKeys::CURRENT_IP);

        const String localIpStr = WiFi.localIP().toString();
        if (localIpStr.length() > 0)
        {
            currentIp[JsonKeys::IP] = localIpStr;
        }

        const String subnetStr = WiFi.subnetMask().toString();
        if (subnetStr.length() > 0)
        {
            currentIp[JsonKeys::SUBNET] = subnetStr;
        }

        const String gatewayStr = WiFi.gatewayIP().toString();
        if (gatewayStr.length() > 0)
        {
            currentIp[JsonKeys::GATEWAY] = gatewayStr;
        }

        const String dns1Str = WiFi.dnsIP(0).toString();
        if (dns1Str.length() > 0)
        {
            currentIp[JsonKeys::DNS1] = dns1Str;
        }

        const String dns2Str = WiFi.dnsIP(1).toString();
        if (dns2Str.length() > 0)
        {
            currentIp[JsonKeys::DNS2] = dns2Str;
        }

        if (currentIp.size() == 0)
        {
            doc.remove(JsonKeys::CURRENT_IP);
        }
    }
}

static bool jsonToAppSettings(const JsonObject &root)
{
    // Validate and copy values where present; keep defaults where missing
    // Support both short keys and legacy long keys for backward compatibility
    bool hasWifi = root.containsKey(JsonKeys::WIFI) || root.containsKey("wifi");
    if (hasWifi)
    {
        JsonVariant wifiVariant = root.containsKey(JsonKeys::WIFI) ? root[JsonKeys::WIFI] : root["wifi"];
        // Support both array and single-object legacy formats
        if (wifiVariant.is<JsonArray>())
        {
            JsonArray wa = wifiVariant.as<JsonArray>();
            size_t idx = 0;
            for (JsonVariant v : wa)
            {
                if (idx >= kMaxWifiCredentials)
                    break;
                if (v.is<JsonObject>())
                {
                    applyWifiSettings(idx, v.as<JsonObject>());
                }
                ++idx;
            }
        }
        else if (wifiVariant.is<JsonObject>())
        {
            // Legacy single object - apply to index 0
            applyWifiSettings(0, wifiVariant.as<JsonObject>());
        }
    }

    bool hasSdCard = (root.containsKey(JsonKeys::SD_CARD) || root.containsKey("sdCard"));
    if (hasSdCard && (root.containsKey(JsonKeys::SD_CARD) ? root[JsonKeys::SD_CARD].is<JsonObject>() : root["sdCard"].is<JsonObject>()))
    {
        JsonObject sdCard = root.containsKey(JsonKeys::SD_CARD) ? root[JsonKeys::SD_CARD].as<JsonObject>() : root["sdCard"].as<JsonObject>();
        if (sdCard.containsKey(JsonKeys::USE_SD_CARD) || sdCard.containsKey("useSdCard"))
        {
            bool newValue = sdCard.containsKey(JsonKeys::USE_SD_CARD) ? sdCard[JsonKeys::USE_SD_CARD].as<bool>() : sdCard["useSdCard"].as<bool>();
            if (appSettings.sdCard.useSdCard != newValue)
            {
                logSettingChange("sdCard.useSdCard",
                                 boolToString(appSettings.sdCard.useSdCard),
                                 boolToString(newValue));
                appSettings.sdCard.useSdCard = newValue;
                // Re-evaluate storage mode when useSdCard changes
                storage_revaluateMode();
            }
        }
        if (sdCard.containsKey(JsonKeys::RECORD_TO_SD_CARD) || sdCard.containsKey("recordToSdCard"))
        {
            bool newValue = sdCard.containsKey(JsonKeys::RECORD_TO_SD_CARD) ? sdCard[JsonKeys::RECORD_TO_SD_CARD].as<bool>() : sdCard["recordToSdCard"].as<bool>();
            if (appSettings.sdCard.recordToSdCard != newValue)
            {
                logSettingChange("sdCard.recordToSdCard",
                                 boolToString(appSettings.sdCard.recordToSdCard),
                                 boolToString(newValue));
                appSettings.sdCard.recordToSdCard = newValue;
                // Re-evaluate storage mode when recordToSdCard changes
                storage_revaluateMode();
            }
        }
        if (sdCard.containsKey(JsonKeys::MODE_1BIT) || sdCard.containsKey("mode1bit"))
        {
            bool newValue = sdCard.containsKey(JsonKeys::MODE_1BIT) ? sdCard[JsonKeys::MODE_1BIT].as<bool>() : sdCard["mode1bit"].as<bool>();
            if (appSettings.sdCard.mode1bit != newValue)
            {
                logSettingChange("sdCard.mode1bit",
                                 boolToString(appSettings.sdCard.mode1bit),
                                 boolToString(newValue));
                appSettings.sdCard.mode1bit = newValue;
            }
        }
        if (sdCard.containsKey(JsonKeys::FREQUENCY) || sdCard.containsKey("frequency"))
        {
            uint32_t newValue = sdCard.containsKey(JsonKeys::FREQUENCY) ? sdCard[JsonKeys::FREQUENCY].as<uint32_t>() : sdCard["frequency"].as<uint32_t>();
            // Validate frequency range (1MHz to 20MHz)
            if (newValue >= 1000000 && newValue <= 20000000)
            {
                if (appSettings.sdCard.frequency != newValue)
                {
                    logSettingChange("sdCard.frequency",
                                     String(appSettings.sdCard.frequency),
                                     String(newValue));
                    appSettings.sdCard.frequency = newValue;
                }
            }
        }
        if (sdCard.containsKey(JsonKeys::FORMAT_IF_MOUNT_FAILED) || sdCard.containsKey("formatIfMountFailed"))
        {
            bool newValue = sdCard.containsKey(JsonKeys::FORMAT_IF_MOUNT_FAILED) ? sdCard[JsonKeys::FORMAT_IF_MOUNT_FAILED].as<bool>() : sdCard["formatIfMountFailed"].as<bool>();
            if (appSettings.sdCard.formatIfMountFailed != newValue)
            {
                logSettingChange("sdCard.formatIfMountFailed",
                                 boolToString(appSettings.sdCard.formatIfMountFailed),
                                 boolToString(newValue));
                appSettings.sdCard.formatIfMountFailed = newValue;
            }
        }
    }

    bool hasAudio = root.containsKey(JsonKeys::AUDIO) || root.containsKey("audio");
    if (hasAudio)
    {
        JsonObject audio = root.containsKey(JsonKeys::AUDIO) ? root[JsonKeys::AUDIO].as<JsonObject>() : root["audio"].as<JsonObject>();
        if (audio.containsKey(JsonKeys::SAMPLE_RATE) || audio.containsKey("sampleRate"))
        {
            uint32_t newValue = sanitizeSampleRate(audio.containsKey(JsonKeys::SAMPLE_RATE) ? audio[JsonKeys::SAMPLE_RATE].as<uint32_t>() : audio["sampleRate"].as<uint32_t>());
            if (appSettings.audio.sampleRate != newValue)
            {
                logSettingChange("audio.sampleRate", String(appSettings.audio.sampleRate), String(newValue));
                appSettings.audio.sampleRate = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::BUFFER_SAMPLES) || audio.containsKey("bufferSamples"))
        {
            size_t newValue = sanitizeBufferSamples(audio.containsKey(JsonKeys::BUFFER_SAMPLES) ? audio[JsonKeys::BUFFER_SAMPLES].as<long>() : audio["bufferSamples"].as<long>());
            if (appSettings.audio.bufferSamples != newValue)
            {
                logSettingChange("audio.bufferSamples",
                                 String(static_cast<unsigned long>(appSettings.audio.bufferSamples)),
                                 String(static_cast<unsigned long>(newValue)));
                appSettings.audio.bufferSamples = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::AUDIO_THRESHOLD) || audio.containsKey("audioThreshold"))
        {
            int rawValue = audio.containsKey(JsonKeys::AUDIO_THRESHOLD) ? audio[JsonKeys::AUDIO_THRESHOLD].as<int>() : audio["audioThreshold"].as<int>();
            if (rawValue < 0 || rawValue > 60) {
                setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.audioThreshold must be 0-60, reset to default (30)");
                rawValue = 30;
            }
            uint8_t newValue = static_cast<uint8_t>(rawValue);

            if (appSettings.audio.audioThreshold != newValue)
            {
                logSettingChange("audio.audioThreshold",
                                 String(static_cast<unsigned>(appSettings.audio.audioThreshold)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.audio.audioThreshold = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::PRE_RECORD_MS) || audio.containsKey("preRecordMs"))
        {
            uint32_t newValue = sanitizePreRecordMs(audio.containsKey(JsonKeys::PRE_RECORD_MS) ? audio[JsonKeys::PRE_RECORD_MS].as<long>() : audio["preRecordMs"].as<long>());
            if (appSettings.audio.preRecordMs != newValue)
            {
                logSettingChange("audio.preRecordMs",
                                 String(appSettings.audio.preRecordMs),
                                 String(newValue));
                appSettings.audio.preRecordMs = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::MIN_RECORDING_MS) || audio.containsKey("minRecordingMs"))
        {
            uint32_t newValue = audio.containsKey(JsonKeys::MIN_RECORDING_MS) ? audio[JsonKeys::MIN_RECORDING_MS].as<uint32_t>() : audio["minRecordingMs"].as<uint32_t>();
            if (newValue < DEFAULT_AUDIO_MIN_RECORDING_MS)
            {
                LOG_DEBUG("Minimum recording time can not be less than 1 second. It is been set to 1 second.");
                newValue = DEFAULT_AUDIO_MIN_RECORDING_MS;
            }
            if (newValue > appSettings.audio.maxRecordingMs)
            {
                LOG_DEBUG("The minimum can not be higher than the maximum recording time. We have set it to match the maximum.");
                newValue = std::min(newValue, static_cast<uint32_t>(appSettings.audio.maxRecordingMs));
            }

            if (appSettings.audio.minRecordingMs != newValue)
            {
                logSettingChange("audio.minRecordingMs",
                                 String(appSettings.audio.minRecordingMs),
                                 String(newValue));
                appSettings.audio.minRecordingMs = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::MAX_RECORDING_MS) || audio.containsKey("maxRecordingMs"))
        {
            uint32_t newValue = audio.containsKey(JsonKeys::MAX_RECORDING_MS) ? audio[JsonKeys::MAX_RECORDING_MS].as<uint32_t>() : audio["maxRecordingMs"].as<uint32_t>();
            
            if (newValue < DEFAULT_AUDIO_MIN_RECORDING_MS)
            {
                LOG_DEBUG("Maximum recording time can not be lower than the minimum allowed. We have set it to the minimum.");
                newValue = (DEFAULT_AUDIO_MIN_RECORDING_MS);
            }

            if (appSettings.sdCard.useSdCard && appSettings.sdCard.recordToSdCard)
            {
                if (newValue > DEFAULT_AUDIO_MAX_SD_RECORDING_MS)
                {
                    LOG_DEBUG("Maximum recording time cannot be longer than allowed. It has been set to the longest time allowed.");
                    newValue = std::min(newValue, static_cast<uint32_t>(DEFAULT_AUDIO_MAX_SD_RECORDING_MS));
                }
            }
            else
            {
                if (newValue > DEFAULT_AUDIO_MAX_RECORDING_MS)
                {
                    LOG_DEBUG("Maximum recording time cannot be longer than allowed. It has been set to the longest time allowed.");
                    newValue = std::min(newValue, static_cast<uint32_t>(DEFAULT_AUDIO_MAX_RECORDING_MS));
                }
            }


            if (newValue < appSettings.audio.minRecordingMs)
            {
                LOG_DEBUG("Maximum recording time cannot be shorter than minimum recording time. It has been set to match the minimum.");
                newValue = std::max(newValue, static_cast<uint32_t>(appSettings.audio.minRecordingMs));
            }

            if (appSettings.audio.maxRecordingMs != newValue)
            {
                logSettingChange("audio.maxRecordingMs",
                                 String(appSettings.audio.maxRecordingMs),
                                 String(newValue));
                appSettings.audio.maxRecordingMs = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::SILENCE_THRESHOLD_MS) || audio.containsKey("silenceThresholdMs"))
        {
            uint32_t newValue = audio.containsKey(JsonKeys::SILENCE_THRESHOLD_MS) ? audio[JsonKeys::SILENCE_THRESHOLD_MS].as<uint32_t>() : audio["silenceThresholdMs"].as<uint32_t>();
            // Clamp and round to nearest 100ms for sanity
            if (newValue > DEFAULT_AUDIO_MAX_SILENCE_THRESHOLD_MS || newValue < DEFAULT_AUDIO_MIN_SILENCE_THRESHOLD_MS) {
                setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.silenceThresholdMs out of range");
                return false;
            }

            newValue = sanitizeSilenceThresholdMs(newValue);
            if (appSettings.audio.silenceThresholdMs != newValue)
            {
                logSettingChange("audio.silenceThresholdMs",
                                 String(appSettings.audio.silenceThresholdMs),
                                 String(newValue));
                appSettings.audio.silenceThresholdMs = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::DISCARD_SMALL_FILES_ENABLED) || audio.containsKey("discardSmallFilesEnabled"))
        {
            bool newValue = audio.containsKey(JsonKeys::DISCARD_SMALL_FILES_ENABLED) ? audio[JsonKeys::DISCARD_SMALL_FILES_ENABLED].as<bool>() : audio["discardSmallFilesEnabled"].as<bool>();
            if (appSettings.audio.discardSmallFilesEnabled != newValue)
            {
                logSettingChange("audio.discardSmallFilesEnabled",
                                 boolToString(appSettings.audio.discardSmallFilesEnabled),
                                 boolToString(newValue));
                appSettings.audio.discardSmallFilesEnabled = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::DISCARD_SMALL_FILES_MIN_MS) || audio.containsKey("discardSmallFilesMinMs"))
        {
            uint32_t newValue = audio.containsKey(JsonKeys::DISCARD_SMALL_FILES_MIN_MS) ? audio[JsonKeys::DISCARD_SMALL_FILES_MIN_MS].as<uint32_t>() : audio["discardSmallFilesMinMs"].as<uint32_t>();
            if (appSettings.audio.discardSmallFilesMinMs != newValue)
            {
                logSettingChange("audio.discardSmallFilesMinMs",
                                 String(appSettings.audio.discardSmallFilesMinMs),
                                 String(newValue));
                appSettings.audio.discardSmallFilesMinMs = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::CODEC_GAIN) || audio.containsKey("codecGain"))
        {
            int gain = audio.containsKey(JsonKeys::CODEC_GAIN) ? audio[JsonKeys::CODEC_GAIN].as<int>() : audio["codecGain"].as<int>();
            int8_t newValue = sanitizeCodecGainDb(gain);
            if (appSettings.audio.codecGainDb != newValue)
            {
                logSettingChange("audio.codecGain", String(appSettings.audio.codecGainDb), String(newValue));
                appSettings.audio.codecGainDb = newValue;
            }
        }
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
        if (audio.containsKey(JsonKeys::RECORD_INPUT_CHANNEL) || audio.containsKey("recordInputChannel"))
        {
            int raw = audio.containsKey(JsonKeys::RECORD_INPUT_CHANNEL) ? audio[JsonKeys::RECORD_INPUT_CHANNEL].as<int>() : audio["recordInputChannel"].as<int>();
            if (raw < 0)
            {
                raw = 0;
            }
            if (raw > 1)
            {
                raw = 1;
            }
            uint8_t newValue = static_cast<uint8_t>(raw);
            if (appSettings.audio.recordInputChannel != newValue)
            {
                logSettingChange("audio.recordInputChannel",
                                 String(static_cast<unsigned>(appSettings.audio.recordInputChannel)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.audio.recordInputChannel = newValue;
            }
        }
#endif

        if (audio.containsKey(JsonKeys::SPEAKER_ENABLED) || audio.containsKey("speakerEnabled"))
        {
            bool newValue = audio.containsKey(JsonKeys::SPEAKER_ENABLED) ? audio[JsonKeys::SPEAKER_ENABLED].as<bool>() : audio["speakerEnabled"].as<bool>();
            if (appSettings.speakerEnabled != newValue)
            {
                logSettingChange("audio.speakerEnabled", boolToString(appSettings.speakerEnabled), boolToString(newValue));
                appSettings.speakerEnabled = newValue;
            }
        }

        if (audio.containsKey(JsonKeys::SPEAKER_VOLUME) || audio.containsKey("speakerVolume"))
        {
            int raw = audio.containsKey(JsonKeys::SPEAKER_VOLUME) ? audio[JsonKeys::SPEAKER_VOLUME].as<int>() : audio["speakerVolume"].as<int>();
            if (raw < 0 || raw > 100) {
                setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.speakerVolume must be 0-100");
                return false;
            }
            uint8_t newValue = sanitizeSpeakerVolume(raw);
            if (appSettings.speakerVolume != newValue)
            {
                logSettingChange("audio.speakerVolume",
                                 String(static_cast<unsigned>(appSettings.speakerVolume)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.speakerVolume = newValue;
            }
        }

        // TX placeholder settings (stored for future feature)
        if (audio.containsKey(JsonKeys::TRANSMIT_ENABLED) || audio.containsKey("transmitEnabled"))
        {
            bool newValue = audio.containsKey(JsonKeys::TRANSMIT_ENABLED) ? audio[JsonKeys::TRANSMIT_ENABLED].as<bool>() : audio["transmitEnabled"].as<bool>();
            if (appSettings.transmitEnabled != newValue)
            {
                logSettingChange("audio.transmitEnabled", boolToString(appSettings.transmitEnabled), boolToString(newValue));
                appSettings.transmitEnabled = newValue;
            }
        }

        if (audio.containsKey(JsonKeys::TRANSMIT_VOLUME) || audio.containsKey("transmitVolume"))
        {
            int raw = audio.containsKey(JsonKeys::TRANSMIT_VOLUME) ? audio[JsonKeys::TRANSMIT_VOLUME].as<int>() : audio["transmitVolume"].as<int>();
            if (raw < 0) raw = 0;
            if (raw > 100) raw = 100;
            uint8_t newValue = static_cast<uint8_t>(raw);
            if (appSettings.transmitVolume != newValue)
            {
                logSettingChange("audio.transmitVolume",
                                 String(static_cast<unsigned>(appSettings.transmitVolume)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.transmitVolume = newValue;
            }
        }

#if defined(ECHO)
        if (audio.containsKey(JsonKeys::REPEATER_ENABLED) || audio.containsKey("repeaterEnabled"))
        {
            bool newValue = audio.containsKey(JsonKeys::REPEATER_ENABLED) ? audio[JsonKeys::REPEATER_ENABLED].as<bool>() : audio["repeaterEnabled"].as<bool>();
            if (newValue && !appSettings.transmitEnabled)
            {
                logWarnf("[Settings] Ignoring repeaterEnabled=true because TX is disabled");
                newValue = false;
            }
            if (appSettings.repeaterEnabled != newValue)
            {
                logSettingChange("repeater.enabled", boolToString(appSettings.repeaterEnabled), boolToString(newValue));
                appSettings.repeaterEnabled = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::REPEATER_MODE) || audio.containsKey("repeaterMode"))
        {
            int raw = audio.containsKey(JsonKeys::REPEATER_MODE) ? audio[JsonKeys::REPEATER_MODE].as<int>() : audio["repeaterMode"].as<int>();
            uint8_t newValue = (raw == 2) ? 2 : 1;
            if (appSettings.repeaterMode != newValue)
            {
                logSettingChange("repeater.mode",
                                 String(static_cast<unsigned>(appSettings.repeaterMode)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.repeaterMode = newValue;
            }
        }
#endif

        // CW settings (global)
        if (audio.containsKey(JsonKeys::CW_WPM) || audio.containsKey("cwWpm"))
        {
            int raw = audio.containsKey(JsonKeys::CW_WPM) ? audio[JsonKeys::CW_WPM].as<int>() : audio["cwWpm"].as<int>();
            if (raw < 5) raw = 5;
            if (raw > 40) raw = 40;
            uint8_t newValue = static_cast<uint8_t>(raw);
            if (appSettings.cwWpm != newValue)
            {
                logSettingChange("cw.wpm", String(static_cast<unsigned>(appSettings.cwWpm)), String(static_cast<unsigned>(newValue)));
                appSettings.cwWpm = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::CW_TONE_HZ) || audio.containsKey("cwToneHz"))
        {
            int raw = audio.containsKey(JsonKeys::CW_TONE_HZ) ? audio[JsonKeys::CW_TONE_HZ].as<int>() : audio["cwToneHz"].as<int>();
            if (raw < 200) raw = 200;
            if (raw > 2000) raw = 2000;
            uint16_t newValue = static_cast<uint16_t>(raw);
            if (appSettings.cwToneHz != newValue)
            {
                logSettingChange("cw.toneHz", String(static_cast<unsigned>(appSettings.cwToneHz)), String(static_cast<unsigned>(newValue)));
                appSettings.cwToneHz = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::CW_VOLUME) || audio.containsKey("cwVolume"))
        {
            int raw = audio.containsKey(JsonKeys::CW_VOLUME) ? audio[JsonKeys::CW_VOLUME].as<int>() : audio["cwVolume"].as<int>();
            if (raw < 0) raw = 0;
            if (raw > 100) raw = 100;
            uint8_t newValue = static_cast<uint8_t>(raw);
            if (appSettings.cwVolume != newValue)
            {
                logSettingChange("cw.volume", String(static_cast<unsigned>(appSettings.cwVolume)), String(static_cast<unsigned>(newValue)));
                appSettings.cwVolume = newValue;
            }
        }
        if (audio.containsKey(JsonKeys::CW_REPEAT) || audio.containsKey("cwRepeat"))
        {
            int raw = audio.containsKey(JsonKeys::CW_REPEAT) ? audio[JsonKeys::CW_REPEAT].as<int>() : audio["cwRepeat"].as<int>();
            if (raw < 1) raw = 1;
            if (raw > 10) raw = 10;
            uint8_t newValue = static_cast<uint8_t>(raw);
            if (appSettings.cwRepeat != newValue)
            {
                logSettingChange("cw.repeat", String(static_cast<unsigned>(appSettings.cwRepeat)), String(static_cast<unsigned>(newValue)));
                appSettings.cwRepeat = newValue;
            }
        }
    }

    bool hasUpload = (root.containsKey(JsonKeys::UPLOAD) || root.containsKey("upload"));
    if (hasUpload && (root.containsKey(JsonKeys::UPLOAD) ? root[JsonKeys::UPLOAD].is<JsonObject>() : root["upload"].is<JsonObject>()))
    {
        JsonObject upload = root.containsKey(JsonKeys::UPLOAD) ? root[JsonKeys::UPLOAD].as<JsonObject>() : root["upload"].as<JsonObject>();
        bool uploadSettingsChanged = false;
        if (upload.containsKey(JsonKeys::QUEUE_DEPTH) || upload.containsKey("queueDepth"))
        {
            uint8_t newValue = sanitizeQueueDepth(upload.containsKey(JsonKeys::QUEUE_DEPTH) ? upload[JsonKeys::QUEUE_DEPTH].as<unsigned long>() : upload["queueDepth"].as<unsigned long>());
            if (appSettings.upload.queueDepth != newValue)
            {
                logSettingChange("upload.queueDepth",
                                 String(appSettings.upload.queueDepth),
                                 String(newValue));
                appSettings.upload.queueDepth = newValue;
                uploadSettingsChanged = true;
            }
        }
        if (upload.containsKey(JsonKeys::CONVERT_TO_MP3) || upload.containsKey("convertToMp3"))
        {
            bool newValue = upload.containsKey(JsonKeys::CONVERT_TO_MP3) ? upload[JsonKeys::CONVERT_TO_MP3].as<bool>() : upload["convertToMp3"].as<bool>();
            if (appSettings.upload.convertToMp3 != newValue)
            {
                logSettingChange("upload.convertToMp3",
                                 boolToString(appSettings.upload.convertToMp3),
                                 boolToString(newValue));
                appSettings.upload.convertToMp3 = newValue;
                uploadSettingsChanged = true;
            }
        }
        bool hostListChanged = false;
        if (upload.containsKey(JsonKeys::API_HOSTS) || upload.containsKey("apiHosts"))
        {
            JsonVariant hostsVariant = upload.containsKey(JsonKeys::API_HOSTS) ? upload[JsonKeys::API_HOSTS] : upload["apiHosts"];
            if (hostsVariant.is<JsonArray>())
            {
                JsonArray hosts = hostsVariant.as<JsonArray>();
                size_t idx = 0;
                for (JsonVariant v : hosts)
                {
                    if (idx >= kApiEndpointCount)
                    {
                        break;
                    }
                    if (v.is<const char *>())
                    {
                        const char *raw = v.as<const char *>();
                        String newValue = String(raw ? raw : "");
                        String oldValue = String(appSettings.upload.apiHosts[idx]);
                        if (newValue != oldValue)
                        {
                            String key = String("upload.apiHosts[") + idx + "]";
                            logSettingChange(key.c_str(), oldValue, newValue);
                            assignUploadHost(idx, raw);
                            hostListChanged = true;
                        }
                    }
                    ++idx;
                }
            }
            else if (hostsVariant.is<JsonObject>())
            {
                JsonObject hostObject = hostsVariant.as<JsonObject>();
                for (size_t idx = 0; idx < kApiEndpointCount; ++idx)
                {
                    String keyName = String(idx);
                    if (!hostObject.containsKey(keyName))
                    {
                        continue;
                    }
                    JsonVariant valueVariant = hostObject[keyName];
                    if (!valueVariant.is<const char *>())
                    {
                        continue;
                    }
                    const char *raw = valueVariant.as<const char *>();
                    String newValue = String(raw ? raw : "");
                    String oldValue = String(appSettings.upload.apiHosts[idx]);
                    if (newValue != oldValue)
                    {
                        String key = String("upload.apiHosts[") + idx + "]";
                        logSettingChange(key.c_str(), oldValue, newValue);
                        assignUploadHost(idx, raw);
                        hostListChanged = true;
                    }
                }
            }
        }
        
        // Handle per-endpoint ports
        if ((upload.containsKey(JsonKeys::API_PORTS) || upload.containsKey("apiPorts")) && 
            (upload.containsKey(JsonKeys::API_PORTS) ? upload[JsonKeys::API_PORTS].is<JsonArray>() : upload["apiPorts"].is<JsonArray>()))
        {
            JsonArray ports = upload.containsKey(JsonKeys::API_PORTS) ? upload[JsonKeys::API_PORTS].as<JsonArray>() : upload["apiPorts"].as<JsonArray>();
            size_t idx = 0;
            for (JsonVariant v : ports)
            {
                if (idx >= kApiEndpointCount)
                {
                    break;
                }
                uint16_t newValue = v.as<uint16_t>();
                if (newValue == 0)
                {
                    // Port must be specified - skip if 0
                    ++idx;
                    continue;
                }
                if (appSettings.upload.apiPorts[idx] != newValue)
                {
                    String key = String("upload.apiPorts[") + idx + "]";
                    logSettingChange(key.c_str(), 
                                     String(appSettings.upload.apiPorts[idx]),
                                     String(newValue));
                    appSettings.upload.apiPorts[idx] = newValue;
                    uploadSettingsChanged = true;
                }
                ++idx;
            }
        }
        
        // Handle per-endpoint enabled flags
        if ((upload.containsKey(JsonKeys::ENABLED) || upload.containsKey("enabled")) && 
            (upload.containsKey(JsonKeys::ENABLED) ? upload[JsonKeys::ENABLED].is<JsonArray>() : upload["enabled"].is<JsonArray>()))
        {
            JsonArray enabledFlags = upload.containsKey(JsonKeys::ENABLED) ? upload[JsonKeys::ENABLED].as<JsonArray>() : upload["enabled"].as<JsonArray>();
            size_t idx = 0;
            for (JsonVariant v : enabledFlags)
            {
                if (idx >= kApiEndpointCount)
                {
                    break;
                }
                bool newValue = v.as<bool>();
                if (appSettings.upload.enabled[idx] != newValue)
                {
                    String key = String("upload.enabled[") + idx + "]";
                    logSettingChange(key.c_str(),
                                     boolToString(appSettings.upload.enabled[idx]),
                                     boolToString(newValue));
                    appSettings.upload.enabled[idx] = newValue;
                    uploadSettingsChanged = true;
                }
                ++idx;
            }
        }
        else
        {
            // Backward compatibility: regions 0,1,2 enabled; Custom (slot 3) disabled
            for (size_t i = 0; i < kApiEndpointCount; ++i)
            {
                appSettings.upload.enabled[i] = (i < 3);
            }
        }

        // Migration / backward compat: map old useCustomHost/customHost/customPort to slot 3 (Custom)
        if (upload.containsKey(JsonKeys::USE_CUSTOM_HOST) || upload.containsKey("useCustomHost"))
        {
            bool newVal = upload.containsKey(JsonKeys::USE_CUSTOM_HOST) ? upload[JsonKeys::USE_CUSTOM_HOST].as<bool>() : upload["useCustomHost"].as<bool>();
            if (appSettings.upload.enabled[3] != newVal)
            {
                logSettingChange("upload.enabled[3]", boolToString(appSettings.upload.enabled[3]), boolToString(newVal));
                appSettings.upload.enabled[3] = newVal;
                uploadSettingsChanged = true;
            }
        }
        if (upload.containsKey(JsonKeys::CUSTOM_HOST) || upload.containsKey("customHost"))
        {
            const char* raw = upload.containsKey(JsonKeys::CUSTOM_HOST) ? upload[JsonKeys::CUSTOM_HOST].as<const char*>() : upload["customHost"].as<const char*>();
            String val = raw ? String(raw) : String();
            if (val.length() > kMaxApiHostLength)
                val = val.substring(0, kMaxApiHostLength);
            if (String(appSettings.upload.apiHosts[3]) != val)
            {
                logSettingChange("upload.apiHosts[3]", String(appSettings.upload.apiHosts[3]), val);
                std::strncpy(appSettings.upload.apiHosts[3], val.c_str(), kMaxApiHostLength);
                appSettings.upload.apiHosts[3][kMaxApiHostLength] = '\0';
                uploadSettingsChanged = true;
            }
        }
        if (upload.containsKey(JsonKeys::CUSTOM_PORT) || upload.containsKey("customPort"))
        {
            uint32_t raw = upload.containsKey(JsonKeys::CUSTOM_PORT) ? upload[JsonKeys::CUSTOM_PORT].as<uint32_t>() : upload["customPort"].as<uint32_t>();
            uint16_t newVal = (raw >= 1 && raw <= 65535) ? static_cast<uint16_t>(raw) : DEFAULT_API_PORT;
            if (appSettings.upload.apiPorts[3] != newVal)
            {
                logSettingChange("upload.apiPorts[3]", String(appSettings.upload.apiPorts[3]), String(newVal));
                appSettings.upload.apiPorts[3] = newVal;
                uploadSettingsChanged = true;
            }
        }

        // Invalidate API endpoints if any upload setting changed (hosts, port, SSL, etc.)
        // This ensures endpoints are reinitialized with new settings and dead endpoints are reset
        if (hostListChanged || uploadSettingsChanged)
        {
            network_invalidateApiEndpoints();
        }
    }

    bool hasRtc = (root.containsKey(JsonKeys::RTC) || root.containsKey("rtc"));
    if (hasRtc && (root.containsKey(JsonKeys::RTC) ? root[JsonKeys::RTC].is<JsonObject>() : root["rtc"].is<JsonObject>()))
    {
        JsonObject rtc = root.containsKey(JsonKeys::RTC) ? root[JsonKeys::RTC].as<JsonObject>() : root["rtc"].as<JsonObject>();
        if (rtc.containsKey(JsonKeys::ENABLED) || rtc.containsKey("enabled"))
        {
            bool newValue = rtc.containsKey(JsonKeys::ENABLED) ? rtc[JsonKeys::ENABLED].as<bool>() : rtc["enabled"].as<bool>();
            if (appSettings.rtc.enabled != newValue)
            {
                logSettingChange("rtc.enabled",
                                 boolToString(appSettings.rtc.enabled),
                                 boolToString(newValue));
                appSettings.rtc.enabled = newValue;
            }
        }
        if (rtc.containsKey(JsonKeys::SDA_PIN) || rtc.containsKey("sdaPin"))
        {
            uint8_t newValue = static_cast<uint8_t>(rtc.containsKey(JsonKeys::SDA_PIN) ? rtc[JsonKeys::SDA_PIN].as<int>() : rtc["sdaPin"].as<int>());
            if (appSettings.rtc.sdaPin != newValue)
            {
                logSettingChange("rtc.sdaPin",
                                 String(static_cast<unsigned>(appSettings.rtc.sdaPin)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.rtc.sdaPin = newValue;
            }
        }
        if (rtc.containsKey(JsonKeys::SCL_PIN) || rtc.containsKey("sclPin"))
        {
            uint8_t newValue = static_cast<uint8_t>(rtc.containsKey(JsonKeys::SCL_PIN) ? rtc[JsonKeys::SCL_PIN].as<int>() : rtc["sclPin"].as<int>());
            if (appSettings.rtc.sclPin != newValue)
            {
                logSettingChange("rtc.sclPin",
                                 String(static_cast<unsigned>(appSettings.rtc.sclPin)),
                                 String(static_cast<unsigned>(newValue)));
                appSettings.rtc.sclPin = newValue;
            }
        }
    }

    // timezone
    bool hasTimezone = (root.containsKey(JsonKeys::TIMEZONE) || root.containsKey("timezone"));
    if (hasTimezone)
    {
        JsonObject timezone = root.containsKey(JsonKeys::TIMEZONE) ? root[JsonKeys::TIMEZONE].as<JsonObject>() : root["timezone"].as<JsonObject>();
        if (timezone.containsKey(JsonKeys::OFFSET_HOURS) || timezone.containsKey("offsetHours"))
        {
            int newValue = timezone.containsKey(JsonKeys::OFFSET_HOURS) ? timezone[JsonKeys::OFFSET_HOURS].as<int>() : timezone["offsetHours"].as<int>();
            // Clamp to valid range (-12 to +14)
            if (newValue < -12) newValue = -12;
            if (newValue > 14) newValue = 14;
            if (appSettings.timezone.offsetHours != newValue)
            {
                logSettingChange("timezone.offsetHours",
                                 String(appSettings.timezone.offsetHours),
                                 String(newValue));
                appSettings.timezone.offsetHours = static_cast<int8_t>(newValue);
                // Apply the new timezone immediately when loading from JSON
                timeKeeper().applyTimezoneFromSettings();
            }
        }
        if (timezone.containsKey(JsonKeys::MAINTENANCE_HOUR) || timezone.containsKey("maintenanceHour"))
        {
            int newValue = timezone.containsKey(JsonKeys::MAINTENANCE_HOUR) ? timezone[JsonKeys::MAINTENANCE_HOUR].as<int>() : timezone["maintenanceHour"].as<int>();
            // Clamp to valid range (0-23)
            if (newValue < 0) newValue = 0;
            if (newValue > 23) newValue = 23;
            if (appSettings.timezone.maintenanceHour != static_cast<uint8_t>(newValue))
            {
                logSettingChange("timezone.maintenanceHour",
                                 String(appSettings.timezone.maintenanceHour),
                                 String(newValue));
                appSettings.timezone.maintenanceHour = static_cast<uint8_t>(newValue);
            }
        }
        if (timezone.containsKey(JsonKeys::MAINTENANCE_MINUTE) || timezone.containsKey("maintenanceMinute"))
        {
            int newValue = timezone.containsKey(JsonKeys::MAINTENANCE_MINUTE) ? timezone[JsonKeys::MAINTENANCE_MINUTE].as<int>() : timezone["maintenanceMinute"].as<int>();
            // Clamp to valid range (0-59)
            if (newValue < 0) newValue = 0;
            if (newValue > 59) newValue = 59;
            if (appSettings.timezone.maintenanceMinute != static_cast<uint8_t>(newValue))
            {
                logSettingChange("timezone.maintenanceMinute",
                                 String(appSettings.timezone.maintenanceMinute),
                                 String(newValue));
                appSettings.timezone.maintenanceMinute = static_cast<uint8_t>(newValue);
            }
        }
    }

    // log
    bool hasLog = (root.containsKey(JsonKeys::LOG) || root.containsKey("log"));
    if (hasLog && (root.containsKey(JsonKeys::LOG) ? root[JsonKeys::LOG].is<JsonObject>() : root["log"].is<JsonObject>()))
    {
        JsonObject log = root.containsKey(JsonKeys::LOG) ? root[JsonKeys::LOG].as<JsonObject>() : root["log"].as<JsonObject>();
        if (log.containsKey(JsonKeys::SERIAL_FATAL) || log.containsKey("serialFatal"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_FATAL) ? log[JsonKeys::SERIAL_FATAL].as<bool>() : log["serialFatal"].as<bool>();
            if (appSettings.log.serialFatal != newValue)
            {
                logSettingChange("log.serialFatal",
                                 boolToString(appSettings.log.serialFatal),
                                 boolToString(newValue));
                appSettings.log.serialFatal = newValue;
            }
        }
        if (log.containsKey(JsonKeys::SERIAL_ERROR) || log.containsKey("serialError"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_ERROR) ? log[JsonKeys::SERIAL_ERROR].as<bool>() : log["serialError"].as<bool>();
            if (appSettings.log.serialError != newValue)
            {
                logSettingChange("log.serialError",
                                 boolToString(appSettings.log.serialError),
                                 boolToString(newValue));
                appSettings.log.serialError = newValue;
            }
        }
        if (log.containsKey(JsonKeys::SERIAL_WARNING) || log.containsKey("serialWarning"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_WARNING) ? log[JsonKeys::SERIAL_WARNING].as<bool>() : log["serialWarning"].as<bool>();
            if (appSettings.log.serialWarning != newValue)
            {
                logSettingChange("log.serialWarning",
                                 boolToString(appSettings.log.serialWarning),
                                 boolToString(newValue));
                appSettings.log.serialWarning = newValue;
            }
        }
        if (log.containsKey(JsonKeys::SERIAL_INFO) || log.containsKey("serialInfo"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_INFO) ? log[JsonKeys::SERIAL_INFO].as<bool>() : log["serialInfo"].as<bool>();
            if (appSettings.log.serialInfo != newValue)
            {
                logSettingChange("log.serialInfo",
                                 boolToString(appSettings.log.serialInfo),
                                 boolToString(newValue));
                appSettings.log.serialInfo = newValue;
            }
        }
        if (log.containsKey(JsonKeys::SERIAL_DEBUG) || log.containsKey("serialDebug"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_DEBUG) ? log[JsonKeys::SERIAL_DEBUG].as<bool>() : log["serialDebug"].as<bool>();
            if (appSettings.log.serialDebug != newValue)
            {
                logSettingChange("log.serialDebug",
                                 boolToString(appSettings.log.serialDebug),
                                 boolToString(newValue));
                appSettings.log.serialDebug = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_FATAL) || log.containsKey("fileFatal"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_FATAL) ? log[JsonKeys::FILE_FATAL].as<bool>() : log["fileFatal"].as<bool>();
            if (appSettings.log.fileFatal != newValue)
            {
                logSettingChange("log.fileFatal",
                                 boolToString(appSettings.log.fileFatal),
                                 boolToString(newValue));
                appSettings.log.fileFatal = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_ERROR) || log.containsKey("fileError"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_ERROR) ? log[JsonKeys::FILE_ERROR].as<bool>() : log["fileError"].as<bool>();
            if (appSettings.log.fileError != newValue)
            {
                logSettingChange("log.fileError",
                                 boolToString(appSettings.log.fileError),
                                 boolToString(newValue));
                appSettings.log.fileError = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_WARNING) || log.containsKey("fileWarning"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_WARNING) ? log[JsonKeys::FILE_WARNING].as<bool>() : log["fileWarning"].as<bool>();
            if (appSettings.log.fileWarning != newValue)
            {
                logSettingChange("log.fileWarning",
                                 boolToString(appSettings.log.fileWarning),
                                 boolToString(newValue));
                appSettings.log.fileWarning = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_INFO) || log.containsKey("fileInfo"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_INFO) ? log[JsonKeys::FILE_INFO].as<bool>() : log["fileInfo"].as<bool>();
            if (appSettings.log.fileInfo != newValue)
            {
                logSettingChange("log.fileInfo",
                                 boolToString(appSettings.log.fileInfo),
                                 boolToString(newValue));
                appSettings.log.fileInfo = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_DEBUG) || log.containsKey("fileDebug"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_DEBUG) ? log[JsonKeys::FILE_DEBUG].as<bool>() : log["fileDebug"].as<bool>();
            if (appSettings.log.fileDebug != newValue)
            {
                logSettingChange("log.fileDebug",
                                 boolToString(appSettings.log.fileDebug),
                                 boolToString(newValue));
                appSettings.log.fileDebug = newValue;
            }
        }
        if (log.containsKey(JsonKeys::SERIAL_EVENT) || log.containsKey("serialEvent"))
        {
            bool newValue = log.containsKey(JsonKeys::SERIAL_EVENT) ? log[JsonKeys::SERIAL_EVENT].as<bool>() : log["serialEvent"].as<bool>();
            if (appSettings.log.serialEvent != newValue)
            {
                logSettingChange("log.serialEvent",
                                 boolToString(appSettings.log.serialEvent),
                                 boolToString(newValue));
                appSettings.log.serialEvent = newValue;
            }
        }
        if (log.containsKey(JsonKeys::FILE_EVENT) || log.containsKey("fileEvent"))
        {
            bool newValue = log.containsKey(JsonKeys::FILE_EVENT) ? log[JsonKeys::FILE_EVENT].as<bool>() : log["fileEvent"].as<bool>();
            if (appSettings.log.fileEvent != newValue)
            {
                logSettingChange("log.fileEvent",
                                 boolToString(appSettings.log.fileEvent),
                                 boolToString(newValue));
                appSettings.log.fileEvent = newValue;
            }
        }
    }

    
    if (root.containsKey(JsonKeys::HOSTNAME) || root.containsKey("hostname"))
    {
        String hostname = root.containsKey(JsonKeys::HOSTNAME)
                            ? root[JsonKeys::HOSTNAME].as<String>()
                            : root["hostname"].as<String>();
        hostname.trim();
        hostname.toLowerCase();
        bool valid = hostname.length() > 0 && hostname.length() <= MAX_HOSTNAME_LENGTH;
        for (size_t i = 0; valid && i < hostname.length(); ++i)
        {
            const char c = hostname.charAt(i);
            const bool validCharacter = (c >= 'a' && c <= 'z') ||
                                        (c >= '0' && c <= '9') || c == '-';
            valid = validCharacter && !((i == 0 || i == hostname.length() - 1) && c == '-');
        }
        if (valid)
        {
            std::strncpy(appSettings.hostname, hostname.c_str(), MAX_HOSTNAME_LENGTH);
            appSettings.hostname[MAX_HOSTNAME_LENGTH] = '\0';
        }
    }

    if (root.containsKey(JsonKeys::MQTT_KEY) || root.containsKey("mqttKey"))
    {
        const String mqttKey = root.containsKey(JsonKeys::MQTT_KEY)
                                   ? root[JsonKeys::MQTT_KEY].as<String>()
                                   : root["mqttKey"].as<String>();
        if (mqttKey.length() <= kMaxMqttKeyLength)
        {
            std::strncpy(appSettings.mqttKey, mqttKey.c_str(), kMaxMqttKeyLength);
            appSettings.mqttKey[kMaxMqttKeyLength] = '\0';
        }
    }

    // wifiTxPower
    if (root.containsKey(JsonKeys::WIFI_TX_POWER) || root.containsKey("wifiTxPower"))
    {
        int newValue = root.containsKey(JsonKeys::WIFI_TX_POWER) ? root[JsonKeys::WIFI_TX_POWER].as<int>() : root["wifiTxPower"].as<int>();
        // Validate range (1-10)
        if (newValue >= 1 && newValue <= 10)
        {
            if (appSettings.wifiTxPower != static_cast<uint8_t>(newValue))
            {
                logSettingChange("wifiTxPower",
                                 String(static_cast<unsigned>(appSettings.wifiTxPower)),
                                 String(newValue));
                appSettings.wifiTxPower = static_cast<uint8_t>(newValue);
            }
        }
    }
    
    // webserverEnabled
    if (root.containsKey(JsonKeys::WEBSERVER_ENABLED) || root.containsKey("webserverEnabled"))
    {
        bool newValue = root.containsKey(JsonKeys::WEBSERVER_ENABLED) ? 
                        root[JsonKeys::WEBSERVER_ENABLED].as<bool>() : 
                        root["webserverEnabled"].as<bool>();
        if (appSettings.webserverEnabled != newValue)
        {
            logSettingChange("webserverEnabled",
                             appSettings.webserverEnabled ? "true" : "false",
                             newValue ? "true" : "false");
            appSettings.webserverEnabled = newValue;
        }
    }

    // ledStyle / startupMode / offlineMode (legacy LED behavior)
    if (root.containsKey(JsonKeys::LED_STYLE) || root.containsKey("ledStyle"))
    {
        int raw = root.containsKey(JsonKeys::LED_STYLE) ? root[JsonKeys::LED_STYLE].as<int>() : root["ledStyle"].as<int>();
        if (raw < 0) raw = 0;
        if (raw > 1) raw = 1;
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.ledStyle != newValue)
        {
            logSettingChange("ledStyle", String(static_cast<unsigned>(appSettings.ledStyle)), String(static_cast<unsigned>(newValue)));
            appSettings.ledStyle = newValue;
        }
    }
    if (root.containsKey(JsonKeys::STARTUP_MODE) || root.containsKey("startupMode"))
    {
        int raw = root.containsKey(JsonKeys::STARTUP_MODE) ? root[JsonKeys::STARTUP_MODE].as<int>() : root["startupMode"].as<int>();
        if (raw < 0) raw = 0;
        if (raw > 10) raw = 10;
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.startupMode != newValue)
        {
            logSettingChange("startupMode", String(static_cast<unsigned>(appSettings.startupMode)), String(static_cast<unsigned>(newValue)));
            appSettings.startupMode = newValue;
        }
    }
    if (root.containsKey(JsonKeys::OFFLINE_MODE) || root.containsKey("offlineMode"))
    {
        bool newValue = root.containsKey(JsonKeys::OFFLINE_MODE) ? root[JsonKeys::OFFLINE_MODE].as<bool>() : root["offlineMode"].as<bool>();
        if (appSettings.offlineMode != newValue)
        {
            logSettingChange("offlineMode", boolToString(appSettings.offlineMode), boolToString(newValue));
            appSettings.offlineMode = newValue;
        }
    }

#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    // Keep live recording/monitor path aligned with appSettings after any load/merge (e.g. NVS without `ric`).
    syncEffectiveRecordInputChannelFromAppSettings();
#endif

    return true;
}

bool settings_save()
{
    clearSettingsError();
    
    // Check if we should debounce
    unsigned long now = millis();
    if (g_lastSettingsChangeMs > 0 && (now - g_lastSettingsChangeMs) < SETTINGS_DEBOUNCE_MS)
    {
        // Mark as pending, will be saved later
        g_pendingSettingsSave = true;
        return true;  // Return success for now, actual save will happen later
    }
    
    // Reset debounce tracking
    g_lastSettingsChangeMs = 0;
    g_pendingSettingsSave = false;
    
    #ifdef ESP32
    // Use PSRAM for JSON document (saves 4KB heap)
    DynamicJsonDocument* docPtr = allocateJsonDocInPsram(kSettingsJsonDocCapacity);
    if (docPtr == nullptr)
    {
        // Fallback to heap if PSRAM allocation fails
        docPtr = new DynamicJsonDocument(kSettingsJsonDocCapacity);
    }
    DynamicJsonDocument& doc = *docPtr;
    #else
    DynamicJsonDocument doc(kSettingsJsonDocCapacity);
    #endif
    
    appSettingsToJson(doc);
    
    #ifdef ESP32
    // Use String on internal heap: VectorPSRAM asserts when PSRAM is missing or alloc fails
    // (same failure mode as SPIRAM queue fallback — hardware can report PSRAM ID read error).
    String out;
    out.reserve(kSettingsJsonDocCapacity);
    serializeJson(doc, out);
    if (out.length() == 0)
    {
        if (heap_caps_get_allocated_size(docPtr) > 0)
        {
            freeJsonDocFromPsram(docPtr, kSettingsJsonDocCapacity);
        }
        else
        {
            delete docPtr;
        }
        setSettingsError("Failed to serialize JSON");
        return false;
    }
    // Free PSRAM JSON document
    if (heap_caps_get_allocated_size(docPtr) > 0)
    {
        freeJsonDocFromPsram(docPtr, kSettingsJsonDocCapacity);
    }
    else
    {
        delete docPtr;
    }
    #else
    String out;
    serializeJson(doc, out);
    #endif
    
    bool ok = writeSettingsJson(out);
    if (ok)
    {
        clearSettingsError();
    }
    else
    {
        if (g_lastSettingsError.length() == 0)
        {
            setSettingsError("Failed to save settings to NVS");
        }
    }
    
    // Send settings updated event if save was successful
    if (ok)
    {
        DynamicJsonDocument eventData(256);
        eventData["message"] = "Settings updated";
        String eventMessage;
        serializeJson(eventData, eventMessage);
        sendEvent("settings_updated", eventMessage);
    }
    
    return ok;
}

// Clear debounce state to force immediate save on next settings_save() call
void settings_clearDebounce()
{
    g_lastSettingsChangeMs = 0;
    g_pendingSettingsSave = false;
}

String settings_getAllJson()
{
    DynamicJsonDocument doc(kSettingsJsonDocCapacity);
    appSettingsToJson(doc);
    String out;
    serializeJson(doc, out);
    return out;
}

// Produce a JSON string suitable for sending to the API server:
// - Based on settings_getAllJson()
// - WiFi SSIDs and passwords are replaced with "HIDDEN_FOR_SECURITY"
String settings_getMaskedJsonForServer()
{
    String settingsJson = settings_getAllJson();

    constexpr size_t kDocCapacity = 4096;
    DynamicJsonDocument doc(kDocCapacity);
    DeserializationError err = deserializeJson(doc, settingsJson);
    if (err)
    {
        // If we can't safely mask, fall back to original JSON
        return settingsJson;
    }

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
    return maskedJson;
}

// Helper: parse wifi parameter names. Supports forms:
//   "wifi.ssid"          -> index 0, field "ssid"
//   "wifi.0.ssid"        -> index 0, field "ssid"
//   "wifi[1].password"   -> index 1, field "password"
static bool parseWifiParam(const String &param, size_t &outIndex, String &outField)
{
    outIndex = 0;
    outField = String();
    String lowerParam = param;
    lowerParam.trim();
    lowerParam.toLowerCase();
    if (!lowerParam.startsWith("wifi"))
        return false;

    // Remove leading "wifi"
    String rest = lowerParam.substring(4);
    if (rest.startsWith("."))
    {
        // Could be ".ssid" or ".0.ssid"
        rest = rest.substring(1);
        // Check if starts with digit (index)
        int dotIdx = rest.indexOf('.');
        if (dotIdx == -1)
        {
            // No further dot - treat as field for index 0
            outField = rest;
            outIndex = 0;
            return true;
        }
        String first = rest.substring(0, dotIdx);
        String tail = rest.substring(dotIdx + 1);
        bool allDigits = true;
        for (size_t i = 0; i < first.length(); ++i)
        {
            char c = first.charAt(i);
            if (c < '0' || c > '9') { allDigits = false; break; }
        }
        if (allDigits && first.length() > 0)
        {
            int idx = first.toInt();
            if (idx < 0) idx = 0;
            outIndex = static_cast<size_t>(idx);
            outField = tail;
            return true;
        }
        else
        {
            // first is a field name
            outIndex = 0;
            outField = rest;
            return true;
        }
    }
    else if (rest.startsWith("["))
    {
        int close = rest.indexOf(']');
        if (close == -1) return false;
        String idxStr = rest.substring(1, close);
        int idx = idxStr.toInt();
        if (idx < 0) idx = 0;
        outIndex = static_cast<size_t>(idx);
        if (rest.length() > (size_t)close + 2 && rest.charAt(close + 1) == '.')
        {
            outField = rest.substring(close + 2);
            return true;
        }
        return false;
    }

    return false;
}

static bool parseUploadHostParam(const String &param, size_t &outIndex)
{
    outIndex = 0;
    String lowerParam = param;
    lowerParam.trim();
    lowerParam.toLowerCase();
    if (!lowerParam.startsWith("upload.apihosts"))
    {
        return false;
    }

    int open = lowerParam.indexOf('[');
    int close = lowerParam.indexOf(']', open + 1);
    if (open == -1 || close == -1 || close <= open + 1)
    {
        return false;
    }

    String indexText = lowerParam.substring(open + 1, close);
    for (int i = 0; i < indexText.length(); ++i)
    {
        char c = indexText.charAt(i);
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    int parsed = indexText.toInt();
    if (parsed < 0)
    {
        return false;
    }

    outIndex = static_cast<size_t>(parsed);
    return true;
}

static bool parseUploadEndpointParam(const String &param, const char *prefix, size_t &outIndex)
{
    outIndex = 0;
    String lowerParam = param;
    lowerParam.trim();
    lowerParam.toLowerCase();
    if (!lowerParam.startsWith(prefix))
    {
        return false;
    }

    int open = lowerParam.indexOf('[');
    int close = lowerParam.indexOf(']', open + 1);
    if (open == -1 || close == -1 || close <= open + 1)
    {
        return false;
    }

    String indexText = lowerParam.substring(open + 1, close);
    for (int i = 0; i < indexText.length(); ++i)
    {
        char c = indexText.charAt(i);
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    int parsed = indexText.toInt();
    if (parsed < 0)
    {
        return false;
    }

    outIndex = static_cast<size_t>(parsed);
    return true;
}

static String normalizeParamAlias(const String &param)
{
    String trimmed = param;
    trimmed.trim();
    if (trimmed.length() == 0)
    {
        return trimmed;
    }

    String lower = trimmed;
    lower.toLowerCase();

    auto parseIndexSuffix = [](const String &suffix, size_t &index) -> bool
    {
        if (suffix.length() == 0)
        {
            index = 0;
            return true;
        }
        for (int i = 0; i < suffix.length(); ++i)
        {
            char c = suffix.charAt(i);
            if (c < '0' || c > '9')
            {
                return false;
            }
        }
        long parsed = suffix.toInt();
        if (parsed < 0)
        {
            return false;
        }
        index = static_cast<size_t>(parsed);
        return true;
    };

    auto makeWifiKey = [](size_t index, const char *field) -> String
    {
        String out("wifi[");
        out += static_cast<unsigned long>(index);
        out += "].";
        out += field;
        return out;
    };

    if (lower == "min")
    {
        return String("audio.minRecordingMs");
    }
    if (lower == "max")
    {
        return String("audio.maxRecordingMs");
    }
    if (lower == "pre" || lower == "preroll" || lower == "prerecord")
    {
        return String("audio.preRecordMs");
    }
    if (lower == "silence")
    {
        return String("audio.silenceThresholdMs");
    }
    if (lower == "gain")
    {
        return String("audio.codecGain");
    }
    if (lower == "athr")
    {
        return String("audio.audioThreshold");
    }
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    if (lower == "inchan" || lower == "inputchan" || lower == "recch")
    {
        return String("audio.recordInputChannel");
    }
#endif

    if (lower.startsWith("host") && lower.length() > 4)
    {
        String idxStr = lower.substring(4);
        bool allDigits = true;
        for (size_t i = 0; i < idxStr.length(); ++i)
        {
            char c = idxStr.charAt(i);
            if (c < '0' || c > '9')
            {
                allDigits = false;
                break;
            }
        }
        if (allDigits)
        {
            return String("upload.apiHosts[") + idxStr + "]";
        }
    }

    if (lower == "customhost") return String("upload.customHost");
    if (lower == "customport") return String("upload.customPort");
    if (lower == "usecustomhost") return String("upload.useCustomHost");
    if (lower == "region0" || lower == "ohio") return String("upload.enabled[0]");
    if (lower == "region1" || lower == "oregon") return String("upload.enabled[1]");
    if (lower == "region2" || lower == "virginia") return String("upload.enabled[2]");
    if (lower == "region3" || lower == "custom") return String("upload.enabled[3]");

    if (lower.startsWith("ssid"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(4), index))
        {
            return makeWifiKey(index, "ssid");
        }
    }
    if (lower.startsWith("pass"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(4), index))
        {
            return makeWifiKey(index, "password");
        }
    }
    if (lower.startsWith("pwd"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(3), index))
        {
            return makeWifiKey(index, "password");
        }
    }
    if (lower.startsWith("ip"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(2), index))
        {
            return makeWifiKey(index, "staticIp");
        }
    }
    if (lower.startsWith("subnet"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(6), index))
        {
            return makeWifiKey(index, "staticSubnet");
        }
    }
    if (lower.startsWith("mask"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(4), index))
        {
            return makeWifiKey(index, "staticSubnet");
        }
    }
    if (lower.startsWith("gateway"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(7), index))
        {
            return makeWifiKey(index, "staticGateway");
        }
    }
    if (lower.startsWith("gw"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(2), index))
        {
            return makeWifiKey(index, "staticGateway");
        }
    }
    if (lower.startsWith("dns1"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(4), index))
        {
            return makeWifiKey(index, "staticDns1");
        }
    }
    if (lower.startsWith("dns2"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(4), index))
        {
            return makeWifiKey(index, "staticDns2");
        }
    }
    if (lower.startsWith("static"))
    {
        size_t index = 0;
        if (parseIndexSuffix(lower.substring(6), index))
        {
            return makeWifiKey(index, "staticIpEnabled");
        }
    }

    return trimmed;
}

bool settings_updateAllFromJson(const String &json, bool saveAsync)
{
    if (json.length() == 0)
    {
        setSettingsError("Empty JSON payload");
        return false;
    }

    #ifdef ESP32
    // Use PSRAM for JSON document (saves 4KB heap)
    DynamicJsonDocument* docPtr = allocateJsonDocInPsram(kSettingsJsonDocCapacity);
    if (docPtr == nullptr)
    {
        // Fallback to heap if PSRAM allocation fails
        docPtr = new DynamicJsonDocument(kSettingsJsonDocCapacity);
    }
    DynamicJsonDocument& doc = *docPtr;
    #else
    DynamicJsonDocument doc(kSettingsJsonDocCapacity);
    #endif
    
    DeserializationError err = deserializeJson(doc, json);
    if (err)
    {
        String preview = json;
        if (preview.length() > 128)
        {
            preview = preview.substring(0, 128) + "...";
        }
        setSettingsError(String("JSON parse failed: ") + err.c_str() + " (len=" + json.length() + ")");
        return false;
    }

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull())
    {
        setSettingsError("JSON root object is null");
        return false;
    }

    // ---------------------------------------------------------------------
    // Version compatibility check
    // For settings imported from external sources (web UI, server, etc.)
    // we require the firmware and config versions in the JSON to match
    // the running firmware. If they don't, refuse to apply with a clear
    // error message so the caller can inform the user.
    // ---------------------------------------------------------------------
    const char *incomingFirmware = nullptr;
    const char *incomingConfigVersion = nullptr;

    // Firmware: short key "fw" or legacy "firmware"
    if (root.containsKey(JsonKeys::FIRMWARE_KEY) && root[JsonKeys::FIRMWARE_KEY].is<const char *>())
    {
        incomingFirmware = root[JsonKeys::FIRMWARE_KEY].as<const char *>();
    }
    else if (root.containsKey("firmware") && root["firmware"].is<const char *>())
    {
        incomingFirmware = root["firmware"].as<const char *>();
    }

    // Config version: short key "cv" or legacy "configVersion"
    if (root.containsKey(JsonKeys::CONFIG_VERSION_KEY) && root[JsonKeys::CONFIG_VERSION_KEY].is<const char *>())
    {
        incomingConfigVersion = root[JsonKeys::CONFIG_VERSION_KEY].as<const char *>();
    }
    else if (root.containsKey("configVersion") && root["configVersion"].is<const char *>())
    {
        incomingConfigVersion = root["configVersion"].as<const char *>();
    }

    if (incomingConfigVersion && strcmp(incomingConfigVersion, CONFIG_VERSION) != 0)
    {
        String msg = String("Config version mismatch: device ") + CONFIG_VERSION +
                     ", imported " + incomingConfigVersion;
        setSettingsError(msg);
        return false;
    }

    if (incomingFirmware && strcmp(incomingFirmware, FIRMWARE) != 0)
    {
        String msg = String("Firmware version mismatch: device ") + FIRMWARE +
                     ", imported " + incomingFirmware;
        setSettingsError(msg);
        return false;
    }

    // Apply values into appSettings (jsonToAppSettings will keep defaults for missing keys)
    if (!jsonToAppSettings(root))
    {
        setSettingsError("Failed to apply settings from JSON");
        return false;
    }

    updateCodecGainFromSettings();
    
    // Check if WiFi credentials were added and start tasks if needed
    system_checkAndStartTasksIfWiFiConfigured();

    if (saveAsync)
    {
        bool scheduled = settings_commitAsync();
        if (!scheduled)
        {
            setSettingsError("Failed to schedule async settings save");
        }
        else
        {
            clearSettingsError();
        }
        return scheduled;
    }
    else
    {
        bool saved = settings_save();
        if (saved)
        {
            clearSettingsError();
        }
        return saved;
    }
}

// Apply a full settings JSON payload that originated from the API server.
// This behaves like settings_updateAllFromJson(json, false) but:
// - Strips any WiFi SSID/password fields that were masked as "HIDDEN_FOR_SECURITY"
// - Suppresses per-setting change events during the apply to avoid loops
bool settings_applyJsonFromServer(const String &json)
{
    if (json.length() == 0)
    {
        setSettingsError("Empty JSON payload from server");
        return false;
    }

    constexpr size_t kDocCapacity = 4096;
    DynamicJsonDocument doc(kDocCapacity);
    DeserializationError err = deserializeJson(doc, json);
    if (err)
    {
        setSettingsError(String("JSON parse failed: ") + err.c_str());
        return false;
    }

    if (doc[JsonKeys::MQTT_KEY] == "HIDDEN_FOR_SECURITY") doc.remove(JsonKeys::MQTT_KEY);
    if (doc["mqttKey"] == "HIDDEN_FOR_SECURITY") doc.remove("mqttKey");

    // Strip WiFi SSID/password fields that are masked as "HIDDEN_FOR_SECURITY"
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

    // Suppress per-setting change events during bulk apply to avoid loops
    bool previousSuppress = g_suppressSettingChangeEvents;
    g_suppressSettingChangeEvents = true;
    bool ok = settings_updateAllFromJson(cleanedJson, false);
    g_suppressSettingChangeEvents = previousSuppress;
    return ok;
}

// Internal helper: send a per-setting change event to the server.
// Uses /api/v1/events via sendEvent("setting_changed", ...).
static void sendSettingChangeEvent(const char *key,
                                   const String &oldValue,
                                   const String &newValue,
                                   bool sensitive)
{
    if (g_suppressSettingChangeEvents)
    {
        return;
    }

    const char *name = key ? key : "(unknown)";

    // Do not send WiFi network credentials (SSID/password) as events
    // Keys typically look like "wifi[0].ssid", "wifi[0].password", etc.
    if (strncmp(name, "wifi[", 5) == 0 || strncmp(name, "wifi.", 5) == 0)
    {
        return;
    }

    DynamicJsonDocument doc(256);
    doc["key"] = name;

    if (sensitive)
    {
        doc["sensitive"] = true;
        doc["old_length"] = static_cast<unsigned>(oldValue.length());
        doc["new_length"] = static_cast<unsigned>(newValue.length());
    }
    else
    {
        doc["old"] = oldValue;
        doc["new"] = newValue;
        doc["sensitive"] = false;
    }

    String payload;
    serializeJson(doc, payload);
    sendEvent("setting_changed", payload);
}

static void settings_save_task(void *pvParameters)
{
    (void)pvParameters;
    
    // Check if there's a pending save from debouncing
    unsigned long now = millis();
    if (g_pendingSettingsSave && g_lastSettingsChangeMs > 0)
    {
        unsigned long elapsed = now - g_lastSettingsChangeMs;
        if (elapsed < SETTINGS_DEBOUNCE_MS)
        {
            // Still debouncing, wait a bit more
            vTaskDelay(pdMS_TO_TICKS(SETTINGS_DEBOUNCE_MS - elapsed));
        }
    }
    
    settings_save();
    vTaskDelete(NULL);
}

bool settings_commitAsync()
{
    // Pinned to Core 0 for networking/temporary tasks
    BaseType_t r = xTaskCreatePinnedToCore(settings_save_task, "SettingsSave", 4096, NULL, 1, NULL, 0);
    if (r != pdPASS)
    {
        return false;
    }
    return true;
}

// Get NVS settings health metrics (for diagnostics)
void settings_getEepromHealth(String& output)
{
    output = "{";
    output += "\"totalWrites\":" + String(g_settingsHealth.totalWrites) + ",";
    output += "\"successfulWrites\":" + String(g_settingsHealth.successfulWrites) + ",";
    output += "\"failedWrites\":" + String(g_settingsHealth.failedWrites) + ",";
    output += "\"totalReads\":" + String(g_settingsHealth.totalReads) + ",";
    output += "\"successfulReads\":" + String(g_settingsHealth.successfulReads) + ",";
    output += "\"failedReads\":" + String(g_settingsHealth.failedReads) + ",";
    output += "\"corruptionDetected\":" + String(g_settingsHealth.corruptionDetected) + ",";
    output += "\"lastWriteMs\":" + String(g_settingsHealth.lastWriteMs) + ",";
    output += "\"lastReadMs\":" + String(g_settingsHealth.lastReadMs) + ",";
    output += "\"lastFailureMs\":" + String(g_settingsHealth.lastFailureMs);
    output += "}";
}

bool settings_reload()
{
    String json = readSettingsJson();
    if (json.length() == 0)
    {
        setSettingsError("No settings stored in NVS");
        return false;
    }

   // Serial.printf("[Settings] Attempting to parse JSON (length %d): %s\n", json.length(), json.c_str());

    DynamicJsonDocument doc(kSettingsJsonDocCapacity);
    DeserializationError err = deserializeJson(doc, json);
    if (err)
    {
        setSettingsError(String("Stored JSON parse failed: ") + err.c_str());
        return false;
    }

    JsonObject root = doc.as<JsonObject>();
    if (!root.isNull())
    {
       // Serial.println("[Settings] Successfully parsed JSON to object");
        jsonToAppSettings(root);
       // Serial.println("[Settings] Loaded from NVS");
        updateCodecGainFromSettings();
        // Apply timezone from loaded settings (if timekeeper is initialized)
        // Note: During initial startup, timekeeper.begin() will apply it, but this handles reloads
        timeKeeper().applyTimezoneFromSettings();
        clearSettingsError();
        return true;
    }
    else
    {
        setSettingsError("Failed to convert stored JSON to object");
        return false;
    }
}

bool settings_factoryReset(bool emitCliResponse)
{
#ifdef ESP32
    if (WiFi.isConnected())
    {
        WiFi.disconnect(true, true);
        delay(500);
    }
    esp_wifi_restore();
#endif

    Preferences prefs;
    if (!nvs_openNamespace(kSettingsNamespace, false, prefs))
        return false;

    nvs_removeKey(prefs, kKeySlot0);
    nvs_removeKey(prefs, kKeySlot1);
    nvs_removeKey(prefs, kKeyActiveSlot);
    nvs_closeNamespace(prefs);
    nvs_clearNamespace(kSettingsNamespace);

    if (emitCliResponse)
    {
        DynamicJsonDocument doc(256);
        doc["status"] = "ok";
        doc["action"] = "factory_reset";
        doc["reboot_pending"] = true;
        String out;
        serializeJson(doc, out);
        Serial.println(out);
    }

    logger_flush();
    Serial.flush();
    delay(2000);
    ESP.restart();
    return true;
}

bool settings_begin()
{
    
    // Initialize mutexes
    ensureSettingsMutex();
    ensureSerialMutex();
    
    // NVS is already initialized by nvs_begin() in main.cpp
    // Just verify we can open the settings namespace
    Preferences prefs;
    if (!nvs_openNamespace(kSettingsNamespace, true, prefs))  // Read-only for check
    {
        setSettingsError("Failed to open NVS settings namespace");
        return false;
    }
    nvs_closeNamespace(prefs);
    
    // Check if settings keys exist in NVS (to distinguish "blank" from "read error")
    bool settingsKeysExist = checkSettingsKeysExist();
    
    String json = readSettingsJson();
    
    // Backup critical settings before any reset
    String preservedHosts[kApiEndpointCount];
    String preservedWifi[kMaxWifiCredentials][2];  // SSID and password
    bool hasPreservedHosts = false;
    bool hasPreservedWifi = false;
    
    if (json.length() > 0)
    {
        DynamicJsonDocument doc(kSettingsJsonDocCapacity);
        DeserializationError err = deserializeJson(doc, json);
        if (!err)
        {
            JsonObject root = doc.as<JsonObject>();
            
            // Preserve upload hosts
            if (root.containsKey("upload"))
            {
                JsonObject upload = root["upload"];
                if (upload.containsKey("apiHosts") && upload["apiHosts"].is<JsonArray>())
                {
                    JsonArray hosts = upload["apiHosts"];
                    for (size_t i = 0; i < kApiEndpointCount && i < hosts.size(); ++i)
                    {
                        if (hosts[i].is<const char*>())
                        {
                            preservedHosts[i] = String(hosts[i].as<const char*>());
                            if (preservedHosts[i].length() > 0)
                            {
                                hasPreservedHosts = true;
                            }
                        }
                    }
                }
            }
            
            // Preserve WiFi credentials
            if (root.containsKey("wifi"))
            {
                JsonVariant wifiVar = root["wifi"];
                if (wifiVar.is<JsonArray>())
                {
                    JsonArray wifiArr = wifiVar.as<JsonArray>();
                    for (size_t i = 0; i < kMaxWifiCredentials && i < wifiArr.size(); ++i)
                    {
                        if (wifiArr[i].is<JsonObject>())
                        {
                            JsonObject w = wifiArr[i].as<JsonObject>();
                            if (w.containsKey("ssid") && w["ssid"].is<const char*>())
                            {
                                preservedWifi[i][0] = String(w["ssid"].as<const char*>());
                            }
                            if (w.containsKey("password") && w["password"].is<const char*>())
                            {
                                preservedWifi[i][1] = String(w["password"].as<const char*>());
                            }
                            if (preservedWifi[i][0].length() > 0)
                            {
                                hasPreservedWifi = true;
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Check if we need to create default settings
    // Only create defaults if NVS is blank (no keys exist), NOT if read failed
    bool createDefaults = (json.length() == 0 && !settingsKeysExist);
    bool needsMigration = false;
    
    // If keys exist but read failed, that's an error condition
    if (json.length() == 0 && settingsKeysExist)
    {
        setSettingsError("NVS read failed - settings keys exist but cannot be read");
        return false;  // Don't create defaults on read failure
    }
    
    // If we have settings, check config version (not firmware version)
    if (!createDefaults)
    {
        DynamicJsonDocument doc(kSettingsJsonDocCapacity);
        DeserializationError err = deserializeJson(doc, json);
        if (!err)
        {
            JsonObject root = doc.as<JsonObject>();
            if (root.containsKey("configVersion"))
            {
                const char* storedConfigVersion = root["configVersion"].as<const char*>();
                if (strcmp(storedConfigVersion, CONFIG_VERSION) != 0)
                {
                    needsMigration = true;
                    // Don't create defaults - migrate instead
                }
            }
            else
            {
                needsMigration = true;
            }
        }
        else
        {
            createDefaults = true;
            setSettingsError(String("Failed to parse settings during init: ") + err.c_str());
        }
    }
    
    // Helper function to create and write default settings
    auto createAndWriteDefaults = [&]() -> bool {
        // Create defaults from current appSettings and save
        DynamicJsonDocument doc(kSettingsJsonDocCapacity);
        appSettingsToJson(doc);
        
        // Restore preserved settings
        if (hasPreservedHosts)
        {
            JsonObject upload = doc["upload"];
            JsonArray apiHosts = upload["apiHosts"];
            for (size_t i = 0; i < kApiEndpointCount; ++i)
            {
                if (preservedHosts[i].length() > 0)
                {
                    apiHosts[i] = preservedHosts[i];
                }
            }
        }
        
        if (hasPreservedWifi)
        {
            JsonArray wifiArr = doc["wifi"];
            for (size_t i = 0; i < kMaxWifiCredentials; ++i)
            {
                if (preservedWifi[i][0].length() > 0)
                {
                    JsonObject w = wifiArr[i];
                    w["ssid"] = preservedWifi[i][0];
                    if (preservedWifi[i][1].length() > 0)
                    {
                        w["password"] = preservedWifi[i][1];
                    }
                }
            }
        }
        
        // Add firmware version and config version
        doc["firmware"] = FIRMWARE;
        doc["configVersion"] = CONFIG_VERSION;
        String out;
        serializeJson(doc, out);
        
        if (!writeSettingsJson(out))
        {
            String errorMsg = settings_getLastError();
            if (errorMsg.length() > 0)
            {
            }
            if (g_lastSettingsError.length() == 0)
            {
                setSettingsError("Failed to write default settings to NVS");
            }
            return false;
        }
        clearSettingsError();
        return true;
    };
    
    if (createDefaults)
    {
        return createAndWriteDefaults();
    }
    else if (needsMigration)
    {
        
        // Load existing settings
        DynamicJsonDocument existingDoc(kSettingsJsonDocCapacity);
        DeserializationError err = deserializeJson(existingDoc, json);
        if (err)
        {
            // Create defaults instead of migrating
            return createAndWriteDefaults();
        }
        
        // Create default settings document
        DynamicJsonDocument defaultDoc(kSettingsJsonDocCapacity);
        appSettingsToJson(defaultDoc);
        
        // Merge: preserve existing settings, add missing defaults
        // Start with existing settings as base
        DynamicJsonDocument mergedDoc(kSettingsJsonDocCapacity);
        JsonObject existingRoot = existingDoc.as<JsonObject>();
        JsonObject mergedRoot = mergedDoc.to<JsonObject>();
        
        // Copy all existing settings first (preserve everything)
        for (JsonPair pair : existingRoot) {
            mergedRoot[pair.key().c_str()] = pair.value();
        }
        
        // Now merge defaults into merged doc (only adds missing keys)
        JsonObject defaultRoot = defaultDoc.as<JsonObject>();
        mergeJsonObjects(mergedRoot, defaultRoot);
        
        // Update version fields
        mergedRoot["firmware"] = FIRMWARE;
        mergedRoot["configVersion"] = CONFIG_VERSION;
        
        // Restore preserved critical settings (in case they got lost)
        if (hasPreservedHosts)
        {
            JsonObject upload = mergedRoot["upload"];
            if (!upload.isNull())
            {
                JsonArray apiHosts = upload["apiHosts"];
                if (apiHosts.isNull())
                {
                    apiHosts = upload.createNestedArray("apiHosts");
                }
                for (size_t i = 0; i < kApiEndpointCount; ++i)
                {
                    if (preservedHosts[i].length() > 0)
                    {
                        if (i >= apiHosts.size())
                        {
                            apiHosts.add(preservedHosts[i]);
                        }
                        else
                        {
                            apiHosts[i] = preservedHosts[i];
                        }
                    }
                }
            }
        }
        
        if (hasPreservedWifi)
        {
            JsonArray wifiArr = mergedRoot["wifi"];
            if (wifiArr.isNull())
            {
                wifiArr = mergedRoot.createNestedArray("wifi");
            }
            for (size_t i = 0; i < kMaxWifiCredentials; ++i)
            {
                if (preservedWifi[i][0].length() > 0)
                {
                    JsonObject w;
                    if (i >= wifiArr.size())
                    {
                        w = wifiArr.createNestedObject();
                    }
                    else
                    {
                        w = wifiArr[i];
                    }
                    w["ssid"] = preservedWifi[i][0];
                    if (preservedWifi[i][1].length() > 0)
                    {
                        w["password"] = preservedWifi[i][1];
                    }
                }
            }
        }
        
        // Save migrated settings
        String out;
        serializeJson(mergedDoc, out);
        
        if (!writeSettingsJson(out))
        {
            String errorMsg = settings_getLastError();
            if (errorMsg.length() > 0)
            {
            }
            setSettingsError("Failed to write migrated settings to NVS");
            return false;
        }
        
        clearSettingsError();
        
        // Reload to apply migrated settings
        return settings_reload();
    }
    
    // Try to load existing settings
    return settings_reload();
}

String settings_getParam(const String &param)
{
    String normalizedParam = normalizeParamAlias(param);
    String lowerParam = normalizedParam;
    lowerParam.toLowerCase();

    if (lowerParam == "hostname" || lowerParam == "hn")
        return String(appSettings.hostname);

    if (lowerParam == "mqtt.key" || lowerParam == "mk")
        return String(appSettings.mqttKey[0] ? "[configured]" : "");

    // Support a small set of dot-notated params
    size_t widx = 0;
    String wfield;
    if (parseWifiParam(lowerParam, widx, wfield))
    {
        if (widx >= kMaxWifiCredentials)
            return String();
        if (wfield == "ssid")
            return String(appSettings.wifi[widx].ssid ? appSettings.wifi[widx].ssid : "");
        if (wfield == "password")
            return String(appSettings.wifi[widx].password ? appSettings.wifi[widx].password : "");
        if (wfield == "connecttimeoutms")
            return String(appSettings.wifi[widx].connectTimeoutMs);
        if (wfield == "staticip")
            return String(appSettings.wifi[widx].staticIp);
        if (wfield == "staticsubnet")
            return String(appSettings.wifi[widx].staticSubnet);
        if (wfield == "staticgateway")
            return String(appSettings.wifi[widx].staticGateway);
        if (wfield == "staticdns1")
            return String(appSettings.wifi[widx].staticDns1);
        if (wfield == "staticdns2")
            return String(appSettings.wifi[widx].staticDns2);
        if (wfield == "staticipenabled")
            return String(appSettings.wifi[widx].staticIpEnabled ? "true" : "false");
    }

    if (lowerParam == "audio.samplerate")
        return String(appSettings.audio.sampleRate);
    if (lowerParam == "audio.buffersamples")
        return String(appSettings.audio.bufferSamples);
    if (lowerParam == "audio.prerecordms")
        return String(appSettings.audio.preRecordMs);
    if (lowerParam == "audio.audiothreshold")
        return String(appSettings.audio.audioThreshold);
    if (lowerParam == "audio.minrecordingms")
        return String(appSettings.audio.minRecordingMs);
    if (lowerParam == "audio.maxrecordingms")
        return String(appSettings.audio.maxRecordingMs);
    if (lowerParam == "audio.silencethresholdms")
        return String(appSettings.audio.silenceThresholdMs);
    if (lowerParam == "audio.codecgain")
        return String(appSettings.audio.codecGainDb);
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    if (lowerParam == "audio.recordinputchannel")
        return String(static_cast<unsigned>(appSettings.audio.recordInputChannel));
#endif
    if (lowerParam == "audio.speakerenabled")
        return String(appSettings.speakerEnabled ? "true" : "false");
    if (lowerParam == "audio.speakervolume")
        return String(static_cast<unsigned>(appSettings.speakerVolume));
    if (lowerParam == "audio.transmitenabled")
        return String(appSettings.transmitEnabled ? "true" : "false");
    if (lowerParam == "audio.transmitvolume")
        return String(static_cast<unsigned>(appSettings.transmitVolume));

#if defined(ECHO)
    if (lowerParam == "repeater.enabled")
        return String(appSettings.repeaterEnabled ? "true" : "false");
    if (lowerParam == "repeater.mode")
        return String(static_cast<unsigned>(appSettings.repeaterMode));
#endif

    if (lowerParam == "cw.wpm")
        return String(static_cast<unsigned>(appSettings.cwWpm));
    if (lowerParam == "cw.tonehz")
        return String(static_cast<unsigned>(appSettings.cwToneHz));
    if (lowerParam == "cw.volume")
        return String(static_cast<unsigned>(appSettings.cwVolume));
    if (lowerParam == "cw.repeat")
        return String(static_cast<unsigned>(appSettings.cwRepeat));

    if (lowerParam == "upload.queuedepth")
        return String(appSettings.upload.queueDepth);
    if (lowerParam == "upload.converttomp3")
        return String(appSettings.upload.convertToMp3 ? "true" : "false");

    if (lowerParam == "rtc.enabled")
        return String(appSettings.rtc.enabled ? "true" : "false");
    if (lowerParam == "rtc.sdapin")
        return String(appSettings.rtc.sdaPin);
    if (lowerParam == "rtc.sclpin")
        return String(appSettings.rtc.sclPin);

    if (lowerParam == "sdcard.usesdcard")
        return String(appSettings.sdCard.useSdCard ? "true" : "false");
    if (lowerParam == "sdcard.recordtosdcard")
        return String(appSettings.sdCard.recordToSdCard ? "true" : "false");
    if (lowerParam == "sdcard.mode1bit")
        return String(appSettings.sdCard.mode1bit ? "true" : "false");
    if (lowerParam == "sdcard.frequency")
        return String(appSettings.sdCard.frequency);
    if (lowerParam == "sdcard.formatifmountfailed")
        return String(appSettings.sdCard.formatIfMountFailed ? "true" : "false");

    size_t hostIndex = 0;
    if (parseUploadHostParam(lowerParam, hostIndex))
    {
        if (hostIndex < kApiEndpointCount)
        {
            return String(appSettings.upload.apiHosts[hostIndex]);
        }
        return String();
    }

    size_t endpointIndex = 0;
    if (parseUploadEndpointParam(lowerParam, "upload.apiports", endpointIndex))
    {
        if (endpointIndex < kApiEndpointCount)
        {
            return String(appSettings.upload.apiPorts[endpointIndex]);
        }
        return String();
    }

    if (parseUploadEndpointParam(lowerParam, "upload.enabled", endpointIndex))
    {
        if (endpointIndex < kApiEndpointCount)
        {
            return boolToString(appSettings.upload.enabled[endpointIndex]);
        }
        return String();
    }

    // Custom = slot 3 in the same array
    if (lowerParam == "upload.usecustomhost")
        return boolToString(appSettings.upload.enabled[3]);
    if (lowerParam == "upload.customhost")
        return String(appSettings.upload.apiHosts[3]);
    if (lowerParam == "upload.customport")
        return String(appSettings.upload.apiPorts[3]);

    return String();
}

bool settings_setParam(const String &param, const String &value)
{
    clearSettingsError();

    // Track when settings change for debouncing
    g_lastSettingsChangeMs = millis();
    
    String normalizedParam = normalizeParamAlias(param);
    String lowerParam = normalizedParam;
    lowerParam.toLowerCase();

    if (lowerParam == "hostname" || lowerParam == "hn")
    {
        String hostname = value;
        hostname.trim();
        hostname.toLowerCase();
        if (hostname.length() == 0 || hostname.length() > MAX_HOSTNAME_LENGTH)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "hostname must be 1-63 characters");
            return false;
        }
        for (size_t i = 0; i < hostname.length(); ++i)
        {
            const char c = hostname.charAt(i);
            const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            if (!valid || ((i == 0 || i == hostname.length() - 1) && c == '-'))
            {
                setSettingsErrorWithCode(ERR_INVALID_VALUE, "hostname may contain lowercase letters, numbers, and interior hyphens");
                return false;
            }
        }
        if (String(appSettings.hostname) == hostname)
            return true;
        logSettingChange("hostname", String(appSettings.hostname), hostname);
        std::strncpy(appSettings.hostname, hostname.c_str(), MAX_HOSTNAME_LENGTH);
        appSettings.hostname[MAX_HOSTNAME_LENGTH] = '\0';
        return settings_save();
    }

    if (lowerParam == "mqtt.key" || lowerParam == "mk")
    {
        if (value.length() > kMaxMqttKeyLength)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "mqtt.key must be at most 127 characters");
            return false;
        }
        if (String(appSettings.mqttKey) == value)
            return true;
        logSettingChange("mqtt.key", appSettings.mqttKey[0] ? "[configured]" : "[empty]",
                         value.length() ? "[configured]" : "[empty]", true);
        std::strncpy(appSettings.mqttKey, value.c_str(), kMaxMqttKeyLength);
        appSettings.mqttKey[kMaxMqttKeyLength] = '\0';
        return settings_save();
    }

    size_t widx = 0;
    String wfield;
    if (parseWifiParam(lowerParam, widx, wfield))
    {
        if (widx >= kMaxWifiCredentials)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "wifi index out of range");
            return false;
        }
        if (wfield == "ssid")
        {
            String key = String("wifi[") + widx + "].ssid";
            String oldValue = appSettings.wifi[widx].ssid ? String(appSettings.wifi[widx].ssid) : String();
            if (oldValue == value)
            {
                return true;
            }
            logSettingChange(key.c_str(), oldValue, value);
            assignStringField(appSettings.wifi[widx].ssid, value.c_str());
            bool saved = settings_save();
            if (saved)
            {
                // Check if WiFi credentials were added and start tasks if needed
                system_checkAndStartTasksIfWiFiConfigured();
            }
            return saved;
        }
        if (wfield == "password")
        {
            String key = String("wifi[") + widx + "].password";
            String oldValue = appSettings.wifi[widx].password ? String(appSettings.wifi[widx].password) : String();
            if (oldValue == value)
            {
                return true;
            }
            logSettingChange(key.c_str(), oldValue, value, true);
            assignStringField(appSettings.wifi[widx].password, value.c_str());
            bool saved = settings_save();
            if (saved)
            {
                // Check if WiFi credentials were added and start tasks if needed
                system_checkAndStartTasksIfWiFiConfigured();
            }
            return saved;
        }
        if (wfield == "connecttimeoutms")
        {
            unsigned long newValue = static_cast<unsigned long>(value.toInt());
            unsigned long oldValue = appSettings.wifi[widx].connectTimeoutMs;
            if (oldValue == newValue)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].connectTimeoutMs";
            logSettingChange(key.c_str(), String(oldValue), String(newValue));
            appSettings.wifi[widx].connectTimeoutMs = newValue;
            return settings_save();
        }
        if (wfield == "staticip")
        {
            String trimmedVal = value;
            trimmedVal.trim();
            String oldValue = String(appSettings.wifi[widx].staticIp);
            if (oldValue == trimmedVal)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticIp";
            logSettingChange(key.c_str(), oldValue, trimmedVal);
            copyToCharBuffer(appSettings.wifi[widx].staticIp, sizeof(appSettings.wifi[widx].staticIp), trimmedVal);
            return settings_save();
        }
        if (wfield == "staticsubnet")
        {
            String trimmedVal = value;
            trimmedVal.trim();
            String oldValue = String(appSettings.wifi[widx].staticSubnet);
            if (oldValue == trimmedVal)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticSubnet";
            logSettingChange(key.c_str(), oldValue, trimmedVal);
            copyToCharBuffer(appSettings.wifi[widx].staticSubnet, sizeof(appSettings.wifi[widx].staticSubnet), trimmedVal);
            return settings_save();
        }
        if (wfield == "staticgateway")
        {
            String trimmedVal = value;
            trimmedVal.trim();
            String oldValue = String(appSettings.wifi[widx].staticGateway);
            if (oldValue == trimmedVal)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticGateway";
            logSettingChange(key.c_str(), oldValue, trimmedVal);
            copyToCharBuffer(appSettings.wifi[widx].staticGateway, sizeof(appSettings.wifi[widx].staticGateway), trimmedVal);
            return settings_save();
        }
        if (wfield == "staticdns1")
        {
            String trimmedVal = value;
            trimmedVal.trim();
            String oldValue = String(appSettings.wifi[widx].staticDns1);
            if (oldValue == trimmedVal)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticDns1";
            logSettingChange(key.c_str(), oldValue, trimmedVal);
            copyToCharBuffer(appSettings.wifi[widx].staticDns1, sizeof(appSettings.wifi[widx].staticDns1), trimmedVal);
            return settings_save();
        }
        if (wfield == "staticdns2")
        {
            String trimmedVal = value;
            trimmedVal.trim();
            String oldValue = String(appSettings.wifi[widx].staticDns2);
            if (oldValue == trimmedVal)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticDns2";
            logSettingChange(key.c_str(), oldValue, trimmedVal);
            copyToCharBuffer(appSettings.wifi[widx].staticDns2, sizeof(appSettings.wifi[widx].staticDns2), trimmedVal);
            return settings_save();
        }
        if (wfield == "staticipenabled")
        {
            String lowered = value;
            lowered.toLowerCase();
            bool newValue = (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on");
            bool oldValue = appSettings.wifi[widx].staticIpEnabled;
            if (oldValue == newValue)
            {
                return true;
            }
            String key = String("wifi[") + widx + "].staticIpEnabled";
            logSettingChange(key.c_str(), boolToString(oldValue), boolToString(newValue));
            appSettings.wifi[widx].staticIpEnabled = newValue;
            return settings_save();
        }
    }

    if (lowerParam == "audio.samplerate")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.sampleRate expects a number"); return false; }
        uint32_t newValue = sanitizeSampleRate(static_cast<uint32_t>(value.toInt()));
        if (appSettings.audio.sampleRate == newValue)
        {
            return true;
        }
        logSettingChange("audio.sampleRate", String(appSettings.audio.sampleRate), String(newValue));
        appSettings.audio.sampleRate = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.buffersamples")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.bufferSamples expects a number"); return false; }
        size_t newValue = sanitizeBufferSamples(value.toInt());
        if (appSettings.audio.bufferSamples == newValue)
        {
            return true;
        }
        logSettingChange("audio.bufferSamples",
                         String(static_cast<unsigned long>(appSettings.audio.bufferSamples)),
                         String(static_cast<unsigned long>(newValue)));
        appSettings.audio.bufferSamples = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.prerecordms")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.preRecordMs expects a number"); return false; }
        uint32_t newValue = sanitizePreRecordMs(value.toInt());
        if (appSettings.audio.preRecordMs == newValue)
        {
            return true;
        }

        logSettingChange("audio.preRecordMs",
                         String(appSettings.audio.preRecordMs),
                         String(newValue));
        appSettings.audio.preRecordMs = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.audiothreshold")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.audioThreshold expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 0 || raw > 60) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.audioThreshold must be 0-60"); return false; }        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.audio.audioThreshold == newValue)
        {
            return true;
        }
        logSettingChange("audio.audioThreshold",
                         String(static_cast<unsigned>(appSettings.audio.audioThreshold)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.audio.audioThreshold = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.minrecordingms")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.minRecordingMs expects a number"); return false; }
        uint32_t newValue = static_cast<uint32_t>(value.toInt());

        if (appSettings.audio.minRecordingMs == newValue)
        {
            return true;
        }

        if (newValue > appSettings.audio.maxRecordingMs)
        {
            LOG_DEBUG("The minimum can not be higher than the maximum recording time. We have set it to match the maximum.");
            newValue = std::min(newValue, static_cast<uint32_t>(appSettings.audio.maxRecordingMs));
        }

        if (newValue < DEFAULT_AUDIO_MIN_RECORDING_MS)
        {
            LOG_DEBUG("Minimum recording time can not be less than 1 second. It is been set to 1 second.");
            newValue = DEFAULT_AUDIO_MIN_RECORDING_MS;
        }

        logSettingChange("audio.minRecordingMs", String(appSettings.audio.minRecordingMs), String(newValue));
        appSettings.audio.minRecordingMs = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.maxrecordingms")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.maxRecordingMs expects a number"); return false; }
        uint32_t newValue = static_cast<uint32_t>(value.toInt());

        if (appSettings.audio.maxRecordingMs == newValue)
        {
            return true;
        }

        if (newValue < DEFAULT_AUDIO_MIN_RECORDING_MS)
        {
            LOG_DEBUG("Maximum recording time can not be lower than the minimum allowed. We have set it to the minimum.");
            newValue = (DEFAULT_AUDIO_MIN_RECORDING_MS);
        }

        if (appSettings.sdCard.useSdCard && appSettings.sdCard.recordToSdCard)
        {
            if (newValue > DEFAULT_AUDIO_MAX_SD_RECORDING_MS)
            {
                LOG_DEBUG("Maximum recording time cannot be longer than allowed. It has been set to the longest time allowed.");
                newValue = std::min(newValue, static_cast<uint32_t>(DEFAULT_AUDIO_MAX_SD_RECORDING_MS));
            }
        }
        else
        {
            if (newValue > DEFAULT_AUDIO_MAX_RECORDING_MS)
            {
                LOG_DEBUG("Maximum recording time cannot be longer than allowed. It has been set to the longest time allowed.");
                newValue = std::min(newValue, static_cast<uint32_t>(DEFAULT_AUDIO_MAX_RECORDING_MS));
            }
        }

        if (newValue < appSettings.audio.minRecordingMs)
        {
            LOG_DEBUG("Maximum recording time cannot be shorter than minimum recording time. It has been set to match the minimum.");
            newValue = std::max(newValue, static_cast<uint32_t>(appSettings.audio.minRecordingMs));
        }

        logSettingChange("audio.maxRecordingMs", String(appSettings.audio.maxRecordingMs), String(newValue));
        appSettings.audio.maxRecordingMs = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.silencethresholdms")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.silenceThresholdMs expects a number"); return false; }
        uint32_t newValue = static_cast<uint32_t>(value.toInt());
        if (newValue > DEFAULT_AUDIO_MAX_SILENCE_THRESHOLD_MS || newValue < DEFAULT_AUDIO_MIN_SILENCE_THRESHOLD_MS)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.silenceThresholdMs must be " + String(DEFAULT_AUDIO_MIN_SILENCE_THRESHOLD_MS) + "-" + String(DEFAULT_AUDIO_MAX_SILENCE_THRESHOLD_MS));
            return false;
        }
        newValue = sanitizeSilenceThresholdMs(newValue);
        if (appSettings.audio.silenceThresholdMs == newValue)
        {
            return true;
        }

        logSettingChange("audio.silenceThresholdMs", String(appSettings.audio.silenceThresholdMs), String(newValue));
        appSettings.audio.silenceThresholdMs = newValue;
        return settings_save();
    }
    if (lowerParam == "audio.codecgain")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.codecGain expects a number"); return false; }
        int gain = value.toInt();
        int8_t newValue = sanitizeCodecGainDb(gain);
        if (appSettings.audio.codecGainDb == newValue)
        {
            return true;
        }
        logSettingChange("audio.codecGain", String(appSettings.audio.codecGainDb), String(newValue));
        appSettings.audio.codecGainDb = newValue;
        updateCodecGainFromSettings();
        return settings_save();
    }
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
    if (lowerParam == "audio.recordinputchannel")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.recordInputChannel expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 0 || raw > 1)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.recordInputChannel must be 0 or 1");
            return false;
        }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.audio.recordInputChannel == newValue)
        {
            syncEffectiveRecordInputChannelFromAppSettings();
            return true;
        }
        logSettingChange("audio.recordInputChannel",
                         String(static_cast<unsigned>(appSettings.audio.recordInputChannel)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.audio.recordInputChannel = newValue;
        syncEffectiveRecordInputChannelFromAppSettings();
        return settings_save();
    }
#else
    if (lowerParam == "audio.recordinputchannel")
    {
        setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.recordInputChannel is only supported on TANGO firmware");
        return false;
    }
#endif

    if (lowerParam == "audio.speakerenabled")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.speakerEnabled == newValue)
        {
            return true;
        }
        logSettingChange("audio.speakerEnabled", boolToString(appSettings.speakerEnabled), boolToString(newValue));
        appSettings.speakerEnabled = newValue;
        return settings_save();
    }

    if (lowerParam == "audio.speakervolume")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.speakerVolume expects a number"); return false; }
        int raw = value.toInt();

        if (raw < 0 || raw > 100)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.speakerVolume must be 0-100");
            return false;
        }
        uint8_t newValue = sanitizeSpeakerVolume(raw);

        if (appSettings.speakerVolume == newValue)
        {
            return true;
        }

        logSettingChange("audio.speakerVolume",
                         String(static_cast<unsigned>(appSettings.speakerVolume)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.speakerVolume = newValue;
        return settings_save();
    }

    if (lowerParam == "audio.transmitenabled")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.transmitEnabled == newValue)
        {
            return true;
        }
        logSettingChange("audio.transmitEnabled", boolToString(appSettings.transmitEnabled), boolToString(newValue));
        appSettings.transmitEnabled = newValue;
        return settings_save();
    }

    if (lowerParam == "audio.transmitvolume")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "audio.transmitVolume expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 0 || raw > 100) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "audio.transmitVolume must be 0-100"); return false; }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.transmitVolume == newValue)
        {
            return true;
        }
        logSettingChange("audio.transmitVolume",
                         String(static_cast<unsigned>(appSettings.transmitVolume)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.transmitVolume = newValue;
        return settings_save();
    }

#if defined(ECHO)
    if (lowerParam == "repeater.enabled")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (newValue && !appSettings.transmitEnabled)
        {
            setSettingsErrorWithCode(ERR_INVALID_VALUE, "Enable TX first (audio.transmitEnabled) before enabling repeater mode");
            return false;
        }
        if (appSettings.repeaterEnabled == newValue)
        {
            return true;
        }
        logSettingChange("repeater.enabled", boolToString(appSettings.repeaterEnabled), boolToString(newValue));
        appSettings.repeaterEnabled = newValue;
        return settings_save();
    }

    if (lowerParam == "repeater.mode")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "repeater.mode expects 1 (simplex) or 2 (duplex)"); return false; }
        int raw = value.toInt();
        if (raw != 1 && raw != 2) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "repeater.mode must be 1 (simplex) or 2 (duplex)"); return false; }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.repeaterMode == newValue)
        {
            return true;
        }
        logSettingChange("repeater.mode",
                         String(static_cast<unsigned>(appSettings.repeaterMode)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.repeaterMode = newValue;
        return settings_save();
    }
#endif

    // CW settings (global). Note: setting does NOT auto-save; explicit save endpoint persists.
    if (lowerParam == "cw.wpm")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "cw.wpm expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 5 || raw > 40) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "cw.wpm must be 5-40"); return false; }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.cwWpm == newValue) return true;
        logSettingChange("cw.wpm", String(static_cast<unsigned>(appSettings.cwWpm)), String(static_cast<unsigned>(newValue)));
        appSettings.cwWpm = newValue;
        return true;
    }
    if (lowerParam == "cw.tonehz")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "cw.toneHz expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 200 || raw > 2000) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "cw.toneHz must be 200-2000"); return false; }
        uint16_t newValue = static_cast<uint16_t>(raw);
        if (appSettings.cwToneHz == newValue) return true;
        logSettingChange("cw.toneHz", String(static_cast<unsigned>(appSettings.cwToneHz)), String(static_cast<unsigned>(newValue)));
        appSettings.cwToneHz = newValue;
        return true;
    }
    if (lowerParam == "cw.volume")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "cw.volume expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 0 || raw > 100) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "cw.volume must be 0-100"); return false; }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.cwVolume == newValue) return true;
        logSettingChange("cw.volume", String(static_cast<unsigned>(appSettings.cwVolume)), String(static_cast<unsigned>(newValue)));
        appSettings.cwVolume = newValue;
        return true;
    }
    if (lowerParam == "cw.repeat")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "cw.repeat expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 1 || raw > 10) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "cw.repeat must be 1-10"); return false; }
        uint8_t newValue = static_cast<uint8_t>(raw);
        if (appSettings.cwRepeat == newValue) return true;
        logSettingChange("cw.repeat", String(static_cast<unsigned>(appSettings.cwRepeat)), String(static_cast<unsigned>(newValue)));
        appSettings.cwRepeat = newValue;
        return true;
    }

    // Timezone settings
    if (lowerParam == "timezone.offsethours")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "timezone.offsetHours expects a number"); return false; }
        int newValue = value.toInt();
        if (newValue < -12 || newValue > 14) { setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "timezone.offsetHours must be -12 to +14"); return false; }
        if (appSettings.timezone.offsetHours == newValue)
        {
            return true;
        }
        logSettingChange("timezone.offsetHours",
                         String(appSettings.timezone.offsetHours),
                         String(newValue));
        appSettings.timezone.offsetHours = static_cast<int8_t>(newValue);
        // Apply the new timezone immediately
        timeKeeper().applyTimezoneFromSettings();
        return settings_save();
    }

    if (lowerParam == "upload.queuedepth")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "upload.queueDepth expects a number"); return false; }
        uint8_t newValue = sanitizeQueueDepth(static_cast<unsigned long>(value.toInt()));
        if (appSettings.upload.queueDepth == newValue)
        {
            return true;
        }
        logSettingChange("upload.queueDepth", String(appSettings.upload.queueDepth), String(newValue));
        appSettings.upload.queueDepth = newValue;
        // queueDepth doesn't affect network connections, but invalidate to reset dead endpoints
        network_invalidateApiEndpoints();
        return settings_save();
    }
    if (lowerParam == "upload.converttomp3")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.upload.convertToMp3 == newValue)
        {
            return true;
        }
        logSettingChange("upload.convertToMp3",
                         boolToString(appSettings.upload.convertToMp3),
                         boolToString(newValue));
        appSettings.upload.convertToMp3 = newValue;
        // convertToMp3 doesn't affect network connections, but invalidate to reset dead endpoints
        network_invalidateApiEndpoints();
        return settings_save();
    }

    if (lowerParam == "rtc.enabled")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.rtc.enabled == newValue)
        {
            return true;
        }
        logSettingChange("rtc.enabled",
                         boolToString(appSettings.rtc.enabled),
                         boolToString(newValue));
        appSettings.rtc.enabled = newValue;
        return settings_save();
    }
    if (lowerParam == "rtc.sdapin")
    {
        uint8_t newValue = static_cast<uint8_t>(value.toInt());
        if (appSettings.rtc.sdaPin == newValue)
        {
            return true;
        }
        logSettingChange("rtc.sdaPin",
                         String(static_cast<unsigned>(appSettings.rtc.sdaPin)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.rtc.sdaPin = newValue;
        return settings_save();
    }
    if (lowerParam == "rtc.sclpin")
    {
        uint8_t newValue = static_cast<uint8_t>(value.toInt());
        if (appSettings.rtc.sclPin == newValue)
        {
            return true;
        }
        logSettingChange("rtc.sclPin",
                         String(static_cast<unsigned>(appSettings.rtc.sclPin)),
                         String(static_cast<unsigned>(newValue)));
        appSettings.rtc.sclPin = newValue;
        return settings_save();
    }

    if (lowerParam == "sdcard.usesdcard")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.sdCard.useSdCard == newValue)
        {
            return true;
        }
        logSettingChange("sdCard.useSdCard",
                         boolToString(appSettings.sdCard.useSdCard),
                         boolToString(newValue));
        appSettings.sdCard.useSdCard = newValue;
        // Re-evaluate storage mode when useSdCard changes
        storage_revaluateMode();
        return settings_save();
    }
    if (lowerParam == "sdcard.recordtosdcard")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.sdCard.recordToSdCard == newValue)
        {
            return true;
        }
        logSettingChange("sdCard.recordToSdCard",
                         boolToString(appSettings.sdCard.recordToSdCard),
                         boolToString(newValue));
        appSettings.sdCard.recordToSdCard = newValue;
        // Re-evaluate storage mode when recordToSdCard changes
        storage_revaluateMode();
        return settings_save();
    }
    if (lowerParam == "sdcard.mode1bit")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.sdCard.mode1bit == newValue)
        {
            return true;
        }
        logSettingChange("sdCard.mode1bit",
                         boolToString(appSettings.sdCard.mode1bit),
                         boolToString(newValue));
        appSettings.sdCard.mode1bit = newValue;
        return settings_save();
    }
    if (lowerParam == "sdcard.frequency")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "sdCard.frequency expects a number"); return false; }
        uint32_t newValue = static_cast<uint32_t>(value.toInt());
        if (newValue < 1000000 || newValue > 20000000)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "sdCard.frequency must be 1000000-20000000");
            return false;
        }
        if (appSettings.sdCard.frequency == newValue)
        {
            return true;
        }
        logSettingChange("sdCard.frequency",
                         String(appSettings.sdCard.frequency),
                         String(newValue));
        appSettings.sdCard.frequency = newValue;
        return settings_save();
    }
    if (lowerParam == "sdcard.formatifmountfailed")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.sdCard.formatIfMountFailed == newValue)
        {
            return true;
        }
        logSettingChange("sdCard.formatIfMountFailed",
                         boolToString(appSettings.sdCard.formatIfMountFailed),
                         boolToString(newValue));
        appSettings.sdCard.formatIfMountFailed = newValue;
        return settings_save();
    }

    size_t hostIndex = 0;
    if (parseUploadHostParam(lowerParam, hostIndex))
    {
        if (hostIndex >= kApiEndpointCount)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "host index out of range");
            return false;
        }
        String trimmed = value;
        trimmed.trim();
        String oldValue = String(appSettings.upload.apiHosts[hostIndex]);
        if (trimmed == oldValue)
        {
            return true;
        }
        String key = String("upload.apiHosts[") + hostIndex + "]";
        logSettingChange(key.c_str(), oldValue, trimmed);
        assignUploadHost(hostIndex, trimmed.c_str());
        network_invalidateApiEndpoints();
        return settings_save();
    }

    size_t endpointIndex = 0;
    if (parseUploadEndpointParam(lowerParam, "upload.apiports", endpointIndex))
    {
        if (endpointIndex >= kApiEndpointCount)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "port index out of range");
            return false;
        }
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "upload.apiPorts expects a number"); return false; }
        uint16_t newValue = static_cast<uint16_t>(value.toInt());
        if (appSettings.upload.apiPorts[endpointIndex] == newValue)
        {
            return true;
        }
        String key = String("upload.apiPorts[") + endpointIndex + "]";
        logSettingChange(key.c_str(), 
                         String(appSettings.upload.apiPorts[endpointIndex]),
                         String(newValue));
        appSettings.upload.apiPorts[endpointIndex] = newValue;
        network_invalidateApiEndpoints();
        return settings_save();
    }

    if (parseUploadEndpointParam(lowerParam, "upload.enabled", endpointIndex))
    {
        if (endpointIndex >= kApiEndpointCount)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "endpoint index out of range");
            return false;
        }
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.upload.enabled[endpointIndex] == newValue)
        {
            return true;
        }
        String key = String("upload.enabled[") + endpointIndex + "]";
        logSettingChange(key.c_str(),
                         boolToString(appSettings.upload.enabled[endpointIndex]),
                         boolToString(newValue));
        appSettings.upload.enabled[endpointIndex] = newValue;
        network_invalidateApiEndpoints();
        settings_clearDebounce();
        return settings_save();
    }

    // Custom = slot 3
    if (lowerParam == "upload.usecustomhost")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (appSettings.upload.enabled[3] == newValue)
        {
            return true;
        }
        logSettingChange("upload.enabled[3]", boolToString(appSettings.upload.enabled[3]), boolToString(newValue));
        appSettings.upload.enabled[3] = newValue;
        network_invalidateApiEndpoints();
        settings_clearDebounce();
        return settings_save();
    }
    if (lowerParam == "upload.customhost")
    {
        String trimmed = value;
        trimmed.trim();
        if (trimmed.length() > kMaxApiHostLength)
            trimmed = trimmed.substring(0, kMaxApiHostLength);
        if (String(appSettings.upload.apiHosts[3]) == trimmed)
        {
            return true;
        }
        logSettingChange("upload.apiHosts[3]", String(appSettings.upload.apiHosts[3]), trimmed);
        std::strncpy(appSettings.upload.apiHosts[3], trimmed.c_str(), kMaxApiHostLength);
        appSettings.upload.apiHosts[3][kMaxApiHostLength] = '\0';
        network_invalidateApiEndpoints();
        settings_clearDebounce();
        return settings_save();
    }
    if (lowerParam == "upload.customport")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "upload.customPort expects a number"); return false; }
        int raw = value.toInt();
        if (raw < 1 || raw > 65535)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "upload.customPort must be 1-65535");
            return false;
        }
        uint16_t newValue = static_cast<uint16_t>(raw);
        if (appSettings.upload.apiPorts[3] == newValue)
        {
            return true;
        }
        logSettingChange("upload.apiPorts[3]", String(appSettings.upload.apiPorts[3]), String(newValue));
        appSettings.upload.apiPorts[3] = newValue;
        network_invalidateApiEndpoints();
        settings_clearDebounce();
        return settings_save();
    }

    // WiFi TX Power (supports both "txpower" and "wtp" aliases)
    if (lowerParam == "txpower" || lowerParam == "wtp" || lowerParam == "wifitxpower")
    {
        if (!isValidInteger(value)) { setSettingsErrorWithCode(ERR_INVALID_VALUE, "wifiTxPower expects a number"); return false; }
        int newValue = value.toInt();
        if (newValue < 1 || newValue > 10)
        {
            setSettingsErrorWithCode(ERR_OUT_OF_RANGE, "wifiTxPower must be 1-10");
            return false;
        }
        uint8_t oldValue = appSettings.wifiTxPower;
        if (oldValue == static_cast<uint8_t>(newValue))
        {
            return true;
        }
        logSettingChange("wifiTxPower", String(oldValue), String(newValue));
        appSettings.wifiTxPower = static_cast<uint8_t>(newValue);
        
        // Apply TX power immediately if WiFi is connected
        if (WiFi.isConnected())
        {
            uint8_t esp32Value = mapWifiTxPowerLevel(appSettings.wifiTxPower);
            #ifdef ESP32
            esp_wifi_set_max_tx_power(esp32Value);
            #endif
        }
        
        return settings_save();
    }
    
    // Webserver Enabled (supports "webserverenabled", "webserver", "wse" aliases)
    if (lowerParam == "webserverenabled" || lowerParam == "webserver" || lowerParam == "wse")
    {
        String v = value;
        v.toLowerCase();
        bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
        bool oldValue = appSettings.webserverEnabled;
        if (oldValue == newValue)
        {
            return true;
        }
        logSettingChange("webserverEnabled", boolToString(oldValue), boolToString(newValue));
        appSettings.webserverEnabled = newValue;
        return settings_save();
    }

    setSettingsErrorWithCode(ERR_UNKNOWN_CMD, String("unknown parameter \"") + param + "\"");
    return false;
}

void settings_processSerial()
{
    if (!Serial)
        return;

    // Static buffer to accumulate characters until we get a complete line
    static String inputBuffer = "";
    constexpr size_t kMaxInputLength = 512; // Maximum command length
    
    // Static variable to track current directory for file management commands
    static String currentDirectory = "/";
    
    // Read all available characters
    while (Serial.available() > 0)
    {
        char c = Serial.read();
        
        // Handle newline - this means we have a complete command
        if (c == '\n' || c == '\r')
        {
            // If it's \r, check if next char is \n and consume it
            if (c == '\r' && Serial.available() > 0 && Serial.peek() == '\n')
            {
                Serial.read(); // Consume the \n
            }
            
            // Process the complete line
            inputBuffer.trim();
            if (inputBuffer.length() > 0)
            {
                String line = inputBuffer;
                inputBuffer = ""; // Clear buffer for next command
                
                // Acquire Serial mutex to prevent interleaving with periodic status messages
                bool mutexAcquired = acquireSerialMutex();
                
                // Helper macro to release mutex before break
                #define RELEASE_AND_BREAK() do { if (mutexAcquired) { releaseSerialMutex(); mutexAcquired = false; } break; } while(0)
                
                // Echo the command back to the user
                Serial.print("> ");
                Serial.println(line);
                esp_task_wdt_reset();

                // Split into tokens: CMD PARAM [VALUE]
                int firstSpace = line.indexOf(' ');
                String cmd = (firstSpace == -1) ? line : line.substring(0, firstSpace);
                cmd.trim();
                cmd.toUpperCase();
                
                // Process the command (continue with existing command processing logic below)
                // The rest of the function will handle the command
                // We'll process it inline here by continuing to the command processing section
                
                // Process commands (existing logic continues here)
                // Use Serial.println directly for CLI output to ensure it always appears
                if (cmd == "HELP" || cmd == "?")
                {
                    DynamicJsonDocument doc(2048);
                    JsonArray cmds = doc.createNestedArray("commands");
                    cmds.add("help"); cmds.add("show <setting>"); cmds.add("set <setting> <val>");
                    cmds.add("save"); cmds.add("load"); cmds.add("export"); cmds.add("import <json>");
                    cmds.add("config <json>"); cmds.add("pushsettings"); cmds.add("pullsettings");
                    cmds.add("autoconfig"); cmds.add("reboot"); cmds.add("factoryreset");
                    cmds.add("maintenance"); cmds.add("makeindex"); cmds.add("pushlogs");
                    cmds.add("format"); cmds.add("time"); cmds.add("status"); cmds.add("status_short");
                    cmds.add("summary"); cmds.add("ip");
                    cmds.add("updatesounds");
                    cmds.add("webserver [on/off]"); cmds.add("txpower [1-10]"); cmds.add("mac"); cmds.add("config");
                    cmds.add("recordings"); cmds.add("audiolevel"); cmds.add("errors");
                    cmds.add("settime <time>"); cmds.add("reconnect"); cmds.add("recover");
                    cmds.add("sample"); cmds.add("health ?"); cmds.add("config ?");
                    cmds.add("debug <true|false>"); cmds.add("cd <dir>"); cmds.add("dir");
                    cmds.add("rm <file>"); cmds.add("rm *");
                    JsonArray shortcuts = doc.createNestedArray("shortcuts");
                    shortcuts.add("min/max/silence/gain/athr -> audio fields");
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
                    shortcuts.add("inchan/inputchan/recch -> audio.recordInputChannel (0 or 1 codec idx; web labels Right=0 Left=1; TANGO only)");
#endif
                    shortcuts.add("pre -> audio.preRecordMs");
                    shortcuts.add("hostN -> upload.apiHosts[N]");
                    shortcuts.add("upload.enabled[0|1|2] -> regions Ohio|Oregon|Virginia (true/false)");
                    shortcuts.add("region0|ohio, region1|oregon, region2|virginia -> upload.enabled[0|1|2]");
                    shortcuts.add("upload.useCustomHost, upload.customHost, upload.customPort -> custom upload server");
                    shortcuts.add("customhost, customport, usecustomhost -> upload custom host params");
                    cliJsonResponse(doc);
                }
                else if (cmd == "UPDATESOUNDS" || cmd == "UPDATE_SOUNDS" || cmd == "SOUNDS" || cmd == "SOUNDSUPDATE")
                {
                    // Force a CDN refresh of sound assets (boot.wav) into /system/sounds/.
                    // Requires SYSTEM_ASSETS_CDN_HOST build flag + SD card mounted + WiFi connected.
                    bool ok = system_assets_updateSoundsNow();
                    if (ok) cliOk("updatesounds", "ok"); else cliError(ERR_HW_ERROR, "updatesounds failed (check WiFi, SD, and CDN flags)");
                }
                else if (cmd == "GET" || cmd == "SHOW" || cmd == "READ")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    if (rest.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "usage: show <setting>");
                        RELEASE_AND_BREAK();
                    }
                    String val = settings_getParam(rest);
                    cliOk(rest, val);
                }
                else if (cmd == "SET" || cmd == "CHANGE" || cmd == "UPDATE")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    int secondSpace = rest.indexOf(' ');
                    if (secondSpace == -1)
                    {
                        cliError(ERR_MISSING_PARAM, "usage: set <setting> <value>");
                        RELEASE_AND_BREAK();
                    }
                    String param = rest.substring(0, secondSpace);
                    String value = rest.substring(secondSpace + 1);
                    param.trim();
                    value.trim();
                    if (param.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "usage: set <setting> <value>");
                        RELEASE_AND_BREAK();
                    }
                    bool ok = settings_setParam(param, value);
                    if (ok)
                    {
                        String accepted = settings_getParam(param);
                        cliOk(param, accepted);
                    }
                    else
                    {
                        const char* code = g_lastSettingsErrorCode.length() > 0 ? g_lastSettingsErrorCode.c_str() : ERR_INVALID_VALUE;
                        String msg = g_lastSettingsError.length() > 0 ? g_lastSettingsError : String("failed to set ") + param;
                        cliError(code, msg);
                    }
                }
                else if (cmd == "SAVE" || cmd == "STORE")
                {
                    bool ok = settings_save();
                    if (ok) cliOk(); else cliError(ERR_HW_ERROR, "failed to save settings");
                }
                else if (cmd == "RELOAD" || cmd == "LOAD" || cmd == "REFRESH")
                {
                    bool ok = settings_reload();
                    if (ok) cliOk(); else cliError(ERR_HW_ERROR, "failed to reload settings");
                }
                else if (cmd == "READCONFIG" || cmd == "READCONFIGURATIONS" || cmd == "EXPORT" || cmd == "DUMP" || cmd == "DUMPCONFIG")
                {
                    settings_reload();
                    String json = settings_getAllJson();
                    if (json.length() == 0) json = "{}";
                    DynamicJsonDocument wrapper(4096);
                    wrapper["status"] = "ok";
                    DynamicJsonDocument inner(4096);
                    deserializeJson(inner, json);
                    wrapper["data"] = inner.as<JsonObject>();
                    String output;
                    serializeJson(wrapper, output);
                    output += "\n";
                    Serial.write(reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
                    Serial.flush();
                }
                else if (cmd == "WRITECONFIG" || cmd == "WRITECONFIGURATIONS" || cmd == "IMPORT" || cmd == "APPLY")
                {
                    String payload = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    payload.trim();

                    if (payload.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "missing JSON payload");
                        RELEASE_AND_BREAK();
                    }

                    int jsonStart = payload.indexOf('{');
                    if (jsonStart > 0)
                    {
                        payload = payload.substring(jsonStart);
                        payload.trim();
                    }

                    int jsonEnd = payload.lastIndexOf('}');
                    if (jsonStart == -1 || jsonEnd == -1)
                    {
                        cliError(ERR_INVALID_VALUE, "invalid JSON payload");
                        RELEASE_AND_BREAK();
                    }

                    if (jsonEnd + 1 < static_cast<int>(payload.length()))
                    {
                        payload = payload.substring(0, jsonEnd + 1);
                    }

                    bool ok = settings_updateAllFromJson(payload, false);
                    if (ok)
                    {
                        cliOk();
                    }
                    else
                    {
                        String err = settings_getLastError();
                        cliError(ERR_INVALID_VALUE, err.length() > 0 ? err : "failed to apply settings");
                    }
                }
                else if (cmd == "CONFIG")
                {
                    String payload = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    payload.trim();

                    if (payload == "?")
                    {
                        sendConfigMessage();
                        cliOk();
                        break;
                    }

                    if (payload.length() == 0)
                    {
                        DynamicJsonDocument snap(1024);
                        snap["host"] = String(appSettings.upload.apiHosts[0] ? appSettings.upload.apiHosts[0] : "");
                        snap["port"] = appSettings.upload.apiPorts[0];
                        snap["mac"] = getDeviceId();
                        snap["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("null");
                        snap["minRecordingMs"] = appSettings.audio.minRecordingMs;
                        snap["maxRecordingMs"] = appSettings.audio.maxRecordingMs;
                        snap["silenceThresholdMs"] = appSettings.audio.silenceThresholdMs;
                        snap["audioThreshold"] = appSettings.audio.audioThreshold;
                        snap["codecGainDb"] = appSettings.audio.codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
                        snap["recordInputChannel"] = appSettings.audio.recordInputChannel;
#endif
                        bool sdAvailable = ensureStorage() && isStorageModeSdCard();
                        if (sdAvailable)
                        {
                            uint64_t sdTotal = SD_MMC.totalBytes();
                            uint64_t sdUsed = SD_MMC.usedBytes();
                            snap["sdTotalBytes"] = static_cast<int64_t>(sdTotal);
                            snap["sdFreeBytes"] = static_cast<int64_t>((sdTotal > sdUsed) ? (sdTotal - sdUsed) : 0);
                        }
                        cliJsonResponse(snap);
                        break;
                    }

                    int jsonStart = payload.indexOf('{');
                    if (jsonStart > 0)
                    {
                        payload = payload.substring(jsonStart);
                        payload.trim();
                    }

                    int jsonEnd = payload.lastIndexOf('}');
                    if (jsonStart == -1 || jsonEnd == -1)
                    {
                        cliError(ERR_INVALID_VALUE, "invalid JSON payload");
                        RELEASE_AND_BREAK();
                    }

                    if (jsonEnd + 1 < static_cast<int>(payload.length()))
                    {
                        payload = payload.substring(0, jsonEnd + 1);
                    }

                    startTrackingConfigChanges();
                    bool ok = settings_updateAllFromJson(payload, false);
                    stopTrackingConfigChanges();
                    
                    if (!ok)
                    {
                        String err = settings_getLastError();
                        cliError(ERR_INVALID_VALUE, err.length() > 0 ? err : "failed to apply settings");
                        break;
                    }
                    
                    // Build JSON response with changes
                    DynamicJsonDocument doc(2048);
                    doc["status"] = "ok";
                    doc["action"] = "config_applied";
                    doc["reboot_pending"] = true;
                    JsonArray changes = doc.createNestedArray("changes");
                    for (size_t i = 0; i < g_configChanges.size(); ++i)
                    {
                        const SettingChange& change = g_configChanges[i];
                        JsonObject c = changes.createNestedObject();
                        c["key"] = change.key;
                        if (change.sensitive)
                        {
                            c["old"] = "(hidden)";
                            c["new"] = "(hidden)";
                        }
                        else
                        {
                            c["old"] = change.oldValue;
                            c["new"] = change.newValue;
                        }
                    }
                    String output;
                    serializeJson(doc, output);
                    Serial.println(output);
                    
                    // Wait for DONE input
                    String doneBuffer = "";
                    while (true)
                    {
                        while (Serial.available() > 0)
                        {
                            char c = Serial.read();
                            
                            if (c == '\n' || c == '\r')
                            {
                                if (c == '\r' && Serial.available() > 0 && Serial.peek() == '\n')
                                {
                                    Serial.read();
                                }
                                
                                doneBuffer.trim();
                                if (doneBuffer.length() > 0)
                                {
                                    String checkInput = doneBuffer;
                                    checkInput.trim();
                                    checkInput.toUpperCase();
                                    
                                    if (checkInput == "DONE")
                                    {
                                        cliOk();
                                        delay(500);
                                        system_rebootFromCli();
                                        return;
                                    }
                                    doneBuffer = "";
                                }
                            }
                            else if (c == '\b' || c == 127)
                            {
                                if (doneBuffer.length() > 0) doneBuffer.remove(doneBuffer.length() - 1);
                            }
                            else if (c >= 32 || c == '\t')
                            {
                                if (doneBuffer.length() < 512) doneBuffer += c;
                            }
                        }
                        delay(100);
                    }
                }
                else if (cmd == "AUTOCONFIG")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    // Count commas to determine if this is short version (SSID,PASSWORD) or full version
                    int comma1 = rest.indexOf(',');
                    if (comma1 == -1)
                    {
                        cliError(ERR_MISSING_PARAM, "usage: AUTOCONFIG SSID,PASSWORD[,HOST_IP,PORT,TZ,THRESHOLD,MIN_MS,MAX_MS,PRE_MS]");
                        RELEASE_AND_BREAK();
                    }
                    
                    int comma2 = rest.indexOf(',', comma1 + 1);
                    bool isShortVersion = (comma2 == -1);
                    
                    String ssid = rest.substring(0, comma1);
                    String password = rest.substring(comma1 + 1, (isShortVersion ? rest.length() : comma2));
                    ssid.trim();
                    password.trim();
                    
                    if (ssid.length() == 0 || password.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "SSID and password must be non-empty");
                        RELEASE_AND_BREAK();
                    }
                    
                    String oldSsid = appSettings.wifi[0].ssid ? String(appSettings.wifi[0].ssid) : String();
                    if (oldSsid != ssid)
                    {
                        logSettingChange("wifi[0].ssid", oldSsid, ssid);
                        assignStringField(appSettings.wifi[0].ssid, ssid.c_str());
                    }
                    
                    String oldPassword = appSettings.wifi[0].password ? String(appSettings.wifi[0].password) : String();
                    if (oldPassword != password)
                    {
                        logSettingChange("wifi[0].password", oldPassword, password, true);
                        assignStringField(appSettings.wifi[0].password, password.c_str());
                    }
                    
                    if (isShortVersion)
                    {
                        settings_clearDebounce();
                        
                        bool saved = settings_save();
                        if (saved) system_checkAndStartTasksIfWiFiConfigured();
                        
                        if (!saved)
                        {
                            String err = settings_getLastError();
                            cliError(ERR_HW_ERROR, err.length() > 0 ? err : "failed to save settings");
                            RELEASE_AND_BREAK();
                        }
                        
                        DynamicJsonDocument doc(512);
                        doc["status"] = "ok";
                        doc["action"] = "autoconfig";
                        doc["reboot_pending"] = true;
                        doc["wifi_ssid"] = ssid;
                        String out;
                        serializeJson(doc, out);
                        Serial.println(out);
                        
                        while (true)
                        {
                            if (Serial.available() > 0)
                            {
                                String doneCmd = Serial.readStringUntil('\n');
                                doneCmd.trim();
                                doneCmd.toUpperCase();
                                if (doneCmd == "DONE")
                                {
                                    delay(500);
                                    ESP.restart();
                                }
                            }
                            delay(100);
                        }
                    }
                    
                    // Full version: parse all remaining parameters
                    int comma3 = (comma2 == -1) ? -1 : rest.indexOf(',', comma2 + 1);
                    int comma4 = (comma3 == -1) ? -1 : rest.indexOf(',', comma3 + 1);
                    int comma5 = (comma4 == -1) ? -1 : rest.indexOf(',', comma4 + 1);
                    int comma6 = (comma5 == -1) ? -1 : rest.indexOf(',', comma5 + 1);
                    int comma7 = (comma6 == -1) ? -1 : rest.indexOf(',', comma6 + 1);
                    int comma8 = (comma7 == -1) ? -1 : rest.indexOf(',', comma7 + 1);
                    
                    if (comma2 == -1 || comma3 == -1 || comma4 == -1 || comma5 == -1 || 
                        comma6 == -1 || comma7 == -1 || comma8 == -1)
                    {
                        cliError(ERR_MISSING_PARAM, "full autoconfig requires 9 comma-separated values");
                        RELEASE_AND_BREAK();
                    }
                    
                    String hostIp = rest.substring(comma2 + 1, comma3);
                    String hostPort = rest.substring(comma3 + 1, comma4);
                    String timezoneOffset = rest.substring(comma4 + 1, comma5);
                    String audioThreshold = rest.substring(comma5 + 1, comma6);
                    String minRecordingMs = rest.substring(comma6 + 1, comma7);
                    String maxRecordingMs = rest.substring(comma7 + 1, comma8);
                    String preRecordingMs = rest.substring(comma8 + 1);
                    
                    hostIp.trim();
                    hostPort.trim();
                    timezoneOffset.trim();
                    audioThreshold.trim();
                    minRecordingMs.trim();
                    maxRecordingMs.trim();
                    preRecordingMs.trim();
                    
                    if (hostIp.length() == 0 || hostPort.length() == 0 || timezoneOffset.length() == 0 || 
                        audioThreshold.length() == 0 || minRecordingMs.length() == 0 || 
                        maxRecordingMs.length() == 0 || preRecordingMs.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "all parameters must be non-empty");
                        RELEASE_AND_BREAK();
                    }
                    
                    int port = hostPort.toInt();
                    if (port <= 0 || port > 65535)
                    {
                        cliError(ERR_OUT_OF_RANGE, "port must be 1-65535");
                        RELEASE_AND_BREAK();
                    }
                    
                    int tzOffset = timezoneOffset.toInt();
                    if (tzOffset < -12 || tzOffset > 14)
                    {
                        cliError(ERR_OUT_OF_RANGE, "timezone offset must be -12 to +14");
                        RELEASE_AND_BREAK();
                    }
                    
                    int audioThresh = audioThreshold.toInt();
                    if (audioThresh < 0 || audioThresh > 60)
                    {
                        cliError(ERR_OUT_OF_RANGE, "audio threshold must be 0-60");
                        RELEASE_AND_BREAK();
                    }
                    
                    uint32_t minRecMs = static_cast<uint32_t>(minRecordingMs.toInt());
                    if (minRecMs == 0)
                    {
                        cliError(ERR_OUT_OF_RANGE, "min recording duration must be > 0");
                        RELEASE_AND_BREAK();
                    }
                    
                    uint32_t maxRecMs = static_cast<uint32_t>(maxRecordingMs.toInt());
                    if (maxRecMs == 0)
                    {
                        cliError(ERR_OUT_OF_RANGE, "max recording duration must be > 0");
                        RELEASE_AND_BREAK();
                    }
                    if (maxRecMs < minRecMs)
                    {
                        cliError(ERR_OUT_OF_RANGE, "max recording must be >= min recording");
                        RELEASE_AND_BREAK();
                    }
                    
                    long preRecMs = preRecordingMs.toInt();
                    if (preRecMs < 0)
                    {
                        cliError(ERR_OUT_OF_RANGE, "pre-recording duration must be >= 0");
                        RELEASE_AND_BREAK();
                    }
                    
                    // Set API Host
                    String oldHost = String(appSettings.upload.apiHosts[0]);
                    if (oldHost != hostIp)
                    {
                        logSettingChange("upload.apiHosts[0]", oldHost, hostIp);
                        assignUploadHost(0, hostIp.c_str());
                    }
                    
                    // Set API Port
                    uint16_t portValue = static_cast<uint16_t>(port);
                    if (appSettings.upload.apiPorts[0] != portValue)
                    {
                        logSettingChange("upload.apiPorts[0]", 
                                         String(appSettings.upload.apiPorts[0]),
                                         String(portValue));
                        appSettings.upload.apiPorts[0] = portValue;
                    }
                    
                    // Set Timezone Offset
                    int8_t tzOffsetValue = static_cast<int8_t>(tzOffset);
                    if (appSettings.timezone.offsetHours != tzOffsetValue)
                    {
                        logSettingChange("timezone.offsetHours",
                                         String(appSettings.timezone.offsetHours),
                                         String(tzOffsetValue));
                        appSettings.timezone.offsetHours = tzOffsetValue;
                    }
                    
                    // Set Audio Threshold
                    uint8_t audioThreshValue = static_cast<uint8_t>(audioThresh);
                    if (appSettings.audio.audioThreshold != audioThreshValue)
                    {
                        logSettingChange("audio.audioThreshold",
                                         String(static_cast<unsigned>(appSettings.audio.audioThreshold)),
                                         String(static_cast<unsigned>(audioThreshValue)));
                        appSettings.audio.audioThreshold = audioThreshValue;
                    }
                    
                    // Set Min Recording Duration
                    if (appSettings.audio.minRecordingMs != minRecMs)
                    {
                        logSettingChange("audio.minRecordingMs",
                                         String(appSettings.audio.minRecordingMs),
                                         String(minRecMs));
                        appSettings.audio.minRecordingMs = minRecMs;
                    }
                    
                    // Set Max Recording Duration
                    if (appSettings.audio.maxRecordingMs != maxRecMs)
                    {
                        logSettingChange("audio.maxRecordingMs",
                                         String(appSettings.audio.maxRecordingMs),
                                         String(maxRecMs));
                        appSettings.audio.maxRecordingMs = maxRecMs;
                    }
                    
                    // Set Pre-Recording Duration (sanitize to 0-500)
                    uint32_t preRecMsValue = sanitizePreRecordMs(preRecMs);
                    if (appSettings.audio.preRecordMs != preRecMsValue)
                    {
                        logSettingChange("audio.preRecordMs",
                                         String(appSettings.audio.preRecordMs),
                                         String(preRecMsValue));
                        appSettings.audio.preRecordMs = preRecMsValue;
                    }
                    
                    // Enable first endpoint
                    if (!appSettings.upload.enabled[0])
                    {
                        logSettingChange("upload.enabled[0]",
                                         boolToString(appSettings.upload.enabled[0]),
                                         "true");
                        appSettings.upload.enabled[0] = true;
                    }
                    
                    // Disable second endpoint
                    if (appSettings.upload.enabled[1])
                    {
                        logSettingChange("upload.enabled[1]",
                                         boolToString(appSettings.upload.enabled[1]),
                                         "false");
                        appSettings.upload.enabled[1] = false;
                    }
                    
                    // Disable third endpoint
                    if (appSettings.upload.enabled[2])
                    {
                        logSettingChange("upload.enabled[2]",
                                         boolToString(appSettings.upload.enabled[2]),
                                         "false");
                        appSettings.upload.enabled[2] = false;
                    }
                    
                    // Invalidate API endpoints to ensure new settings are used
                    network_invalidateApiEndpoints();
                    
                    // Save all settings once at the end (synchronous save)
                    // Clear debounce state to ensure immediate save
                    settings_clearDebounce();
                    
                    if (!settings_save())
                    {
                        String err = settings_getLastError();
                        cliError(ERR_HW_ERROR, err.length() > 0 ? err : "failed to save settings");
                        RELEASE_AND_BREAK();
                    }
                    
                    {
                        DynamicJsonDocument doc(1024);
                        doc["status"] = "ok";
                        doc["action"] = "autoconfig";
                        doc["reboot_pending"] = true;
                        JsonObject params = doc.createNestedObject("settings");
                        params["wifi_ssid"] = ssid;
                        params["upload_host"] = hostIp;
                        params["upload_port"] = port;
                        params["timezone_offset"] = tzOffset;
                        params["audio_threshold"] = audioThresh;
                        params["min_recording_ms"] = minRecMs;
                        params["max_recording_ms"] = maxRecMs;
                        params["pre_record_ms"] = preRecMsValue;
                        String out;
                        serializeJson(doc, out);
                        Serial.println(out);
                    }
                    
                    // Flush serial output
                    Serial.flush();
                    logger_flush();
                    
                    // Wait 2 seconds, checking for early DONE input
                    unsigned long rebootStartMs = millis();
                    const unsigned long rebootDelayMs = 2000;
                    String doneBuffer = "";
                    
                    while ((millis() - rebootStartMs) < rebootDelayMs)
                    {
                        // Check for DONE input (allows early reboot)
                        while (Serial.available() > 0)
                        {
                            char c = Serial.read();
                            
                            if (c == '\n' || c == '\r')
                            {
                                if (c == '\r' && Serial.available() > 0 && Serial.peek() == '\n')
                                {
                                    Serial.read(); // Consume the \n
                                }
                                
                                doneBuffer.trim();
                                if (doneBuffer.length() > 0)
                                {
                                    String checkInput = doneBuffer;
                                    checkInput.trim();
                                    checkInput.toUpperCase();
                                    
                                    if (checkInput == "DONE")
                                    {
                                        Serial.flush();
                                        logger_flush();
                                        delay(100);
                                        system_rebootFromCli();
                                        return;
                                    }
                                    doneBuffer = "";
                                }
                            }
                            else if (c == '\b' || c == 127)
                            {
                                if (doneBuffer.length() > 0)
                                {
                                    doneBuffer.remove(doneBuffer.length() - 1);
                                }
                            }
                            else if (c >= 32 || c == '\t')
                            {
                                if (doneBuffer.length() < 512)
                                {
                                    doneBuffer += c;
                                }
                            }
                        }
                        delay(50); // Small delay to avoid busy-waiting
                    }
                    
                    Serial.flush();
                    logger_flush();
                    delay(100);
                    system_rebootFromCli();
                    return; // This will reboot
                }
                else if (cmd == "TXPOWER" || cmd == "TX_POWER")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest.length() == 0)
                    {
                        DynamicJsonDocument doc(256);
                        doc["wifiTxPower"] = static_cast<unsigned>(appSettings.wifiTxPower);
                        doc["esp32Value"] = static_cast<unsigned>(mapWifiTxPowerLevel(appSettings.wifiTxPower));
                        cliJsonResponse(doc);
                        break;
                    }
                    
                    int value = rest.toInt();
                    if (value < 1 || value > 10)
                    {
                        cliError(ERR_OUT_OF_RANGE, "TX power must be 1-10");
                        break;
                    }
                    
                    uint8_t oldValue = appSettings.wifiTxPower;
                    appSettings.wifiTxPower = static_cast<uint8_t>(value);
                    
                    if (settings_save())
                    {
                        if (WiFi.isConnected())
                        {
                            uint8_t esp32Value = mapWifiTxPowerLevel(appSettings.wifiTxPower);
                            #ifdef ESP32
                            esp_wifi_set_max_tx_power(esp32Value);
                            #endif
                        }
                        cliOk("wifiTxPower", String(appSettings.wifiTxPower));
                        system_notifySettingsChanged();
                    }
                    else
                    {
                        appSettings.wifiTxPower = oldValue;
                        cliError(ERR_HW_ERROR, "failed to save settings");
                    }
                }
                else if (cmd == "WEBSERVER" || cmd == "WEB_SERVER")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest.length() == 0)
                    {
                        cliOk("webserverEnabled", appSettings.webserverEnabled ? "true" : "false");
                        break;
                    }
                    
                    String v = rest;
                    v.toLowerCase();
                    bool newValue = (v == "1" || v == "true" || v == "yes" || v == "on");
                    bool oldValue = appSettings.webserverEnabled;
                    
                    if (oldValue == newValue)
                    {
                        cliOk("webserverEnabled", newValue ? "true" : "false");
                        break;
                    }
                    
                    appSettings.webserverEnabled = newValue;
                    
                    if (settings_save())
                    {
                        cliOk("webserverEnabled", newValue ? "true" : "false");
                        system_notifySettingsChanged();
                    }
                    else
                    {
                        appSettings.webserverEnabled = oldValue;
                        cliError(ERR_HW_ERROR, "failed to save settings");
                    }
                }
                else if (cmd == "SETTIME" || cmd == "SET_TIME")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest.length() == 0)
                    {
                        cliError(ERR_MISSING_PARAM, "usage: settime <YYYY-MM-DDTHH:MM:SSZ> or <epoch_seconds>");
                        break;
                    }
                    
                    time_t epochSeconds = 0;
                    long microseconds = 0;
                    bool parsed = false;
                    
                    if (parseIsoTimestampToEpoch(rest, epochSeconds, microseconds))
                    {
                        parsed = true;
                    }
                    else
                    {
                        char* endPtr = nullptr;
                        long long epochValue = strtoll(rest.c_str(), &endPtr, 10);
                        if (endPtr != nullptr && *endPtr == '\0' && epochValue > 0)
                        {
                            epochSeconds = static_cast<time_t>(epochValue);
                            microseconds = 0;
                            parsed = true;
                        }
                    }
                    
                    if (!parsed)
                    {
                        cliError(ERR_INVALID_VALUE, "invalid time format, use ISO (YYYY-MM-DDTHH:MM:SSZ) or epoch seconds");
                        break;
                    }
                    
                    if (!isEpochValid(epochSeconds))
                    {
                        cliError(ERR_OUT_OF_RANGE, "epoch time must be >= 2021-01-01");
                        break;
                    }
                    
                    if (timeKeeper().syncFromEventTimestamp(epochSeconds, microseconds))
                    {
                        struct tm timeinfo;
                        gmtime_r(&epochSeconds, &timeinfo);
                        char iso[32];
                        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
                        cliOk("time", String(iso));
                    }
                    else
                    {
                        cliError(ERR_HW_ERROR, "could not set system time");
                    }
                }
                else if (cmd == "REBOOT" || cmd == "RESET" || cmd == "RESTART")
                {
                    cliOk();
                    system_rebootFromCli();
                    return;
                }
                else if (cmd == "FACTORYRESET" || cmd == "FACTORY" || cmd == "FACTORY_RESET")
                {
                    if (!settings_factoryReset(true))
                    {
                        cliError(ERR_HW_ERROR, "cannot open NVS settings namespace");
                        break;
                    }
                    return;
                }
                else if (cmd == "MAINTENANCE" || cmd == "MAINT")
                {
                    String maskedJson = settings_getMaskedJsonForServer();
                    bool ok = network_pushSettingsToServer(maskedJson);
                    if (ok) cliOk(); else cliError(ERR_HW_ERROR, "settings push to server failed");
                }
                else if (cmd == "PUSHSETTINGS")
                {
                    String maskedJson = settings_getMaskedJsonForServer();
                    bool ok = network_pushSettingsToServer(maskedJson);
                    if (ok) cliOk(); else cliError(ERR_HW_ERROR, "push settings failed");
                }
                else if (cmd == "PULLSETTINGS")
                {
                    String serverJson;
                    if (!network_pullSettingsFromServer(serverJson))
                    {
                        cliError(ERR_HW_ERROR, "network error pulling settings");
                    }
                    else if (serverJson.length() == 0)
                    {
                        cliError(ERR_HW_ERROR, "empty response from server");
                    }
                    else
                    {
                        bool ok = settings_applyJsonFromServer(serverJson);
                        if (ok)
                        {
                            cliOk();
                        }
                        else
                        {
                            String err = settings_getLastError();
                            cliError(ERR_HW_ERROR, err.length() > 0 ? err : "failed to apply server settings");
                        }
                    }
                }
                else if (cmd == "SUMMARY" || cmd == "UPDATESUMMARY")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    if (!timeKeeper().timeIsValid()) { cliError(ERR_HW_ERROR, "time not synced"); break; }
                    
                    time_t now = time(nullptr);
                    struct tm* timeInfo = localtime(&now);
                    int year = timeInfo->tm_year + 1900;
                    int month = timeInfo->tm_mon + 1;
                    
                    bool monthOk = storage_updateMonthlySummary(year, month);
                    bool yearOk = storage_updateYearlySummary(year);
                    
                    DynamicJsonDocument doc(256);
                    doc["monthly"] = monthOk;
                    doc["yearly"] = yearOk;
                    cliJsonResponse(doc);
                }
                else if (cmd == "MAKEINDEX" || cmd == "MAKE-INDEX")
                {
                    cliOk();
                }
                else if (cmd == "PUSHLOGS" || cmd == "UPLOADLOGS" || cmd == "PUSHLOG" || cmd == "UPLOADLOG")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    
                    String logPath = getLatestLogPath();
                    if (logPath.isEmpty()) { cliError(ERR_HW_ERROR, "time not synced, cannot determine log path"); break; }
                    if (!SD_MMC.exists(logPath)) { cliError(ERR_HW_ERROR, String("log file not found: ") + logPath); break; }
                    
                    if (uploadLogFile(logPath))
                    {
                        cliOk("path", logPath);
                    }
                    else
                    {
                        cliError(ERR_HW_ERROR, "could not upload log file");
                    }
                }
                else if (cmd == "FORMAT" || cmd == "FORMATSD" || cmd == "FORMATSDCARD")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    File root = SD_MMC.open("/");
                    if (!root || !root.isDirectory())
                    {
                        if (root) root.close();
                        cliError(ERR_HW_ERROR, "cannot open root directory");
                        break;
                    }
                    
                    File file = root.openNextFile();
                    while (file)
                    {
                        String fileName = String(file.name());
                        String filePath = fileName;
                        if (file.isDirectory())
                        {
                            if (fileName != "." && fileName != "..")
                            {
                                File subDir = SD_MMC.open(filePath);
                                if (subDir && subDir.isDirectory())
                                {
                                    File subFile = subDir.openNextFile();
                                    while (subFile)
                                    {
                                        String subPath = String(subFile.name());
                                        if (subFile.isDirectory()) { subFile.close(); SD_MMC.rmdir(subPath); }
                                        else { subFile.close(); SD_MMC.remove(subPath); }
                                        subFile = subDir.openNextFile();
                                    }
                                    subDir.close();
                                }
                                SD_MMC.rmdir(filePath);
                            }
                        }
                        else
                        {
                            SD_MMC.remove(filePath);
                        }
                        file.close();
                        file = root.openNextFile();
                        yield();
                    }
                    root.close();
                    
                    SD_MMC.end();
                    delay(500);
                    const bool mode1bit = appSettings.sdCard.mode1bit;
                    const uint32_t frequency = appSettings.sdCard.frequency;
                    if (SD_MMC.begin("/sdcard", mode1bit, false, frequency, SD_MMC_MAX_OPEN_FILES))
                    {
                        cliOk();
                    }
                    else
                    {
                        cliError(ERR_HW_ERROR, "could not remount SD card");
                    }
                }
                else if (cmd == "TIME" || cmd == "CLOCK")
                {
                    struct timeval now;
                    gettimeofday(&now, nullptr);
                    time_t seconds = now.tv_sec;
                    struct tm utc;
                    gmtime_r(&seconds, &utc);
                    char iso[32];
                    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &utc);
                    char isoMs[48];
                    snprintf(isoMs, sizeof(isoMs), "%s.%03ldZ", iso, (long)(now.tv_usec / 1000));
                    DynamicJsonDocument doc(256);
                    doc["time"] = String(isoMs);
                    doc["epoch"] = static_cast<int64_t>(seconds);
                    doc["uptime_ms"] = static_cast<int64_t>(millis());
                    cliJsonResponse(doc);
                }
                else if (cmd == "STATUS" || cmd == "INFO")
                {
                    // Output comprehensive status in condensed JSON format
                    DynamicJsonDocument doc(4096); // Increased size for comprehensive data
                    
                    // Storage information
                    StorageMode mode = getStorageMode();
                    doc["storage"] = (mode == StorageMode::SD_CARD) ? "SD" : "PS";
                    bool sdAvailable = isStorageModeSdCard(); // Use cached value, don't call ensureStorage() which can block
                    doc["sd"] = sdAvailable;
                    
                    // SD card free percentage
                    if (sdAvailable)
                    {
                        // Only read SD card stats if available (may be slow, but status command is not time-critical)
                        uint64_t sdTotal = SD_MMC.totalBytes();
                        uint64_t sdUsed = SD_MMC.usedBytes();
                        uint64_t sdFree = (sdTotal > sdUsed) ? (sdTotal - sdUsed) : 0;
                        // Calculate free percentage with 2 decimal places
                        if (sdTotal > 0)
                        {
                            double freePercent = (static_cast<double>(sdFree) / static_cast<double>(sdTotal)) * 100.0;
                            doc["sdFree"] = static_cast<int>(freePercent * 100.0) / 100.0; // Round to 2 decimal places
                        }
                        else
                        {
                            doc["sdFree"] = 0.0;
                        }
                    }
                    else
                    {
                        doc["sdFree"] = nullptr;
                    }
                    
                    // Heap memory information
                    uint32_t heapTotal = ESP.getHeapSize();
                    uint32_t heapFree = ESP.getFreeHeap();
                    uint32_t heapUsed = (heapTotal > heapFree) ? (heapTotal - heapFree) : 0;
                    uint32_t minFreeHeap = ESP.getMinFreeHeap();
                    JsonObject heapObj = doc.createNestedObject("heap");
                    heapObj["total"] = heapTotal;
                    heapObj["free"] = heapFree;
                    heapObj["used"] = heapUsed;
                    heapObj["minFree"] = minFreeHeap;
                    #ifdef ESP32
                    uint32_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
                    heapObj["FreeBlock"] = largestFreeBlock;
                    #endif
                    
                    // PSRAM information
                    JsonObject psramObj = doc.createNestedObject("psram");
                    #ifdef ESP32
                    uint32_t psramTotal = ESP.getPsramSize();
                    if (psramTotal > 0)
                    {
                        uint32_t psramFree = ESP.getFreePsram();
                        uint32_t psramUsed = (psramTotal > psramFree) ? (psramTotal - psramFree) : 0;
                        psramObj["total"] = psramTotal;
                        psramObj["free"] = psramFree;
                        psramObj["used"] = psramUsed;
                    }
                    else
                    {
                        psramObj["total"] = nullptr;
                        psramObj["free"] = nullptr;
                        psramObj["used"] = nullptr;
                    }
                    #else
                    psramObj["total"] = nullptr;
                    psramObj["free"] = nullptr;
                    psramObj["used"] = nullptr;
                    #endif
                    
                    // Recording and upload status
                    doc["record"] = recorder_isRecording();
                    doc["upload"] = system_isUploading();
                    doc["queue"] = system_getUploadQueueSize();
                    
                    // WiFi information
                    JsonObject wifiObj = doc.createNestedObject("wifi");
                    wifiObj["conn"] = WiFi.isConnected();
                    if (WiFi.isConnected())
                    {
                        wifiObj["ssid"] = WiFi.SSID();
                        wifiObj["ip"] = WiFi.localIP().toString();
                        wifiObj["rssi"] = WiFi.RSSI();
                    }
                    else
                    {
                        wifiObj["ssid"] = nullptr;
                        wifiObj["ip"] = nullptr;
                        wifiObj["rssi"] = nullptr;
                    }
                    
                    // Device uptime (in seconds)
                    unsigned long uptimeMs = millis();
                    doc["uptime"] = static_cast<int64_t>(uptimeMs / 1000);
                    
                    // Time information
                    JsonObject timeObj = doc.createNestedObject("time");
                    struct timeval now;
                    gettimeofday(&now, nullptr);
                    time_t seconds = now.tv_sec;
                    timeObj["valid"] = timeKeeper().timeIsValid();
                    timeObj["RTC"] = timeKeeper().hasRtc();
                    if (timeKeeper().timeIsValid() && seconds > 0)
                    {
                        struct tm utc;
                        gmtime_r(&seconds, &utc);
                        char iso[32];
                        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &utc);
                        char isoMs[48];
                        snprintf(isoMs, sizeof(isoMs), "%s.%03ldZ", iso, (long)(now.tv_usec / 1000));
                        timeObj["time"] = String(isoMs);
                        timeObj["epoch"] = static_cast<int64_t>(seconds);
                    }
                    else
                    {
                        timeObj["time"] = nullptr;
                        timeObj["epoch"] = nullptr;
                    }
                    timeObj["uptime"] = static_cast<int64_t>(uptimeMs / 1000);
                    
                    // Recording statistics (from memory, no SD card operations)
                    RecordingStats sessionStats = recorder_getSessionStats();
                    JsonObject recordings = doc.createNestedObject("recordings");
                    recordings["total"] = sessionStats.recordingCount;
                    recordings["duration"] = static_cast<int64_t>(sessionStats.totalDurationMs);
                    recordings["uploaded"] = sessionStats.uploadedCount;
                    recordings["error"] = sessionStats.errorCount;
                    
                    // Get last upload epoch once (used for both recordings and api sections)
                    time_t lastUploadEpoch = network_getLastUploadEpoch();
                    
                    // Last upload time (epoch and ISO string)
                    if (isEpochValid(lastUploadEpoch))
                    {
                        recordings["lastEpoch"] = static_cast<int64_t>(lastUploadEpoch);
                        recordings["lastTime"] = formatIsoTimestamp(lastUploadEpoch, 0);
                    }
                    else
                    {
                        recordings["lastEpoch"] = nullptr;
                        recordings["lastTime"] = nullptr;
                    }
                    
                    // Audio level statistics (compact: only dB and current dynamic range)
                    AudioLevelStats audioStats = recorder_getAudioLevelStats();
                    JsonObject audio = doc.createNestedObject("audio");
                    // Round to 2 decimal places
                    audio["currentDb"] = static_cast<int>(audioStats.currentDb * 100.0) / 100.0;
                    audio["minDb"] = static_cast<int>(audioStats.minDb * 100.0) / 100.0;
                    audio["maxDb"] = static_cast<int>(audioStats.maxDb * 100.0) / 100.0;
                    // Average dB: average of min and max
                    double avgDb = (audioStats.minDb + audioStats.maxDb) / 2.0;
                    audio["avgDb"] = static_cast<int>(avgDb * 100.0) / 100.0;
                    // Current dynamic range utilization only
                    audio["currentDynamic"] = static_cast<int>(audioStats.dynamicRangeUtil * 100.0) / 100.0;
                    
                    // Network/API statistics
                    JsonObject apiObj = doc.createNestedObject("api");
                    apiObj["dead"] = network_areAllEndpointsDead();
                    apiObj["recovery"] = static_cast<int64_t>(0); // Recovery removed
                    
                    // Last upload time (ISO string of the start time of the last file uploaded)
                    if (isEpochValid(lastUploadEpoch))
                    {
                        apiObj["lastUpload"] = formatIsoTimestamp(lastUploadEpoch, 0);
                    }
                    else
                    {
                        apiObj["lastUpload"] = nullptr;
                    }
                    
                    // Last event time (ISO string)
                    time_t lastEventEpoch = network_getLastEventEpoch();
                    if (isEpochValid(lastEventEpoch))
                    {
                        apiObj["lastEvent"] = formatIsoTimestamp(lastEventEpoch, 0);
                    }
                    else
                    {
                        apiObj["lastEvent"] = nullptr;
                    }
                    
                    // Event and upload counts
                    apiObj["Events"] = static_cast<int64_t>(network_getTotalEventCount());
                    apiObj["Uploads"] = static_cast<int64_t>(network_getTotalUploadCount());
                    
                    // Configuration summary (compact format)
                    JsonObject config = doc.createNestedObject("config");
                    config["mac"] = getDeviceId();
                    config["fw"] = FIRMWARE;
                    if (appSettings.upload.apiHosts[0] && strlen(appSettings.upload.apiHosts[0]) > 0)
                    {
                        config["host"] = String(appSettings.upload.apiHosts[0]);
                        uint16_t apiPort = appSettings.upload.apiPorts[0];
                        if (apiPort == 0)
                        {
                            // Port must be specified - use 0 as error indicator
                            apiPort = 0;
                        }
                        config["port"] = apiPort;
                    }
                    else
                    {
                        config["host"] = nullptr;
                        config["port"] = nullptr;
                    }
                    config["athr"] = appSettings.audio.audioThreshold;
                    config["min"] = appSettings.audio.minRecordingMs;
                    config["max"] = appSettings.audio.maxRecordingMs;
                    config["silence"] = appSettings.audio.silenceThresholdMs;
                    config["gain"] = appSettings.audio.codecGainDb;
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
                    config["inchan"] = appSettings.audio.recordInputChannel;
#endif
                    config["samples"] = appSettings.audio.sampleRate;
                    config["pre"] = appSettings.audio.preRecordMs;
                    
                    cliJsonResponse(doc);
                }
                else if (cmd == "STATUS_SHORT" || cmd == "STATUSSHORT")
                {
                    DynamicJsonDocument doc(512);
                    doc["record"] = recorder_isRecording();
                    doc["upload"] = system_isUploading();
                    doc["queue"] = system_getUploadQueueSize();
                    doc["uptime"] = static_cast<int64_t>(millis() / 1000);
                    
                    RecordingStats sessionStats = recorder_getSessionStats();
                    doc["recorded"] = sessionStats.recordingCount;
                    doc["uploaded"] = sessionStats.uploadedCount;
                    
                    AudioLevelStats audioStats = recorder_getAudioLevelStats();
                    JsonObject audio = doc.createNestedObject("audio");
                    audio["currentDb"] = static_cast<int>(audioStats.currentDb * 100.0) / 100.0;
                    audio["minDb"] = static_cast<int>(audioStats.minDb * 100.0) / 100.0;
                    audio["maxDb"] = static_cast<int>(audioStats.maxDb * 100.0) / 100.0;
                    double avgDb = (audioStats.minDb + audioStats.maxDb) / 2.0;
                    audio["avgDb"] = static_cast<int>(avgDb * 100.0) / 100.0;
                    
                    cliJsonResponse(doc);
                }
                else if (cmd == "IP")
                {
                    DynamicJsonDocument doc(256);
                    doc["connected"] = WiFi.isConnected();
                    doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("null");
                    cliJsonResponse(doc);
                }
                else if (cmd == "MAC")
                {
                    cliOk("mac", getDeviceId());
                }
                else if (cmd == "RECORDINGS" || cmd == "RECORDINGSSUMMARY")
                {
                    RecordingStats stats = recorder_getSessionStats();
                    DynamicJsonDocument doc(256);
                    doc["count"] = stats.recordingCount;
                    doc["total_duration_ms"] = static_cast<int64_t>(stats.totalDurationMs);
                    doc["uploaded"] = stats.uploadedCount;
                    doc["errors"] = stats.errorCount;
                    cliJsonResponse(doc);
                }
                else if (cmd == "AUDIOLEVEL" || cmd == "AUDIOLEVELS" || cmd == "VU")
                {
                    AudioLevelStats stats = recorder_getAudioLevelStats();
                    DynamicJsonDocument doc(384);
                    doc["currentLevel"] = stats.currentLevel;
                    doc["currentDb"] = static_cast<int>(stats.currentDb * 100.0) / 100.0;
                    doc["minLevel"] = stats.minLevel;
                    doc["minDb"] = static_cast<int>(stats.minDb * 100.0) / 100.0;
                    doc["maxLevel"] = stats.maxLevel;
                    doc["maxDb"] = static_cast<int>(stats.maxDb * 100.0) / 100.0;
                    doc["averageLevel"] = stats.averageLevel;
                    doc["peakSample"] = stats.peakSample;
                    cliJsonResponse(doc);
                }
                else if (cmd == "ERRORS" || cmd == "ERROR")
                {
                    ErrorMessage errors[50];
                    size_t count = logger_getRecentErrors(errors, 50);
                    unsigned long lastSeqId = logger_getLastErrorSequenceId();
                    DynamicJsonDocument doc(2048);
                    doc["lastSequenceId"] = lastSeqId;
                    doc["count"] = count;
                    JsonArray arr = doc.createNestedArray("errors");
                    for (size_t i = 0; i < count && i < 10; ++i)
                    {
                        JsonObject e = arr.createNestedObject();
                        e["level"] = (errors[i].level == LogLevel::FATAL) ? "FATAL" : "ERROR";
                        e["message"] = errors[i].message;
                        e["seq"] = errors[i].sequenceId;
                    }
                    cliJsonResponse(doc);
                }
                else if (cmd == "CD" || cmd == "CHDIR")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest.length() == 0) { cliOk("directory", currentDirectory); break; }
                    
                    String targetPath = currentDirectory;
                    if (rest == "/") { targetPath = "/"; }
                    else if (rest.startsWith("/")) { targetPath = rest; }
                    else { if (!currentDirectory.endsWith("/")) targetPath += "/"; targetPath += rest; }
                    
                    targetPath.replace("//", "/");
                    if (targetPath.endsWith("/") && targetPath.length() > 1) targetPath.remove(targetPath.length() - 1);
                    
                    if (!SD_MMC.exists(targetPath)) { cliError(ERR_HW_ERROR, String("directory not found: ") + targetPath); break; }
                    
                    File dir = SD_MMC.open(targetPath);
                    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); cliError(ERR_HW_ERROR, String("not a directory: ") + targetPath); break; }
                    dir.close();
                    
                    currentDirectory = targetPath;
                    cliOk("directory", currentDirectory);
                }
                else if (cmd == "DIR" || cmd == "LS" || cmd == "LIST")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    if (!ensureStorage()) { cliError(ERR_HW_ERROR, "storage not initialized"); break; }
                    
                    File dir = SD_MMC.open(currentDirectory);
                    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); cliError(ERR_HW_ERROR, String("cannot open: ") + currentDirectory); break; }
                    
                    DynamicJsonDocument doc(4096);
                    doc["directory"] = currentDirectory;
                    JsonArray entries = doc.createNestedArray("entries");
                    int fileCount = 0, dirCount = 0;
                    
                    File file = dir.openNextFile();
                    while (file)
                    {
                        String fileName = String(file.name());
                        if (fileName.startsWith(currentDirectory))
                        {
                            fileName = fileName.substring(currentDirectory.length());
                            if (fileName.startsWith("/")) fileName = fileName.substring(1);
                        }
                        
                        JsonObject entry = entries.createNestedObject();
                        entry["name"] = fileName;
                        if (file.isDirectory()) { entry["type"] = "dir"; dirCount++; }
                        else { entry["type"] = "file"; entry["size"] = static_cast<int64_t>(file.size()); fileCount++; }
                        file.close();
                        file = dir.openNextFile();
                    }
                    dir.close();
                    
                    doc["files"] = fileCount;
                    doc["dirs"] = dirCount;
                    cliJsonResponse(doc);
                }
                else if (cmd == "RM" || cmd == "DELETE" || cmd == "DEL")
                {
                    if (!isStorageModeSdCard()) { cliError(ERR_HW_ERROR, "SD card not available"); break; }
                    if (!ensureStorage()) { cliError(ERR_HW_ERROR, "storage not initialized"); break; }
                    
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest.length() == 0) { cliError(ERR_MISSING_PARAM, "usage: rm <filename> or rm *"); break; }
                    
                    if (rest == "*")
                    {
                        File dir = SD_MMC.open(currentDirectory);
                        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); cliError(ERR_HW_ERROR, String("cannot open: ") + currentDirectory); break; }
                        
                        int deletedCount = 0, failedCount = 0;
                        File file = dir.openNextFile();
                        while (file)
                        {
                            String filePath = String(file.name());
                            if (!file.isDirectory()) { file.close(); if (SD_MMC.remove(filePath)) deletedCount++; else failedCount++; }
                            else { file.close(); }
                            file = dir.openNextFile();
                        }
                        dir.close();
                        
                        DynamicJsonDocument doc(256);
                        doc["deleted"] = deletedCount;
                        doc["failed"] = failedCount;
                        cliJsonResponse(doc);
                    }
                    else
                    {
                        String targetPath = currentDirectory;
                        if (rest.startsWith("/")) { targetPath = rest; }
                        else { if (!currentDirectory.endsWith("/")) targetPath += "/"; targetPath += rest; }
                        targetPath.replace("//", "/");
                        
                        if (!SD_MMC.exists(targetPath)) { cliError(ERR_HW_ERROR, String("file not found: ") + targetPath); break; }
                        
                        File checkFile = SD_MMC.open(targetPath);
                        if (checkFile && checkFile.isDirectory()) { checkFile.close(); cliError(ERR_INVALID_VALUE, "cannot delete directory"); break; }
                        if (checkFile) checkFile.close();
                        
                        if (SD_MMC.remove(targetPath)) { cliOk("deleted", targetPath); }
                        else { cliError(ERR_HW_ERROR, String("could not delete: ") + targetPath); }
                    }
                }
                else if (cmd == "RECOVER" || cmd == "RECOVERY")
                {
                    if (!isWiFiConnected())
                    {
                        connectToWiFi();
                        delay(2000);
                    }
                    
                    if (!isWiFiConnected())
                    {
                        cliError(ERR_HW_ERROR, "WiFi not connected");
                        break;
                    }
                    
                    network_invalidateApiEndpoints();
                    
                    DynamicJsonDocument doc(256);
                    doc["endpoints_reset"] = true;
                    doc["all_dead"] = network_areAllEndpointsDead();
                    doc["queue_size"] = system_getUploadQueueSize();
                    cliJsonResponse(doc);
                }
                else if (cmd == "RECONNECT" || cmd == "RECONNECTWIFI" || cmd == "WIFI_RECONNECT")
                {
                    WiFi.disconnect(true, true);
                    delay(500);
                    WiFi.mode(WIFI_OFF);
                    delay(500);
                    WiFi.mode(WIFI_STA);
                    network_reinitializeWiFi();
                    delay(500);
                    connectToWiFi();
                    delay(1000);
                    
                    bool connected = isWiFiConnected();
                    if (connected) network_invalidateApiEndpoints();
                    
                    DynamicJsonDocument doc(256);
                    doc["connected"] = connected;
                    if (connected) doc["ip"] = WiFi.localIP().toString();
                    cliJsonResponse(doc);
                }
                else if (cmd == "SAMPLE")
                {
                    if (recorder_isRecording())
                    {
                        cliError(ERR_BUSY, "recording already in progress");
                        break;
                    }
                    
                    if (recorder_startSampleRecording())
                    {
                        cliOk();
                    }
                    else
                    {
                        cliError(ERR_HW_ERROR, "could not start sample recording");
                    }
                }
                else if (cmd == "HEALTH")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    
                    if (rest == "?")
                    {
                        health_loadYearlySummary();
                        sendHealthMessage(true);
                        cliOk();
                    }
                    else
                    {
                        cliError(ERR_MISSING_PARAM, "usage: health ?");
                    }
                }
                else if (cmd == "DEBUG")
                {
                    String rest = (firstSpace == -1) ? String() : line.substring(firstSpace + 1);
                    rest.trim();
                    rest.toUpperCase();
                    
                    if (rest.length() == 0)
                    {
                        bool debugEnabled = appSettings.log.serialInfo && appSettings.log.serialDebug;
                        cliOk("debug", debugEnabled ? "true" : "false");
                    }
                    else
                    {
                        bool enableDebug = false;
                        if (rest == "TRUE" || rest == "1" || rest == "ON" || rest == "YES")
                        {
                            enableDebug = true;
                        }
                        else if (rest == "FALSE" || rest == "0" || rest == "OFF" || rest == "NO")
                        {
                            enableDebug = false;
                        }
                        else
                        {
                            cliError(ERR_INVALID_VALUE, "use true/false or 1/0");
                            RELEASE_AND_BREAK();
                        }
                        
                        appSettings.log.serialInfo = enableDebug;
                        appSettings.log.serialDebug = enableDebug;
                        
                        if (settings_save())
                        {
                            cliOk("debug", enableDebug ? "true" : "false");
                        }
                        else
                        {
                            cliError(ERR_HW_ERROR, "failed to save settings");
                        }
                    }
                }
                else
                {
                    cliError(ERR_UNKNOWN_CMD, String("unknown command \"") + cmd + "\"");
                }
                
                // Release Serial mutex before breaking out of character reading loop
                if (mutexAcquired)
                {
                    releaseSerialMutex();
                    mutexAcquired = false;
                }
                
                // Undefine the helper macro
                #undef RELEASE_AND_BREAK
                
                // Break out of character reading loop after processing command
                break;
            }
            else
            {
                // Empty line, just clear buffer
                inputBuffer = "";
            }
        }
        // Handle backspace/delete
        else if (c == '\b' || c == 127) // Backspace or Delete
        {
            if (inputBuffer.length() > 0)
            {
                inputBuffer.remove(inputBuffer.length() - 1);
            }
        }
        // Handle printable characters
        else if (c >= 32 || c == '\t')
        {
            // Prevent buffer overflow
            if (inputBuffer.length() < kMaxInputLength)
            {
                inputBuffer += c;
            }
        }
        // Ignore other control characters
        
    }
}

String settings_getLastError()
{
    return g_lastSettingsError;
}

String settings_getLastErrorCode()
{
    return g_lastSettingsErrorCode;
}
