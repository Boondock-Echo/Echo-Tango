#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * Centralized NVS Management Library
 * 
 * This library provides efficient NVS operations for the ESP32 using the custom
 * 64KB NVS partition. It supports multiple namespaces and provides thread-safe
 * operations with optimized read/write performance.
 * 
 * Features:
 * - Thread-safe operations with mutex protection
 * - Efficient batch operations
 * - Namespace management for different use cases
 * - Error tracking and health metrics
 * - Automatic initialization and cleanup
 */

// NVS namespace constants for different use cases
namespace NvsNamespace {
    constexpr const char* UPLOAD_QUEUE = "uploadq";  // Recording upload queue
    constexpr const char* LOGS = "logs";             // Recording logs metadata
    constexpr const char* SYSTEM = "system";          // System state
    // Add more namespaces as needed
}

// NVS operation result
enum class NvsResult {
    SUCCESS,
    ERROR_NOT_INITIALIZED,
    ERROR_NAMESPACE_OPEN_FAILED,
    ERROR_KEY_NOT_FOUND,
    ERROR_READ_FAILED,
    ERROR_WRITE_FAILED,
    ERROR_MUTEX_TIMEOUT,
    ERROR_INVALID_PARAMETER,
    ERROR_OUT_OF_MEMORY
};

// NVS health metrics
struct NvsHealth {
    unsigned long totalReads;
    unsigned long totalWrites;
    unsigned long readErrors;
    unsigned long writeErrors;
    unsigned long mutexTimeouts;
    unsigned long lastErrorMs;
    String lastError;
};

/**
 * Initialize the NVS management system
 * Must be called once at startup before any NVS operations
 */
bool nvs_begin();

/**
 * Shutdown the NVS management system
 * Call this before system shutdown if needed
 */
void nvs_end();

/**
 * Open a namespace for read/write operations
 * Returns true on success, false on error
 * 
 * @param namespace_name Name of the namespace to open
 * @param readOnly If true, opens in read-only mode (more efficient)
 * @param prefs Reference to Preferences object (will be initialized)
 * @return true if namespace opened successfully
 */
bool nvs_openNamespace(const char* namespace_name, bool readOnly, Preferences& prefs);

/**
 * Close a namespace (cleanup)
 * Always call this after finishing operations on a namespace
 * 
 * @param prefs Reference to Preferences object to close
 */
void nvs_closeNamespace(Preferences& prefs);

/**
 * Read a byte value from NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param defaultValue Default value if key doesn't exist
 * @param outValue Output parameter for the read value
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_readUChar(Preferences& prefs, const char* key, uint8_t defaultValue, uint8_t& outValue);

/**
 * Write a byte value to NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param value Value to write
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_writeUChar(Preferences& prefs, const char* key, uint8_t value);

/**
 * Read a 32-bit unsigned integer from NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param defaultValue Default value if key doesn't exist
 * @param outValue Output parameter for the read value
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_readUInt(Preferences& prefs, const char* key, uint32_t defaultValue, uint32_t& outValue);

/**
 * Write a 32-bit unsigned integer to NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param value Value to write
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_writeUInt(Preferences& prefs, const char* key, uint32_t value);

/**
 * Read binary data from NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param buffer Buffer to read into
 * @param bufferSize Size of buffer (will be updated with actual bytes read)
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_readBytes(Preferences& prefs, const char* key, uint8_t* buffer, size_t& bufferSize);

/**
 * Write binary data to NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param buffer Buffer containing data to write
 * @param bufferSize Number of bytes to write
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_writeBytes(Preferences& prefs, const char* key, const uint8_t* buffer, size_t bufferSize);

/**
 * Read a string from NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param defaultValue Default value if key doesn't exist
 * @param outValue Output parameter for the read value
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_readString(Preferences& prefs, const char* key, const char* defaultValue, String& outValue);

/**
 * Write a string to NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param value String value to write
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_writeString(Preferences& prefs, const char* key, const char* value);

/**
 * Check if a key exists in NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name
 * @param exists Output parameter indicating if key exists
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_keyExists(Preferences& prefs, const char* key, bool& exists);

/**
 * Remove a key from NVS
 * 
 * @param prefs Open Preferences object
 * @param key Key name to remove
 * @return NvsResult indicating success or error type
 */
NvsResult nvs_removeKey(Preferences& prefs, const char* key);

/**
 * Clear all keys in a namespace
 * 
 * @param namespace_name Name of the namespace to clear
 * @return true if cleared successfully
 */
bool nvs_clearNamespace(const char* namespace_name);

/**
 * Get health metrics for NVS operations
 * 
 * @return NvsHealth structure with current metrics
 */
NvsHealth nvs_getHealth();

/**
 * Reset health metrics (for testing/debugging)
 */
void nvs_resetHealth();





