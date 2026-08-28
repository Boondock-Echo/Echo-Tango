#pragma once

#include <Arduino.h>

// Debugging settings turn it on or off where debugging.
#define DEBUG_ENABLED           1
#if DEBUG_ENABLED
    #define LOG_DEBUG(...)      Serial.printf("[DEBUG]: " __VA_ARGS__)

#else
    #define LOG_DEBUG(...)
#endif

// Initialize settings subsystem (reads NVS or creates defaults)
bool settings_begin();

// Process any serial input for the CLI (call frequently from loop)
void settings_processSerial();

// Get a textual representation of a parameter (returns empty string if not found)
String settings_getParam(const String &param);

// Set a parameter to a textual value; returns true if set and saved
bool settings_setParam(const String &param, const String &value);

// Force save current appSettings to NVS
bool settings_save();

// Clear debounce state to force immediate save (useful for AUTOCONFIG)
void settings_clearDebounce();

// Force reload from NVS (overwrites current appSettings)
bool settings_reload();

// Return a JSON string containing all current settings (same shape as saved JSON)
String settings_getAllJson();

// Return a JSON string suitable for sending to the API server:
// WiFi SSIDs and passwords are masked as "HIDDEN_FOR_SECURITY".
String settings_getMaskedJsonForServer();

// Retrieve the latest error message recorded by settings operations.
String settings_getLastError();

// Retrieve the latest error code (e.g. "INVALID_VALUE", "OUT_OF_RANGE").
String settings_getLastErrorCode();

// Update settings from a full JSON payload. If saveAsync is true the settings
// will be committed to NVS on a background task; otherwise save synchronously.
// Returns true if the JSON parsed and applied successfully (save may still fail).
bool settings_updateAllFromJson(const String &json, bool saveAsync = true);

// Apply a full settings JSON payload that originated from the API server.
// Strips masked WiFi credentials and suppresses per-setting change events
// during the update to avoid event loops.
bool settings_applyJsonFromServer(const String &json);

// Enable/disable per-setting change events (used internally to avoid loops).
void settings_setChangeEventsSuppressed(bool suppressed);

// Schedule a background commit of current settings to NVS. Returns true if
// a background task to perform the save was created successfully.
bool settings_commitAsync();

// Get NVS settings health metrics as JSON string (for diagnostics)
void settings_getEepromHealth(String& output);

// Get Serial mutex for protecting Serial output from other files
// Returns nullptr if mutex not initialized
SemaphoreHandle_t settings_getSerialMutex();

// Erase WiFi credentials and NVS settings, then reboot. Returns false if NVS cannot be opened.
bool settings_factoryReset(bool emitCliResponse = false);