#include "nvs.h"
#include "logger.h"
#include <cstring>

namespace {
    // Mutex for thread-safe NVS operations
    SemaphoreHandle_t g_nvsMutex = nullptr;
    constexpr TickType_t kNvsMutexTimeoutMs = pdMS_TO_TICKS(5000);
    
    // Initialization flag
    bool g_nvsInitialized = false;
    
    // Health metrics
    NvsHealth g_nvsHealth = {};
    
    // Initialize mutex (only if FreeRTOS scheduler is running)
    void ensureMutex() {
        if (g_nvsMutex == nullptr) {
            // Check if scheduler is running before creating mutex
            // xTaskGetSchedulerState() returns pdTRUE if scheduler is running
            if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
                g_nvsMutex = xSemaphoreCreateMutex();
                if (g_nvsMutex == nullptr) {
                    // Can't log here - might cause issues
                }
            }
            // If scheduler not started, mutex will be created on first use
        }
    }
    
    // Acquire mutex (returns true if acquired)
    bool acquireMutex() {
        ensureMutex();
        if (g_nvsMutex == nullptr) {
            return false;
        }
        
        BaseType_t result = xSemaphoreTake(g_nvsMutex, kNvsMutexTimeoutMs);
        if (result != pdTRUE) {
            g_nvsHealth.mutexTimeouts++;
            g_nvsHealth.lastErrorMs = millis();
            g_nvsHealth.lastError = "Mutex timeout";
            return false;
        }
        return true;
    }
    
    // Release mutex
    void releaseMutex() {
        if (g_nvsMutex != nullptr) {
            xSemaphoreGive(g_nvsMutex);
        }
    }
    
    // Update error tracking
    void recordError(const String& errorMsg, bool isRead) {
        g_nvsHealth.lastErrorMs = millis();
        g_nvsHealth.lastError = errorMsg;
        if (isRead) {
            g_nvsHealth.readErrors++;
        } else {
            g_nvsHealth.writeErrors++;
        }
    }
}

bool nvs_begin() {
    if (g_nvsInitialized) {
        return true;
    }
    
    // Initialize NVS partition first (this is critical - must succeed)
    // Note: We don't use Preferences here, just ensure NVS is initialized
    // The actual namespace opening happens in nvs_openNamespace
    
    ensureMutex();
    if (g_nvsMutex == nullptr) {
        // Can't use logger here as it might not be initialized yet
        // Serial.println("[NVS] Failed to initialize mutex");
        return false;
    }
    
    // Reset health metrics
    g_nvsHealth = {};
    
    g_nvsInitialized = true;
    // Don't log here - logger might not be ready
    return true;
}

void nvs_end() {
    if (!g_nvsInitialized) {
        return;
    }
    
    if (g_nvsMutex != nullptr) {
        vSemaphoreDelete(g_nvsMutex);
        g_nvsMutex = nullptr;
    }
    
    g_nvsInitialized = false;
}

bool nvs_openNamespace(const char* namespace_name, bool readOnly, Preferences& prefs) {
    if (!g_nvsInitialized) {
        // Don't log here - might cause issues during early initialization
        return false;
    }
    
    if (namespace_name == nullptr || strlen(namespace_name) == 0) {
        recordError(String("Invalid namespace name"), false);
        return false;
    }
    
    // Note: Preferences.begin() is thread-safe internally, but we use mutex
    // for our own operations to ensure consistency
    // If NVS partition is corrupted, begin() will return false - handle gracefully
    bool success = prefs.begin(namespace_name, readOnly);
    
    if (!success) {
        recordError(String("Failed to open namespace: ") + String(namespace_name), false);
        // Don't log here during early initialization to avoid potential crashes
        return false;
    }
    
    return true;
}

void nvs_closeNamespace(Preferences& prefs) {
    prefs.end();
}

