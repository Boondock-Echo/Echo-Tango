#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

constexpr size_t kMaxUploadPathLength = 128;

struct UploadRequest
{
	char path[kMaxUploadPathLength];
	uint32_t durationMs = 0;
	float peakDb = -120.0f;
	size_t sizeBytes = 0;
	size_t fileSize = 0;
	unsigned long recordedAtMs = 0;
	time_t recordedAtEpoch = 0;
	char endReason[32] = {0};
	uint8_t attempts = 0;
	
	// PSRAM mode support
	bool isPsramMode = false;
	uint8_t* psramData = nullptr; // Pointer to PSRAM buffer containing WAV file data
};

void network_begin();
void network_loop();

bool isWiFiConnected();
bool network_hasAnyWiFiCredentials(); // Returns true if at least one WiFi SSID is configured

void connectToWiFi();
void network_reconnectWiFi(); // Reconnect WiFi (same as RECONNECT CLI command)
bool uploadAudioFile(const UploadRequest &request, bool sendTags = false);
/// Empty string if last uploadAudioFile succeeded; otherwise short reason (wifi, open_failed, last server error key, etc.).
const char* network_getLastUploadFailureReason();
bool uploadLogFile(const String &logFilePath);
void sendEvent(const String &type, const String &message, JsonObject *settings = nullptr);

// Settings synchronization helpers
// Push full settings JSON to API server via /api/v1/settings (multipart/form-data)
bool network_pushSettingsToServer(const String &settingsJson);
// Pull settings JSON from API server via /api/v1/settings/{mac_address}
bool network_pullSettingsFromServer(String &outSettingsJson);
void network_invalidateApiEndpoints();
bool network_areAllEndpointsDead(); // Returns true if all enabled endpoints are dead (callers handle logging)
void network_retryDeadEndpoints(); // Periodically retry dead endpoints when WiFi is connected
void network_reinitializeWiFi(); // Reinitialize WiFi and re-register event handlers

// Cloud path (TCP to configured API) vs WiFi link-only: record outcomes from connect attempts while STA is up.
void network_recordCloudPathConnectResult(bool success);
// True when recent TCP connects to the cloud have succeeded (no streak of failures since last success).
bool network_isCloudPathOk();
// True when cloud TCP has been down long enough that a full reboot was scheduled (see CLOUD_PATH_FULL_REBOOT_AFTER_MS).
bool network_isCloudPathFullRebootDue();
void network_clearCloudPathFullRebootDue();

// Select a random healthy API endpoint (enabled, not dead, with valid host/port).
// Returns true on success and fills indexOut, hostOut, and portOut.
// Returns false if no suitable endpoint is available.
bool network_getRandomHealthyEndpoint(size_t &indexOut, const char* &hostOut, uint16_t &portOut);

// Get upload and event statistics
time_t network_getLastUploadEpoch(); // Returns epoch time of the start time of the last file uploaded (0 if none)
unsigned long network_getTotalUploadCount(); // Returns total number of successful uploads
unsigned long network_getTotalUploadAttempts(); // Returns total number of upload attempts (successful + failed)
float network_getOverallUploadSuccessRate(); // Returns overall upload success rate (0.0-1.0, or -1.0 if no attempts)
unsigned long network_getTotalEventCount(); // Returns total number of events sent
time_t network_getLastEventEpoch(); // Returns epoch time of the last event sent (0 if none)
void network_incrementUploadAttempt(); // Increment upload attempt counter (call before upload attempt)

// Network quality and endpoint health functions
void network_updateRssi(); // Update RSSI metrics
void network_recordEndpointRequest(size_t endpointIndex, bool success, unsigned long responseTimeMs); // Record endpoint request metrics
void network_updateEndpointHealthScore(size_t endpointIndex); // Update endpoint health score
bool network_isCircuitBreakerOpen(size_t endpointIndex); // Check if circuit breaker is open for endpoint
void network_resetCircuitBreaker(size_t endpointIndex); // Reset circuit breaker for endpoint
uint32_t network_calculateAdaptiveBackoff(size_t endpointIndex, uint32_t baseBackoffMs); // Calculate adaptive backoff based on network quality

void network_drainEventsUntilMillis(unsigned long deadlineMs);

#if defined(ECHO)
void network_pumpEventsOnce();
void network_echoGetEventDropStats(unsigned long *droppedTooOld, unsigned long *droppedNoTime,
								   unsigned long *droppedQueueFull);
#endif