NvsResult nvs_readUChar(Preferences& prefs, const char* key, uint8_t defaultValue, uint8_t& outValue) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), true);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalReads++;
    
    if (!prefs.isKey(key)) {
        outValue = defaultValue;
        releaseMutex();
        return NvsResult::ERROR_KEY_NOT_FOUND;
    }
    
    outValue = prefs.getUChar(key, defaultValue);
    releaseMutex();
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_writeUChar(Preferences& prefs, const char* key, uint8_t value) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), false);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalWrites++;
    
    bool success = prefs.putUChar(key, value);
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to write UChar: ") + String(key), false);
        return NvsResult::ERROR_WRITE_FAILED;
    }
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_readUInt(Preferences& prefs, const char* key, uint32_t defaultValue, uint32_t& outValue) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), true);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalReads++;
    
    if (!prefs.isKey(key)) {
        outValue = defaultValue;
        releaseMutex();
        return NvsResult::ERROR_KEY_NOT_FOUND;
    }
    
    outValue = prefs.getUInt(key, defaultValue);
    releaseMutex();
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_writeUInt(Preferences& prefs, const char* key, uint32_t value) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), false);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalWrites++;
    
    bool success = prefs.putUInt(key, value);
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to write UInt: ") + String(key), false);
        return NvsResult::ERROR_WRITE_FAILED;
    }
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_readBytes(Preferences& prefs, const char* key, uint8_t* buffer, size_t& bufferSize) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0 || buffer == nullptr) {
        recordError(String("Invalid parameters"), true);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalReads++;
    
    size_t actualSize = prefs.getBytes(key, buffer, bufferSize);
    bufferSize = actualSize;
    releaseMutex();
    
    if (actualSize == 0 && prefs.isKey(key)) {
        // Key exists but is empty - this is valid
        return NvsResult::SUCCESS;
    }
    
    if (actualSize == 0) {
        recordError(String("Key not found: ") + String(key), true);
        return NvsResult::ERROR_KEY_NOT_FOUND;
    }
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_writeBytes(Preferences& prefs, const char* key, const uint8_t* buffer, size_t bufferSize) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0 || buffer == nullptr) {
        recordError(String("Invalid parameters"), false);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalWrites++;
    
    bool success = prefs.putBytes(key, buffer, bufferSize);
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to write bytes: ") + String(key), false);
        return NvsResult::ERROR_WRITE_FAILED;
    }
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_readString(Preferences& prefs, const char* key, const char* defaultValue, String& outValue) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), true);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalReads++;
    
    if (!prefs.isKey(key)) {
        outValue = String(defaultValue ? defaultValue : "");
        releaseMutex();
        return NvsResult::ERROR_KEY_NOT_FOUND;
    }
    
    outValue = prefs.getString(key, defaultValue ? defaultValue : "");
    releaseMutex();
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_writeString(Preferences& prefs, const char* key, const char* value) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), false);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    g_nvsHealth.totalWrites++;
    
    bool success = prefs.putString(key, value ? value : "");
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to write string: ") + String(key), false);
        return NvsResult::ERROR_WRITE_FAILED;
    }
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_keyExists(Preferences& prefs, const char* key, bool& exists) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), true);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    exists = prefs.isKey(key);
    releaseMutex();
    
    return NvsResult::SUCCESS;
}

NvsResult nvs_removeKey(Preferences& prefs, const char* key) {
    if (!g_nvsInitialized) {
        return NvsResult::ERROR_NOT_INITIALIZED;
    }
    
    if (key == nullptr || strlen(key) == 0) {
        recordError(String("Invalid key"), false);
        return NvsResult::ERROR_INVALID_PARAMETER;
    }
    
    if (!acquireMutex()) {
        return NvsResult::ERROR_MUTEX_TIMEOUT;
    }
    
    bool success = prefs.remove(key);
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to remove key: ") + String(key), false);
        return NvsResult::ERROR_WRITE_FAILED;
    }
    
    return NvsResult::SUCCESS;
}

bool nvs_clearNamespace(const char* namespace_name) {
    if (!g_nvsInitialized) {
        return false;
    }
    
    if (namespace_name == nullptr || strlen(namespace_name) == 0) {
        recordError(String("Invalid namespace name"), false);
        return false;
    }
    
    if (!acquireMutex()) {
        return false;
    }
    
    Preferences prefs;
    if (!prefs.begin(namespace_name, false)) {
        releaseMutex();
        recordError(String("Failed to open namespace for clearing: ") + String(namespace_name), false);
        return false;
    }
    
    // Clear all keys (Preferences doesn't have a clearAll method, so we need to
    // iterate and remove keys manually - but Preferences doesn't provide iteration)
    // Instead, we'll just close and let the caller handle individual key removal
    // For now, we'll use Preferences::clear() if available, otherwise return false
    bool success = prefs.clear();
    
    prefs.end();
    releaseMutex();
    
    if (!success) {
        recordError(String("Failed to clear namespace: ") + String(namespace_name), false);
    }
    
    return success;
}

NvsHealth nvs_getHealth() {
    return g_nvsHealth;
}

void nvs_resetHealth() {
    if (!acquireMutex()) {
        return;
    }
    
    g_nvsHealth = {};
    releaseMutex();
}

