#include "network.h"

#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "boondock_server.h"
#include "common.h"
#include "config.h"
#include "logger.h"
#include "health.h"
#include "main.h"
#include "recorder.h"
#include "settings.h"
#include "timekeeper.h"
#include "upload_queue.h"
#include "system_assets.h"
#include "networkHandller.h"
#if defined(ECHO)
#include "mqtt_task.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include <WiFi.h>

void sendEvent(const String &type, const String &message, JsonObject *settings);

namespace
{
	constexpr unsigned long kMdnsRefreshIntervalMs = 5UL * 60UL * 1000UL;
	constexpr unsigned long kMdnsRetryIntervalMs = 30UL * 1000UL;
	unsigned long lastMdnsStartAttemptMs = 0;
	bool mdnsResponderRunning = false;
	IPAddress mdnsResponderIp;

	const String &getLocalHostname()
	{
		static String hostname;
		hostname = appSettings.hostname;
		return hostname;
	}

	bool startMdnsResponder()
	{
		const String &hostname = getLocalHostname();
		const bool wasRunning = mdnsResponderRunning;
		lastMdnsStartAttemptMs = millis();
		MDNS.end();
		if (!MDNS.begin(hostname.c_str()))
		{
			mdnsResponderRunning = false;
			logWarnf("[mDNS] Failed to register %s.local", hostname.c_str());
			return false;
		}

		MDNS.addService("http", "tcp", 80);
		mdnsResponderRunning = true;
		mdnsResponderIp = WiFi.localIP();
		if (wasRunning)
		{
			logDebugf("[mDNS] Refreshed http://%s.local", hostname.c_str());
		}
		else
		{
			logEventf("[mDNS] Device available at http://%s.local\n", hostname.c_str());
		}
		return true;
	}

	void maintainMdnsResponder()
	{
		if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
		{
			return;
		}

		const unsigned long now = millis();
		const unsigned long interval = mdnsResponderRunning
			? kMdnsRefreshIntervalMs
			: kMdnsRetryIntervalMs;
		const bool ipChanged = mdnsResponderRunning && mdnsResponderIp != WiFi.localIP();
		if (ipChanged || lastMdnsStartAttemptMs == 0 ||
			(now - lastMdnsStartAttemptMs) >= interval)
		{
			// ESPmDNS has no responder-health API. Periodically rebuilding it both
			// re-announces the records and recovers a responder that stopped replying.
			startMdnsResponder();
		}
	}

bool cloudPathMonitoringPaused()
{
	if (networkHandler_isUploadPaused())
	{
		return true;
	}
#if defined(ECHO)
	if (mqtt_isCommandInProgress())
	{
		return true;
	}
#endif
	return false;
}

/** Terminal JSON on UART (record/upload pattern); not gated by log.serialEvent */
static void wifiEmitStaTerminalSimple(const char *ev, const String &summaryLine)
{
	StaticJsonDocument<512> terminalDoc;
	terminalDoc["tm"] = getFormattedTimeWithTimezone();
	terminalDoc["ty"] = "event";
	terminalDoc["ev"] = ev;
	terminalDoc["ms"] = summaryLine;
	terminalDoc["mc"] = getDeviceId();
	terminalDoc["si"] = getSessionId();
	String terminalJson;
	serializeJson(terminalDoc, terminalJson);
	serialWriteJsonAtomic(terminalJson);
}
} // namespace

// Cloud path health: TCP to API while STA reports connected (file scope; used by anonymous namespace helpers).
static uint32_t g_cloudPathConsecutiveFailures = 0;
static unsigned long g_cloudPathLastSuccessMs = 0;
static unsigned long g_cloudPathLastRecoveryMs = 0;
static unsigned long g_cloudPathLastIdleProbeMs = 0;
static unsigned long g_cloudPathBootMs = 0;
static bool g_cloudPathFullRebootDue = false;

void network_clearCloudPathFullRebootDue()
{
	g_cloudPathFullRebootDue = false;
}

bool network_isCloudPathFullRebootDue()
{
	return g_cloudPathFullRebootDue;
}

void network_recordCloudPathConnectResult(bool success)
{
	if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
	{
		return;
	}
	if (success)
	{
		g_cloudPathConsecutiveFailures = 0;
		g_cloudPathLastSuccessMs = millis();
		g_cloudPathFullRebootDue = false;
		return;
	}
	if (g_cloudPathConsecutiveFailures < 0xFFFFU)
	{
		g_cloudPathConsecutiveFailures++;
	}
}

bool network_isCloudPathOk()
{
	return g_cloudPathConsecutiveFailures == 0;
}

bool network_getRandomHealthyEndpoint(size_t &indexOut, const char *&hostOut, uint16_t &portOut);
void network_reconnectWiFi();

// Mutex timeout tracking (file scope for access from other files)
MutexMetrics g_mutexMetrics = {};

// Network quality metrics (file scope for access from other files)
NetworkQualityMetrics g_networkQualityMetrics = {};

// Endpoint health metrics (file scope for access from other files)
EndpointHealthMetrics g_endpointHealthMetrics[kApiEndpointCount] = {};

// Upload mutex reference for heartbeat "not busy" check from network_loop (set by ensureUploadResources)
static SemaphoreHandle_t s_uploadMutexForHeartbeat = nullptr;

// Returns true if upload mutex is free (not held). Used to send heartbeat only when not busy with upload.
static bool isUploadMutexFree()
{
	if (s_uploadMutexForHeartbeat == nullptr)
	{
		return false;
	}
	if (xSemaphoreTake(s_uploadMutexForHeartbeat, 0) != pdTRUE)
	{
		return false;
	}
	xSemaphoreGive(s_uploadMutexForHeartbeat);
	return true;
}

static uint16_t getUploadPort(size_t idx)
{
	return appSettings.upload.apiPorts[idx];
}

namespace
{
	// Shared WiFi client for uploadAudioFile + cooperative event send from NetworkTask (single-threaded use).
	WiFiClient g_networkSharedWiFiClient;
	// Helper function to mask hostnames in logs for security
	const char *maskHostnameForLogging(const char *host)
	{
		if (host == nullptr || strlen(host) == 0)
		{
			return "[null]";
		}
		// Return generic placeholder to avoid exposing server infrastructure
		return "boondockecho.com";
	}

	bool wifiEverConnected = false;
	bool wifiDisconnectLogged = false;
	unsigned long wifiConnectedAtMs = 0;							  // Timestamp when WiFi last got IP address
	constexpr unsigned long kEventSendDelayAfterWiFiConnectMs = 3000; // Wait 3 seconds after WiFi connects before sending events

	// Deferred WiFi event handling to avoid stack overflow in arduino_events task
	struct DeferredWiFiEvent
	{
		bool gotIp = false;
		bool lostIp = false;
		bool disconnected = false;
		int disconnectReason = 0;
		char ipAddress[16]; // IPv4 address max length
	};
	DeferredWiFiEvent deferredWiFiEvent = {};

	constexpr size_t kUploadChunkSize = 4096;
	uint8_t uploadBuffer[kUploadChunkSize];

	SemaphoreHandle_t uploadMutex = nullptr;

	// Event queue for async event sending
#if defined(ECHO)
	constexpr size_t kEventQueueSize = 10;
	constexpr unsigned long kMaxEventAgeMs = 30000; // drop events older than 30s
#else
	constexpr size_t kEventQueueSize = 32;
#endif
	// sendConfigMessage() JSON must fit here; long Wi‑Fi SSID + full recorder config exceeded 512 and broke parsing server-side.
	constexpr size_t kMaxEventMessageLength = 1536;
	struct EventQueueEntry
	{
		char eventType[32];
		char eventMessage[kMaxEventMessageLength];
		bool hasSettings;
		char settingsJson[1024];
#if defined(ECHO)
		unsigned long enqueuedAtMs;
		uint32_t eventCounter;
#endif
	};
	QueueHandle_t eventQueue = nullptr;

#if defined(ECHO)
	static unsigned long g_eventDroppedTooOld = 0;
	static unsigned long g_eventDroppedNoTime = 0;
	static unsigned long g_eventDroppedQueueFull = 0;
	static uint32_t g_eventCounterSeq = 0;
#endif

	// Startup grace period for event send failures (suppress warnings for first 60 seconds)
	static unsigned long deviceStartupMs = 0;
	constexpr unsigned long kEventSendFailureGracePeriodMs = 60000; // 60 seconds

	// Circuit breaker constants
	constexpr uint32_t kCircuitBreakerFailureThreshold = 10;	// Open after 10 consecutive failures
	constexpr unsigned long kCircuitBreakerResetTimeMs = 60000; // Reset after 60 seconds
	constexpr float kMinHealthScoreForCircuitBreaker = 20.0f;	// Open if health score < 20

	constexpr uint8_t kApiFailureThreshold = 5;

	// Rate limiting for event sending failures
	unsigned long lastEventSendFailedLogMs = 0;					   // Last time event send failure was logged
	bool lastEventSendFailedState = false;						   // Previous state of event send failure
	constexpr unsigned long kEventErrorLogIntervalMs = 30000;	   // Log errors at most every 30 seconds
	constexpr unsigned long kEventStateChangeLogIntervalMs = 1000; // Log state changes immediately (1 second debounce)

	// Upload and event statistics tracking
	time_t lastUploadEpoch = 0;			   // Epoch time of the start time of the last file uploaded
	unsigned long totalUploadCount = 0;	   // Total number of successful uploads
	unsigned long totalUploadAttempts = 0; // Total number of upload attempts (successful + failed)
	unsigned long totalEventCount = 0;	   // Total number of events sent
	time_t lastEventEpoch = 0;			   // Epoch time of the last event sent

	// Simplified endpoint state - tracks failures, dead status, and recovery timing
	struct ApiEndpointState
	{
		const char *host = nullptr;
		uint8_t failureCount = 0;
		bool dead = false;
		unsigned long markedDeadAtMs = 0; // Timestamp when endpoint was marked dead (0 if not dead)
	};

	ApiEndpointState apiEndpointStates[kApiEndpointCount];
	bool apiEndpointsInitialized = false;

	// Initialize endpoint health metrics
	void initializeEndpointHealthMetrics()
	{
		for (size_t i = 0; i < kApiEndpointCount; ++i)
		{
			const char *configured = appSettings.upload.apiHosts[i];
			if (configured == nullptr || configured[0] == '\0')
			{
				configured = (i < (sizeof(kApiEndpoints) / sizeof(kApiEndpoints[0]))) ? kApiEndpoints[i] : nullptr;
			}
			g_endpointHealthMetrics[i].host = configured;
			g_endpointHealthMetrics[i].healthScore = 100.0f;
			g_endpointHealthMetrics[i].minResponseTimeMs = UINT32_MAX;
		}
	}

	void ensureUploadResources()
	{
		if (uploadMutex == nullptr)
		{
			uploadMutex = xSemaphoreCreateMutex();
			if (uploadMutex == nullptr)
			{
				Serial.println("Failed to create upload mutex");
			}
			s_uploadMutexForHeartbeat = uploadMutex;
		}
	}

	// Helper function to get User-Agent string (cached to avoid stack allocations)
	const char *getUserAgentString()
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

	static constexpr uint32_t kUploadChunksPerYield = 4u;

	// Exponential backoff between TCP retries; always brief cooldown after final failure (incl. maxRetries=1).
	static void delayAfterTcpConnectFailure(uint8_t attempt, uint8_t maxRetries)
	{
		esp_task_wdt_reset();
		uint32_t delayMs = 50u;
		if (attempt + 1u < maxRetries)
		{
			delayMs = 50u << attempt;
			if (delayMs > 500u)
			{
				delayMs = 500u;
			}
		}
		vTaskDelay(pdMS_TO_TICKS(delayMs > 0 ? delayMs : 1));
	}

	static void delayAfterEndpointFailure(size_t endpointIndex)
	{
		constexpr uint32_t kBaseBackoffMs = 50;
		const uint32_t backoffMs = network_calculateAdaptiveBackoff(endpointIndex, kBaseBackoffMs);
		esp_task_wdt_reset();
		vTaskDelay(pdMS_TO_TICKS(backoffMs > 0 ? backoffMs : 1));
	}

	// Helper function to connect WiFiClient with watchdog feeding and retry logic
	// Returns true if connection successful, false otherwise
	// maxRetries: Maximum number of connection attempts (default: 1 to avoid long blocking)
	// connectionTimeoutMs: Timeout per connection attempt in milliseconds (default: 3000)
	bool connectWiFiClientWithRetry(WiFiClient &client, const char *host, uint16_t port,
									uint8_t maxRetries = 1, unsigned long connectionTimeoutMs = 3000)
	{
		// Set shorter timeout to prevent long blocking calls (1 second max per attempt)
		client.setTimeout(connectionTimeoutMs);

		unsigned long connectionStartMs = millis();
		for (uint8_t attempt = 0; attempt < maxRetries; ++attempt)
		{
			// Feed watchdog before connection attempt
			esp_task_wdt_reset();

			unsigned long attemptStartMs = millis();
			// Attempt connection (this will block for up to connectionTimeoutMs)
			// Note: We can't feed watchdog during the blocking connect() call,
			// but we set a shorter timeout (1 second) to minimize the blocking duration
			errno = 0;
			bool connected = client.connect(host, port);
			const int savedErrno = errno;

			// Feed watchdog immediately after connection attempt
			esp_task_wdt_reset();

			if (connected)
			{
				logDebugf("[Network] Successfully connected to %s:%u (attempt %u/%u, elapsed: %lums)",
						  maskHostnameForLogging(host), port, attempt + 1, maxRetries,
						  millis() - attemptStartMs);
				network_recordCloudPathConnectResult(true);
				return true;
			}

			// Connection failed — write error is often 0 on failed TCP connect; errno is more informative.
			const int lastError = client.getWriteError();
			String errorMsg;
			if (lastError != 0)
			{
				errorMsg = "write_err=" + String(lastError);
			}
			else if (savedErrno != 0)
			{
				errorMsg = "errno=" + String(savedErrno);
			}
			else
			{
				errorMsg = "tcp_fail";
			}

			// Connection failed - log the failure immediately as error
			// Log every failure to catch all connection issues
			logErrorf("[Network] Connection error to %s:%u (attempt %u/%u, timeout: %lums) - %s",
					  maskHostnameForLogging(host), port, attempt + 1, maxRetries, connectionTimeoutMs, errorMsg.c_str());

			delayAfterTcpConnectFailure(attempt, maxRetries);
		}

		// All retries failed
		if (WiFi.isConnected() && WiFi.status() == WL_CONNECTED)
		{
			network_recordCloudPathConnectResult(false);
		}
		return false;
	}

	void ensureApiEndpoints()
	{
		if (apiEndpointsInitialized)
		{
			return;
		}

		for (size_t i = 0; i < kApiEndpointCount; ++i)
		{
			const char *configured = appSettings.upload.apiHosts[i];
			if (configured == nullptr || configured[0] == '\0')
			{
				configured = (i < (sizeof(kApiEndpoints) / sizeof(kApiEndpoints[0]))) ? kApiEndpoints[i] : nullptr;
			}
			apiEndpointStates[i].host = configured;
			apiEndpointStates[i].failureCount = 0;
			apiEndpointStates[i].dead = false;
			apiEndpointStates[i].markedDeadAtMs = 0;
		}

		apiEndpointsInitialized = true;
		initializeEndpointHealthMetrics();
	}

	void notifyApiFailure(const char *host, const String &reason, uint8_t failures, bool markedDead)
	{
		// Report dead endpoints to Serial port (always JSON format)
		if (markedDead)
		{
			String message = "Endpoint marked DEAD: ";
			message += (host ? host : "(null)");
			message += " (failures=";
			message += String(static_cast<unsigned>(failures));
			message += ")";
			Serial.print("{\"ty\":\"error\",\"ms\":\"");
			// Escape JSON special characters
			for (size_t i = 0; i < message.length(); ++i)
			{
				char c = message.charAt(i);
				if (c == '"' || c == '\\')
				{
					Serial.print('\\');
				}
				Serial.print(c);
			}
			Serial.print("\",\"mc\":\"");
			Serial.print(getDeviceId());
			Serial.print("\",\"si\":\"");
			Serial.print(getSessionId());
			Serial.println("\"}");
		}
		else
		{
		}
	}

	void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
	{
		logDebugf("[WiFi] Event received: %d", static_cast<int>(event));
		switch (event)
		{
		case ARDUINO_EVENT_WIFI_STA_CONNECTED:
			logDebugf("[WiFi] Station connected to AP");
			break;
		case ARDUINO_EVENT_WIFI_STA_GOT_IP:
		{
			// MINIMAL work in event handler to prevent stack overflow
			// Defer all logging and event sending to network_loop()
			deferredWiFiEvent.gotIp = true;
			IPAddress ip = WiFi.localIP();
			snprintf(deferredWiFiEvent.ipAddress, sizeof(deferredWiFiEvent.ipAddress),
					 "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
			wifiEverConnected = true;
			wifiDisconnectLogged = false;
			wifiConnectedAtMs = millis(); // Record when WiFi connected for event delay
			timeKeeper().notifyNetworkConnected();

			// Reset dead endpoints when WiFi reconnects to allow retry
			for (size_t i = 0; i < kApiEndpointCount; ++i)
			{
				if (apiEndpointStates[i].dead)
				{
					apiEndpointStates[i].dead = false;
					apiEndpointStates[i].failureCount = 0;
					apiEndpointStates[i].markedDeadAtMs = 0;
				}
			}
		}
		break;
		case ARDUINO_EVENT_WIFI_STA_LOST_IP:
			// Defer logging to network_loop()
			deferredWiFiEvent.lostIp = true;
			wifiConnectedAtMs = 0; // Reset connection timestamp
			break;
		case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
			// Defer logging to network_loop()
			logDebugf("[WiFi] Station disconnected (reason: %d)",
					  info.wifi_sta_disconnected.reason);
			wifiDisconnectLogged = false;
			wifiConnectedAtMs = 0; // Reset connection timestamp
			deferredWiFiEvent.disconnected = true;
			deferredWiFiEvent.disconnectReason = info.wifi_sta_disconnected.reason;
			break;
		case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
		{
			// Station connected to AP - print MAC address
			// In AP mode, logger will output plain text automatically
			uint8_t *mac = info.wifi_ap_staconnected.mac;
			Serial.printf("[AP] Client connected - MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
						  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
		break;
		case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
		{
			// Station disconnected from AP - print MAC address
			// In AP mode, logger will output plain text automatically
			uint8_t *mac = info.wifi_ap_stadisconnected.mac;
			Serial.printf("[AP] Client disconnected - MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
						  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
		break;
		default:
			break;
		}
	}

	// Fill outIndices with healthy endpoint indices in random order (Fisher-Yates shuffle).
	// When Custom (slot 3) is enabled and valid, use only slot 3 for all API. Otherwise use regions 0,1,2.
	void getHealthyEndpointIndicesRandomOrder(size_t *outIndices, size_t maxCount, size_t &outCount)
	{
		ensureApiEndpoints();
		outCount = 0;

		const ApiEndpointState &customEp = apiEndpointStates[3];
		const bool customValid = appSettings.upload.enabled[3] && customEp.host != nullptr && std::strlen(customEp.host) > 0 && getUploadPort(3) != 0;

		if (customValid)
		{
			if (maxCount > 0)
			{
				outIndices[0] = 3;
				outCount = 1;
			}
			return;
		}

		for (size_t i = 0; i < 3 && outCount < maxCount; ++i)
		{
			const ApiEndpointState &endpoint = apiEndpointStates[i];
			if (!appSettings.upload.enabled[i] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
				continue;
			uint16_t port = getUploadPort(i);
			if (port == 0)
				continue;
			outIndices[outCount++] = i;
		}
		for (size_t i = outCount; i > 1; --i)
		{
			size_t j = static_cast<size_t>(random(i));
			size_t tmp = outIndices[j];
			outIndices[j] = outIndices[i - 1];
			outIndices[i - 1] = tmp;
		}
	}

	bool cloudPathSecondaryWanProbeOk()
	{
		WiFiClient c;
		c.setTimeout(CLOUD_PATH_SECONDARY_PROBE_TIMEOUT_MS);
		esp_task_wdt_reset();
		const bool ok = c.connect(CLOUD_PATH_SECONDARY_PROBE_HOST, CLOUD_PATH_SECONDARY_PROBE_PORT);
		esp_task_wdt_reset();
		if (ok)
		{
			c.stop();
		}
		return ok;
	}

	void cloudPathTryIdleProbe()
	{
		if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
		{
			return;
		}
		ensureApiEndpoints();
		size_t idx = 0;
		const char *host = nullptr;
		uint16_t port = 0;
		if (!network_getRandomHealthyEndpoint(idx, host, port) || host == nullptr)
		{
			for (size_t i = 0; i < kApiEndpointCount; ++i)
			{
				if (!appSettings.upload.enabled[i])
				{
					continue;
				}
				const char *h = appSettings.upload.apiHosts[i];
				if (h == nullptr || h[0] == '\0')
				{
					h = (i < (sizeof(kApiEndpoints) / sizeof(kApiEndpoints[0]))) ? kApiEndpoints[i] : nullptr;
				}
				if (h == nullptr || h[0] == '\0')
				{
					continue;
				}
				host = h;
				port = getUploadPort(i);
				if (port == 0)
				{
					port = DEFAULT_API_PORT;
				}
				break;
			}
		}
		if (host == nullptr || host[0] == '\0')
		{
			return;
		}
		WiFiClient client;
		connectWiFiClientWithRetry(client, host, port, 1, 2000);
	}

	void cloudPathMaybeRecover(unsigned long nowMs)
	{
		if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
		{
			return;
		}
		if (g_cloudPathConsecutiveFailures < CLOUD_PATH_FAILURE_THRESHOLD)
		{
			return;
		}
		if (g_cloudPathLastRecoveryMs != 0 &&
			(nowMs - g_cloudPathLastRecoveryMs) < CLOUD_PATH_RECOVERY_MIN_INTERVAL_MS)
		{
			return;
		}
		if (g_cloudPathLastSuccessMs > 0)
		{
			if ((nowMs - g_cloudPathLastSuccessMs) < CLOUD_PATH_MIN_TIME_SINCE_LAST_SUCCESS_MS)
			{
				return;
			}
		}
		else
		{
			if (g_cloudPathBootMs > 0 && (nowMs - g_cloudPathBootMs) < CLOUD_PATH_BOOT_GRACE_MS)
			{
				return;
			}
		}

		if (cloudPathSecondaryWanProbeOk())
		{
			logInfof("[Network] Cloud path recovery skipped: secondary probe OK (WAN reachable; likely API outage)");
			g_cloudPathConsecutiveFailures = 0;
			return;
		}

		logEventf("[Network] Cloud path recovery: TCP failures to API with STA up — running WiFi reconnect\n");
		network_reconnectWiFi();
		g_cloudPathLastRecoveryMs = nowMs;
		g_cloudPathConsecutiveFailures = 0;
	}

	void cloudPathUpdateFullRebootDue(unsigned long nowMs)
	{
		if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED || g_cloudPathFullRebootDue)
		{
			return;
		}

		static unsigned long s_lastEvalMs = 0;
		if (s_lastEvalMs != 0 && (nowMs - s_lastEvalMs) < CLOUD_PATH_FULL_REBOOT_EVAL_INTERVAL_MS)
		{
			return;
		}
		s_lastEvalMs = nowMs;

		bool pastDeadline = false;
		if (g_cloudPathLastSuccessMs > 0)
		{
			pastDeadline = (nowMs - g_cloudPathLastSuccessMs) >= CLOUD_PATH_FULL_REBOOT_AFTER_MS;
		}
		else if (g_cloudPathBootMs > 0)
		{
			pastDeadline = (nowMs - g_cloudPathBootMs) >= CLOUD_PATH_FULL_REBOOT_AFTER_MS;
		}
		else
		{
			return;
		}

		if (!pastDeadline)
		{
			return;
		}

		if (cloudPathSecondaryWanProbeOk())
		{
			return;
		}

		g_cloudPathFullRebootDue = true;
		logErrorf("[Network] No successful cloud TCP for %lu min and WAN probe failed — full reboot after recording completes",
				  static_cast<unsigned long>(CLOUD_PATH_FULL_REBOOT_AFTER_MS / 60000UL));
	}

} // namespace

bool network_getRandomHealthyEndpoint(size_t &indexOut, const char *&hostOut, uint16_t &portOut)
{
	ensureApiEndpoints();

	if (appSettings.upload.enabled[3] && apiEndpointStates[3].host != nullptr && std::strlen(apiEndpointStates[3].host) > 0 && getUploadPort(3) != 0)
	{
		indexOut = 3;
		hostOut = apiEndpointStates[3].host;
		portOut = getUploadPort(3);
		return true;
	}

	struct Candidate
	{
		size_t index;
		const char *host;
		uint16_t port;
	};

	Candidate candidates[3];
	size_t candidateCount = 0;

	for (size_t i = 0; i < 3; ++i)
	{
		ApiEndpointState &endpoint = apiEndpointStates[i];
		const char *host = endpoint.host;

		if (!appSettings.upload.enabled[i] || endpoint.dead || host == nullptr || std::strlen(host) == 0)
		{
			continue;
		}

		uint16_t port = getUploadPort(i);
		if (port == 0)
		{
			port = DEFAULT_API_PORT;
		}

		candidates[candidateCount].index = i;
		candidates[candidateCount].host = host;
		candidates[candidateCount].port = port;
		++candidateCount;
	}

	if (candidateCount == 0)
	{
		return false;
	}

	size_t choice = (candidateCount == 1) ? 0 : static_cast<size_t>(random(candidateCount));

	indexOut = candidates[choice].index;
	hostOut = candidates[choice].host;
	portOut = candidates[choice].port;
	return true;
}

bool network_hasAnyWiFiCredentials()
{
	for (size_t idx = 0; idx < kMaxWifiCredentials; ++idx)
	{
		const char *ssid_c = appSettings.wifi[idx].ssid;
		if (ssid_c != nullptr && std::strlen(ssid_c) > 0)
		{
			return true;
		}
	}
	return false;
}

void network_begin()
{
	ensureUploadResources();
	apiEndpointsInitialized = false;

	// Record device startup time for grace period (only set once, on first call)
	static bool startupTimeSet = false;
	if (!startupTimeSet)
	{
		deviceStartupMs = millis();
		g_cloudPathBootMs = deviceStartupMs;
		startupTimeSet = true;
	}

	WiFi.disconnect(true, true);
	// Ensure WiFi is in station mode only (not AP or mixed mode)
	// This is critical when WiFi credentials are configured
	WiFi.mode(WIFI_STA);
	const String &hostname = getLocalHostname();
	if (!WiFi.setHostname(hostname.c_str()))
	{
		logWarnf("[WiFi] Failed to set DHCP hostname to %s", hostname.c_str());
	}
	WiFi.onEvent(handleWiFiEvent);

	// Initialize event queue and task if not already initialized
	if (eventQueue == nullptr)
	{
		eventQueue = xQueueCreate(kEventQueueSize, sizeof(EventQueueEntry));
		if (eventQueue == nullptr)
		{
		}
	}

	// Apply WiFi TX power setting
	uint8_t esp32TxPower = mapWifiTxPowerLevel(appSettings.wifiTxPower);
	esp_wifi_set_max_tx_power(esp32TxPower);

	// Initialize device ID (fetches MAC address once and stores it)
	const String &deviceId = getDeviceId();

	wifiEverConnected = false;
	wifiDisconnectLogged = false;

	// Note: AP mode is started from main.cpp when no credentials are detected
	// This function is only called when credentials exist
	connectToWiFi();
}

void network_loop()
{
	static unsigned long lastHeartbeatMs = 0;
	static unsigned long firstConnectedInElseMs = 0;

	// Process deferred WiFi events (moved from event handler to avoid stack overflow)
	if (deferredWiFiEvent.gotIp)
	{
		deferredWiFiEvent.gotIp = false;
		g_cloudPathConsecutiveFailures = 0;
		g_cloudPathFullRebootDue = false;
		const char *ipStr = deferredWiFiEvent.ipAddress;
		startMdnsResponder();
		logEventf("[WiFi] 📶 WiFi connected, IP: %s, Gateway: %s, Subnet: %s, DNS1: %s\n",
				  ipStr,
				  WiFi.gatewayIP().toString().c_str(),
				  WiFi.subnetMask().toString().c_str(),
				  WiFi.dnsIP().toString().c_str());

		{
			const String gwStr = WiFi.gatewayIP().toString();
			const String snStr = WiFi.subnetMask().toString();
			const String dnsStr = WiFi.dnsIP().toString();
			String summary = "[WiFi] 📶 WiFi connected, IP: ";
			summary += ipStr;
			summary += ", Gateway: ";
			summary += gwStr;
			summary += ", Subnet: ";
			summary += snStr;
			summary += ", DNS1: ";
			summary += dnsStr;
			wifiEmitStaTerminalSimple("wifi_connected", summary);
		}

		// Send online event with reset reason metadata
		{
			StaticJsonDocument<256> onlineDoc;
			onlineDoc["message"] = "Device started";
			onlineDoc["ip"] = ipStr;
			onlineDoc["resetReason"] = system_getResetReasonString();
			String eventMessage;
			serializeJson(onlineDoc, eventMessage);
			sendEvent("online", eventMessage);
		}

		// Best-effort CDN sync for system files (SPA + notification sounds).
		system_assets_syncFromCdnBestEffort();

		// Log endpoint resets
		for (size_t i = 0; i < kApiEndpointCount; ++i)
		{
			if (apiEndpointStates[i].host != nullptr)
			{
			}
		}
	}

	if (deferredWiFiEvent.lostIp)
	{
		deferredWiFiEvent.lostIp = false;
		MDNS.end();
		mdnsResponderRunning = false;
		lastMdnsStartAttemptMs = 0;
		logEventf("[WiFi] ✗ Lost IP address\n");
		wifiEmitStaTerminalSimple("wifi_lost_ip", "[WiFi] ✗ Lost IP address");
	}

	if (deferredWiFiEvent.disconnected)
	{
		deferredWiFiEvent.disconnected = false;
		MDNS.end();
		mdnsResponderRunning = false;
		lastMdnsStartAttemptMs = 0;
		int reasonCode = deferredWiFiEvent.disconnectReason;

		// Provide a human-readable description for the disconnect reason
		const char *reasonDesc = nullptr;
		switch (reasonCode)
		{
		case 1:
			reasonDesc = "General Network Error";
			break;
		case 2:
			reasonDesc = "Authentication Expired";
			break;
		case 3:
			reasonDesc = "Graceful Dissconnect";
			break;
		case 4:
			reasonDesc = "Session Expired";
			break;
		case 5:
			reasonDesc = "Router is full";
			break;
		case 6:
			reasonDesc = "Not logged in";
			break;
		case 7:
			reasonDesc = "WiFi Link Lost";
			break;
		case 8:
			reasonDesc = "Left the network";
			break;
		case 9:
			reasonDesc = "Router rejected request";
			break;
		case 10:
			reasonDesc = "Power Requirement mismatch";
			break;
		case 11:
			reasonDesc = "Router channel mismatch";
			break;
		case 12:
			reasonDesc = "Information mismatch";
			break;
		case 13:
			reasonDesc = "Security data corrupted";
			break;
		case 14:
			reasonDesc = "Wrong Password or weak signal";
			break;
		case 15:
			reasonDesc = "Group Key Update Timeout";
			break;
		case 16:
			reasonDesc = "IE In 4Way Differs";
			break;
		case 17:
			reasonDesc = "Group Cipher Invalid";
			break;
		case 18:
			reasonDesc = "Pairwise Cipher Invalid";
			break;
		case 19:
			reasonDesc = "AKMP Invalid";
			break;
		case 20:
			reasonDesc = "Unsupported RSN IE Version";
			break;
		case 23:
			reasonDesc = "IEEE 802.1X Auth Failed";
			break;
		case 24:
			reasonDesc = "Cipher Suite Rejected";
			break;
		case 201:
			reasonDesc = "Lost Connection. Out of range";
			break;
		case 202:
			reasonDesc = "Router Out of Range";
			break;
		case 203:
			reasonDesc = "Authentication failed";
			break;
		case 204:
			reasonDesc = "Failed to link";
			break;
		case 205:
			reasonDesc = "Connection too lsow";
			break;
		default:
			reasonDesc = "Unknown reason";
			break;
		}

		logWarnf("[WiFi] Disconnected from WiFi, reason code: %d (%s)\n", reasonCode, reasonDesc);
		logErrorf("[WiFi] ✗ Disconnected from WiFi (reason code: %d - %s)\n", reasonCode, reasonDesc);

		{
			String summary = "[WiFi] Disconnected from WiFi, reason code: ";
			summary += String(reasonCode);
			summary += " (";
			summary += reasonDesc;
			summary += ")";
			wifiEmitStaTerminalSimple("wifi_disconnected", summary);
		}
	}

	// Web server is now handled in its own dedicated task (webServerTask)
	// No need to call boondock_server_loop() here anymore

	// Check if we need to start AP mode (no credentials and AP not active)
	if (!network_hasAnyWiFiCredentials() && !boondock_server_isAPModeActive())
	{
		logInfof("[Network] No WiFi credentials - starting Access Point mode");
		boondock_server_startAPMode();
		return;
	}

	// Ensure WiFi is in station mode only when credentials are configured
	// This prevents mixed mode (AP+STA) after credentials are set
	// Only check when NOT connected to avoid disrupting active connections
	if (network_hasAnyWiFiCredentials() && !WiFi.isConnected() && WiFi.getMode() != WIFI_STA)
	{
		WiFi.mode(WIFI_STA);
		network_reinitializeWiFi();
	}

	// Enhanced WiFi auto-recovery with exponential backoff
	static unsigned long lastWiFiReconnectAttemptMs = 0;
	static uint32_t wifiReconnectAttemptCount = 0;
	constexpr unsigned long kMinWiFiReconnectIntervalMs = 5000;	  // Minimum 5 seconds between attempts
	constexpr unsigned long kMaxWiFiReconnectIntervalMs = 300000; // Max 5 minutes between attempts
	constexpr uint32_t kMaxWiFiReconnectAttempts = 20;			  // Max attempts before giving up for a while

	if (!WiFi.isConnected())
	{
		firstConnectedInElseMs = 0; // Reset so next connect we wait before first heartbeat
		// Check if we have WiFi credentials configured
		if (!network_hasAnyWiFiCredentials())
		{
			// No credentials configured - AP mode should be active (handled by boondock_server)
			return;
		}

		if (wifiEverConnected && !wifiDisconnectLogged)
		{
			logEventf("[WiFi] ✗ Connection lost, attempting reconnect\n");
			wifiDisconnectLogged = true;
		}

		// Enhanced auto-recovery with exponential backoff
		const unsigned long now = millis();
		unsigned long reconnectInterval = kMinWiFiReconnectIntervalMs;

		// Calculate exponential backoff
		if (wifiReconnectAttemptCount > 0)
		{
			reconnectInterval = kMinWiFiReconnectIntervalMs * (1UL << (wifiReconnectAttemptCount < 6 ? wifiReconnectAttemptCount : 6));
			if (reconnectInterval > kMaxWiFiReconnectIntervalMs)
			{
				reconnectInterval = kMaxWiFiReconnectIntervalMs;
			}
		}

		// Attempt reconnect if enough time has passed
		if (lastWiFiReconnectAttemptMs == 0 || (now - lastWiFiReconnectAttemptMs) >= reconnectInterval)
		{
			if (wifiReconnectAttemptCount < kMaxWiFiReconnectAttempts)
			{
				logDebugf("[WiFi] Reconnection attempt %u/%u (backoff: %lums)",
						  wifiReconnectAttemptCount + 1, kMaxWiFiReconnectAttempts, reconnectInterval);
				lastWiFiReconnectAttemptMs = now;
				wifiReconnectAttemptCount++;
				connectToWiFi();
				logDebugf("[WiFi] Reconnection attempt completed (status: %d)", WiFi.status());
			}
			else
			{
				// Too many attempts, wait longer before retrying
				if ((now - lastWiFiReconnectAttemptMs) >= kMaxWiFiReconnectIntervalMs * 2)
				{
					logDebugf("[WiFi] Resetting reconnection counter after extended wait");
					// Reset attempt count after extended wait
					wifiReconnectAttemptCount = 0;
					lastWiFiReconnectAttemptMs = 0;
				}
			}
		}
	}
	else
	{
		maintainMdnsResponder();

		// WiFi is connected - reset reconnect attempt counter
		if (wifiReconnectAttemptCount > 0)
		{
			wifiReconnectAttemptCount = 0;
			lastWiFiReconnectAttemptMs = 0;
		}

		// Periodic heartbeat to server (once per minute, when not busy with upload)
		unsigned long now = millis();
		if (firstConnectedInElseMs == 0)
		{
			firstConnectedInElseMs = now;
		}
		constexpr unsigned long kHeartbeatIntervalMs = 60000;
		constexpr unsigned long kHeartbeatDelayAfterConnectMs = 3000;
		bool intervalElapsed = (lastHeartbeatMs == 0 || (now - lastHeartbeatMs >= kHeartbeatIntervalMs));
		bool pastConnectDelay = (now - firstConnectedInElseMs >= kHeartbeatDelayAfterConnectMs);
		if (intervalElapsed && pastConnectDelay && isUploadMutexFree())
		{
			RecordingStats sessionStats = recorder_getSessionStats();
			SystemHealthMetrics metrics = health_getHealthMetrics();
			DynamicJsonDocument heartbeatDoc(512);
			heartbeatDoc["message"] = "Ping";
			JsonObject health = heartbeatDoc.createNestedObject("health");
			health["tm"] = getFormattedTimeWithTimezone();
			health["rc"] = sessionStats.recordingCount;
			health["uc"] = sessionStats.uploadedCount;
			health["pq"] = system_getUploadQueueSize();
			health["td"] = static_cast<int>(sessionStats.totalDurationMs / 1000);
			health["am"] = metrics.apiMinResponseTimeMs;
			health["ax"] = metrics.apiMaxResponseTimeMs;
			health["aa"] = metrics.apiAverageResponseTimeMs;
			health["si"] = getSessionId();
			String heartbeatJson;
			serializeJson(heartbeatDoc, heartbeatJson);
			sendEvent("ping", heartbeatJson);
			lastHeartbeatMs = now;
		}

		// Cloud path: optional idle TCP probe; recover WiFi when API TCP fails but STA stays up.
		// Skip while MQTT play_cloud / play_transmit (or live-audio upload pause) holds the network idle on purpose.
		if (!cloudPathMonitoringPaused())
		{
			const unsigned long nowCp = millis();
			bool staleNoSuccess = false;
			if (g_cloudPathLastSuccessMs == 0)
			{
				staleNoSuccess = (g_cloudPathBootMs > 0 && (nowCp - g_cloudPathBootMs) >= CLOUD_PATH_IDLE_NO_SUCCESS_MS);
			}
			else
			{
				staleNoSuccess = (nowCp - g_cloudPathLastSuccessMs) >= CLOUD_PATH_IDLE_NO_SUCCESS_MS;
			}
			if (staleNoSuccess &&
				(g_cloudPathLastIdleProbeMs == 0 || (nowCp - g_cloudPathLastIdleProbeMs) >= CLOUD_PATH_PROBE_INTERVAL_MS))
			{
				cloudPathTryIdleProbe();
				g_cloudPathLastIdleProbeMs = nowCp;
			}
			cloudPathMaybeRecover(nowCp);
			cloudPathUpdateFullRebootDue(nowCp);
		}
	}
}

bool isWiFiConnected()
{
	return WiFi.isConnected();
}

// Connectivity verification removed - simplified for reliability

static char g_lastUploadFailureReason[64] = {0};

static void setUploadFailureReason(const char *r)
{
	if (r == nullptr || *r == '\0')
	{
		g_lastUploadFailureReason[0] = '\0';
		return;
	}
	strncpy(g_lastUploadFailureReason, r, sizeof(g_lastUploadFailureReason) - 1);
	g_lastUploadFailureReason[sizeof(g_lastUploadFailureReason) - 1] = '\0';
}

const char *network_getLastUploadFailureReason()
{
	return g_lastUploadFailureReason;
}

bool uploadAudioFile(const UploadRequest &request, bool sendTags)
{
	ensureUploadResources();
	g_lastUploadFailureReason[0] = '\0';

	if (!WiFi.isConnected())
	{
		setUploadFailureReason("wifi");
		// Don't send event here - WiFi is not connected, so event would fail anyway
		return false;
	}

	ensureApiEndpoints();

	if (uploadMutex != nullptr)
	{
		g_mutexMetrics.totalAttempts++;
		unsigned long mutexStartMs = millis();
		if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
		{
			g_mutexMetrics.timeoutCount++;
			g_mutexMetrics.lastTimeoutMs = millis();
			g_mutexMetrics.consecutiveTimeouts++;

			// Calculate timeout rate
			if (g_mutexMetrics.totalAttempts > 0)
			{
				g_mutexMetrics.timeoutRate = static_cast<float>(g_mutexMetrics.timeoutCount) /
											 static_cast<float>(g_mutexMetrics.totalAttempts);
			}

			// Alert if timeout rate > 10%
			if (g_mutexMetrics.timeoutRate > 0.10f && g_mutexMetrics.totalAttempts >= 10)
			{
				logErrorf("[Upload] Mutex timeout rate high: %.1f%% (%u/%u)\n",
						  g_mutexMetrics.timeoutRate * 100.0f,
						  static_cast<unsigned>(g_mutexMetrics.timeoutCount),
						  static_cast<unsigned>(g_mutexMetrics.totalAttempts));
			}

			sendEvent("audio_upload_skipped", "{\"reason\":\"busy\"}");
			setUploadFailureReason("mutex_timeout");
			return false;
		}
		else
		{
			g_mutexMetrics.successCount++;
			g_mutexMetrics.consecutiveTimeouts = 0;

			// Calculate timeout rate
			if (g_mutexMetrics.totalAttempts > 0)
			{
				g_mutexMetrics.timeoutRate = static_cast<float>(g_mutexMetrics.timeoutCount) /
											 static_cast<float>(g_mutexMetrics.totalAttempts);
			}
		}
	}

	bool overallResult = false;
	String sourcePath = String(request.path);
	size_t fileSize = request.fileSize;

	// PSRAM mode: data is already in memory, no file to open
	if (request.isPsramMode && request.psramData != nullptr)
	{
		fileSize = request.fileSize; // Use fileSize from request
	}
	else
	{
		// SD card mode: open file
		if (!isStorageModeSdCard())
		{
			setUploadFailureReason("sd_unavailable");
			return false;
		}
		File fileProbe = SD_MMC.open(sourcePath, FILE_READ);
		if (!fileProbe)
		{
			// Record storage read error
			extern void storage_recordReadError();
			storage_recordReadError();
		}
		if (!fileProbe)
		{
			// File might have been renamed after time sync (unsynced_*.wav -> timestamp.wav)
			// Try to find the renamed file by recalculating the path from recordedAtMs
			if (sourcePath.indexOf("/unsynced_") >= 0 && request.recordedAtMs > 0)
			{
				// Calculate the corrected epoch from recordedAtMs
				time_t correctedEpoch = calculateEpochFromMillis(request.recordedAtMs);
				if (isEpochValid(correctedEpoch))
				{
					// Generate the new path based on corrected timestamp
					struct tm timeinfo;
					gmtime_r(&correctedEpoch, &timeinfo);
					char timePart[32];
					strftime(timePart, sizeof(timePart), "%Y-%m-%d-%H-%M-%S.wav", &timeinfo);

					// Determine base directory (could be /queue or /inbox)
					String baseDir = "/queue";
					if (sourcePath.startsWith("/inbox"))
					{
						baseDir = "/inbox";
						char dirPath[64];
						strftime(dirPath, sizeof(dirPath), "/inbox/%Y/%m/%d", &timeinfo);
						baseDir = String(dirPath);
					}

					String newPath = baseDir + "/" + String(timePart);

					// Try the new path
					File newFileProbe = SD_MMC.open(newPath, FILE_READ);
					if (newFileProbe)
					{
						sourcePath = newPath;
						fileSize = newFileProbe.size();
						newFileProbe.close();
					}
					else
					{
						// New path doesn't exist either, try to find any file in /queue with matching size
						// This is a fallback in case the rename happened differently
						if (isStorageModeSdCard() && SD_MMC.exists("/queue"))
						{
							File queueDir = SD_MMC.open("/queue");
							if (queueDir && queueDir.isDirectory())
							{
								bool found = false;
								while (true)
								{
									File entry = queueDir.openNextFile();
									if (!entry)
									{
										break;
									}
									if (!entry.isDirectory())
									{
										String entryPath = String(entry.name());
										if (!entryPath.startsWith("/"))
										{
											entryPath = "/queue/" + entryPath;
										}
										// Skip marker files
										if (!entryPath.endsWith(".uploaded") && entryPath.endsWith(".wav"))
										{
											size_t entrySize = entry.size();
											entry.close();
											// Check if size matches (within reasonable tolerance)
											if (entrySize > 0 && entrySize == request.fileSize)
											{
												sourcePath = entryPath;
												fileSize = entrySize;
												found = true;
												break;
											}
										}
										else
										{
											entry.close();
										}
									}
									else
									{
										entry.close();
									}
								}
								queueDir.close();
								if (found)
								{
									// Found a matching file, continue
								}
								else
								{
									sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
									setUploadFailureReason("open_failed");
									if (uploadMutex != nullptr)
									{
										xSemaphoreGive(uploadMutex);
									}
									return false;
								}
							}
							else
							{
								queueDir.close();
								sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
								setUploadFailureReason("open_failed");
								if (uploadMutex != nullptr)
								{
									xSemaphoreGive(uploadMutex);
								}
								return false;
							}
						}
						else
						{
							sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
							setUploadFailureReason("open_failed");
							if (uploadMutex != nullptr)
							{
								xSemaphoreGive(uploadMutex);
							}
							return false;
						}
					}
				}
				else
				{
					sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
					setUploadFailureReason("open_failed");
					if (uploadMutex != nullptr)
					{
						xSemaphoreGive(uploadMutex);
					}
					return false;
				}
			}
			else
			{
				sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
				setUploadFailureReason("open_failed");
				if (uploadMutex != nullptr)
				{
					xSemaphoreGive(uploadMutex);
				}
				return false;
			}
		}
		else
		{
			fileSize = fileProbe.size();
			fileProbe.close();
		}
	}

	const String boundary = "----BoondockBoundary";
	const String lineBreak = "\r\n";

	const String &deviceId = getDeviceId();

	String uploadFileName = sourcePath.substring(sourcePath.lastIndexOf('/') + 1);
	if (uploadFileName.isEmpty())
	{
		uploadFileName = "audio.wav";
	}

	const String isoTimestamp = formatIsoTimestamp(request.recordedAtEpoch, request.recordedAtMs);
	// convertToMp3 will be set per-endpoint in the loop below

	// These parts are common for all endpoints
	String macPart = "--" + boundary + lineBreak +
					 "Content-Disposition: form-data; name=\"mac_address\"" + lineBreak + lineBreak +
					 deviceId + lineBreak;

	String tagsPart;
	if (sendTags)
	{
		DynamicJsonDocument tagsDoc(1024);
		JsonObject recorderObject = tagsDoc.createNestedObject("recorder");
		recorderObject["id"] = deviceId;
		recorderObject["trigger"] = 1;
		if (std::strlen(request.endReason) > 0)
		{
			recorderObject["endReason"] = request.endReason;
		}
		recorderObject["duration"] = static_cast<uint32_t>(request.durationMs / 1000U);
		recorderObject["durationMs"] = request.durationMs;
		recorderObject["size"] = static_cast<uint32_t>(fileSize);
		recorderObject["dataBytes"] = static_cast<uint32_t>(request.sizeBytes);
		recorderObject["timestamp"] = isoTimestamp;
		recorderObject["path"] = uploadFileName;
		if (request.peakDb > -120.0f)
		{
			recorderObject["decibel"] = request.peakDb;
		}

		JsonObject dockObject = tagsDoc.createNestedObject("dock");
		dockObject["id"] = deviceId;

		JsonObject userObject = tagsDoc.createNestedObject("user");
		userObject["name"] = deviceId;
		userObject["ip"] = WiFi.localIP().toString();

		String tagsJson;
		serializeJson(tagsDoc, tagsJson);

		tagsPart = "--" + boundary + lineBreak +
				   "Content-Disposition: form-data; name=\"tags\"" + lineBreak + lineBreak +
				   tagsJson + lineBreak;
	}

	String fileHeader = "--" + boundary + lineBreak +
						"Content-Disposition: form-data; name=\"audio_file\"; filename=\"" + uploadFileName + "\"" + lineBreak +
						"Content-Type: audio/wav" + lineBreak + lineBreak;

	String closing = lineBreak + "--" + boundary + "--" + lineBreak;

	// Limit total time for all endpoint attempts to prevent watchdog timeout
	const unsigned long uploadStartMs = millis();
	constexpr unsigned long kMaxTotalUploadTimeMs = UPLOAD_TOTAL_TIMEOUT_MS;

	size_t order[kApiEndpointCount];
	size_t orderCount = 0;
	getHealthyEndpointIndicesRandomOrder(order, kApiEndpointCount, orderCount);

	String lastAttemptError;
	for (size_t i = 0; i < orderCount; ++i)
	{
		size_t idx = order[i];
		// Check if we've exceeded maximum total time
		if ((millis() - uploadStartMs) >= kMaxTotalUploadTimeMs)
		{
			logWarnf("[Upload] Maximum upload time exceeded (%lums), aborting endpoint attempts\n", kMaxTotalUploadTimeMs);
			esp_task_wdt_reset();
			break;
		}

		// Feed watchdog periodically during endpoint loop
		esp_task_wdt_reset();

		ApiEndpointState &endpoint = apiEndpointStates[idx];
		if (!appSettings.upload.enabled[idx] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}

		// Check circuit breaker
		if (network_isCircuitBreakerOpen(idx))
		{
			continue;
		}

		// Get per-endpoint settings
		uint16_t endpointPort = getUploadPort(idx);
		if (endpointPort == 0)
		{
			// Port must be specified - skip this endpoint
			continue;
		}

		const unsigned long attemptStartMs = millis();

		File audioFile;
		if (!request.isPsramMode)
		{
			// SD card mode: open file (use updated sourcePath if file was renamed)
			if (!isStorageModeSdCard())
			{
				break;
			}
			audioFile = SD_MMC.open(sourcePath, FILE_READ);
			if (!audioFile)
			{
				sendEvent("audio_upload_failed", "{\"reason\":\"file_open\",\"path\":\"" + sourcePath + "\"}");
				setUploadFailureReason("open_failed");
				break;
			}
		}

		WiFiClient *clientPtr = &g_networkSharedWiFiClient;
		clientPtr->stop();
		// Always convert to MP3
		bool convertToMp3 = true;

		// Generate convertMp3Part per-endpoint
		String convertMp3Part = "--" + boundary + lineBreak +
								"Content-Disposition: form-data; name=\"convert_to_mp3\"" + lineBreak + lineBreak +
								(convertToMp3 ? "true" : "false") + lineBreak;

		// Calculate content length for this endpoint
		const size_t contentLength = convertMp3Part.length() + macPart.length() +
									 (sendTags ? tagsPart.length() : 0) +
									 fileHeader.length() + closing.length() + fileSize;

		bool attemptSuccess = true;
		String attemptError;

		// Feed watchdog before connection attempt
		esp_task_wdt_reset();

		// Log API endpoint attempt with endpoint URL and file info
		String endpointUrl = String("http://") + String(endpoint.host) + ":" + String(endpointPort) + String(DEFAULT_AUDIO_UPLOAD_PATH);
		logDebugf("[Upload] File: %s, Size: %lu bytes, Duration: %lu ms\n",
				  sourcePath.c_str(), static_cast<unsigned long>(fileSize), static_cast<unsigned long>(request.durationMs));

		// Use single attempt with 3 second timeout to allow server time to accept connection
		// Multiple endpoints will be tried, so we don't need retries per endpoint
		if (!connectWiFiClientWithRetry(*clientPtr, endpoint.host, endpointPort, 1, 3000))
		{
			attemptSuccess = false;
			attemptError = "upload_server_unreachable";
			// Error already logged in helper function
		}
		else
		{
			String uploadUrl = String(DEFAULT_AUDIO_UPLOAD_PATH);
			String hostHeader = String(endpoint.host) + ":" + String(endpointPort);

			String requestLine = "POST " + uploadUrl + " HTTP/1.1";
			String headers = "Host: " + hostHeader + "\r\n"
													 "User-Agent: " +
							 getUserAgentString() + "\r\n"
													"Accept: application/json\r\n"
													"Content-Type: multipart/form-data; boundary=" +
							 boundary + "\r\n"
										"Content-Length: " +
							 String(static_cast<unsigned long>(contentLength)) + "\r\n"
																				 "Connection: close\r\n";

			clientPtr->print(requestLine + "\r\n");
			clientPtr->print(headers);
			clientPtr->print("\r\n");

			clientPtr->print(convertMp3Part);
			clientPtr->print(macPart);
			if (sendTags)
			{
				clientPtr->print(tagsPart);
			}
			clientPtr->print(fileHeader);

			// Upload audio data
			unsigned long uploadStartMs = millis();
			if (request.isPsramMode && request.psramData != nullptr)
			{
				// PSRAM mode: upload from memory buffer
				size_t remaining = fileSize;
				const uint8_t *dataPtr = request.psramData;
				uint32_t chunkCount = 0;
				uint32_t yieldCount = 0;
				unsigned long lastWatchdogFeedMs = millis();
				while (remaining > 0)
				{
					size_t chunkSize = (remaining > sizeof(uploadBuffer)) ? sizeof(uploadBuffer) : remaining;
					std::memcpy(uploadBuffer, dataPtr, chunkSize);
					clientPtr->write(uploadBuffer, chunkSize);
					dataPtr += chunkSize;
					remaining -= chunkSize;

					if ((++yieldCount & (kUploadChunksPerYield - 1u)) == 0u)
					{
						vTaskDelay(1);
					}

					// Feed watchdog more frequently: every 20 chunks OR every 500ms (whichever comes first)
					unsigned long now = millis();
					if (++chunkCount >= 20 || (now - lastWatchdogFeedMs) >= 500)
					{
						esp_task_wdt_reset();
						chunkCount = 0;
						lastWatchdogFeedMs = now;
					}

					// Safety timeout: if upload takes too long, abort (see UPLOAD_BODY_TIMEOUT_MS)
					if ((now - uploadStartMs) > UPLOAD_BODY_TIMEOUT_MS)
					{
						logWarnf("[Upload] Upload timeout after %lu seconds, aborting\n", static_cast<unsigned long>(UPLOAD_BODY_TIMEOUT_MS / 1000));
						break;
					}
				}
			}
			else
			{
				// SD card mode: upload from file
				size_t bytesRead = 0;
				uint32_t chunkCount = 0;
				uint32_t yieldCount = 0;
				unsigned long lastWatchdogFeedMs = millis();
				while ((bytesRead = audioFile.read(uploadBuffer, sizeof(uploadBuffer))) > 0)
				{
					clientPtr->write(uploadBuffer, bytesRead);

					if ((++yieldCount & (kUploadChunksPerYield - 1u)) == 0u)
					{
						vTaskDelay(1);
					}

					// Feed watchdog more frequently: every 20 chunks OR every 500ms (whichever comes first)
					unsigned long now = millis();
					if (++chunkCount >= 20 || (now - lastWatchdogFeedMs) >= 500)
					{
						esp_task_wdt_reset();
						chunkCount = 0;
						lastWatchdogFeedMs = now;
					}

					// Safety timeout: if upload takes too long, abort (see UPLOAD_BODY_TIMEOUT_MS)
					if ((now - uploadStartMs) > UPLOAD_BODY_TIMEOUT_MS)
					{
						logWarnf("[Upload] Upload timeout after %lu seconds, aborting\n", static_cast<unsigned long>(UPLOAD_BODY_TIMEOUT_MS / 1000));
						break;
					}
				}
				// Check for read errors (bytesRead == 0 but file not at end)
				if (bytesRead == 0 && audioFile.available() > 0)
				{
					extern void storage_recordReadError();
					storage_recordReadError();
				}
				if (audioFile)
				{
					audioFile.close();
				}
			}

			clientPtr->print(closing);

			unsigned long responseStartMs = millis();
			unsigned long responseTimer = responseStartMs;
			String response;
			uint32_t responseLoopCount = 0;
			unsigned long lastWatchdogFeedMs = millis();
			constexpr unsigned long kMaxResponseWaitMs = UPLOAD_RESPONSE_WAIT_MS;

			while ((millis() - responseStartMs) < kMaxResponseWaitMs)
			{
				// Feed watchdog more frequently: every 10 iterations OR every 500ms (whichever comes first)
				unsigned long now = millis();
				if (++responseLoopCount >= 10 || (now - lastWatchdogFeedMs) >= 500)
				{
					esp_task_wdt_reset();
					responseLoopCount = 0;
					lastWatchdogFeedMs = now;
				}

				uint32_t readLoopCount = 0;
				while (clientPtr->available())
				{
					char c = static_cast<char>(clientPtr->read());
					response += c;
					responseTimer = millis();

					// Feed watchdog during long reads (every 1000 bytes)
					if (++readLoopCount >= 1000)
					{
						esp_task_wdt_reset();
						readLoopCount = 0;
					}

					// Safety: don't let response grow too large
					if (response.length() > 10000)
					{
						logWarnf("[Upload] Response too large, truncating\n");
						break;
					}
				}

				if (!clientPtr->connected() && clientPtr->available() == 0)
				{
					break;
				}

				delay(10);
			}

			if (response.isEmpty())
			{
				attemptSuccess = false;
				attemptError = "empty_response";
				logWarnf("[Upload] Empty response from %s:%u\n", endpoint.host, endpointPort);
			}
			else
			{
				int statusCode = 0;
				int firstSpace = response.indexOf(' ');
				if (firstSpace >= 0)
				{
					int secondSpace = response.indexOf(' ', firstSpace + 1);
					if (secondSpace > firstSpace)
					{
						statusCode = response.substring(firstSpace + 1, secondSpace).toInt();
					}
				}

				const int bodyIndex = response.indexOf("\r\n\r\n");
				String body = bodyIndex >= 0 ? response.substring(bodyIndex + 4) : "";
				if (statusCode < 200 || statusCode >= 300)
				{
					attemptSuccess = false;
					attemptError = statusCode == 0 ? "invalid_status" : String("http_") + statusCode;
					logErrorf("[Upload] HTTP error from %s:%u - Status: %d\n", endpoint.host, endpointPort, statusCode);
				}
				else
				{
					String analysis = analyzeServerResponse(body);
					if (analysis != "OK")
					{
						attemptSuccess = false;
						attemptError = analysis;
						logErrorf("[Upload] Server response error from %s:%u - %s\n", endpoint.host, endpointPort, analysis.c_str());
					}
				}
				if (!body.isEmpty())
				{
					syncClockFromApiResponse(body);
				}
			}
		}

		if (clientPtr != nullptr)
		{
			clientPtr->stop();
		}
		if (!request.isPsramMode)
		{
			audioFile.close();
		}

		// Record endpoint request metrics
		unsigned long responseTimeMs = millis() - attemptStartMs;
		network_recordEndpointRequest(idx, attemptSuccess, responseTimeMs);

		if (attemptSuccess)
		{
			endpoint.failureCount = 0;
			// Reset dead flag on successful upload - connectivity restored
			if (endpoint.dead)
			{
				// Endpoint recovered via successful upload
				// Send recovery message in JSON format
				String message = "Endpoint recovered: ";
				message += (endpoint.host ? endpoint.host : "(null)");
				message += " (successful upload)";
				Serial.print("{\"ty\":\"info\",\"ms\":\"");
				// Escape JSON special characters
				for (size_t i = 0; i < message.length(); ++i)
				{
					char c = message.charAt(i);
					if (c == '"' || c == '\\')
					{
						Serial.print('\\');
					}
					Serial.print(c);
				}
				Serial.print("\",\"mc\":\"");
				Serial.print(getDeviceId());
				Serial.print("\",\"si\":\"");
				Serial.print(getSessionId());
				Serial.println("\"}");
				endpoint.dead = false;
				endpoint.markedDeadAtMs = 0;
			}
			// Reset circuit breaker on success
			if (g_endpointHealthMetrics[idx].circuitBreakerOpen)
			{
				network_resetCircuitBreaker(idx);
			}
			// overallResult = true;

			// Build index record with upload metadata
			UploadIndexRecord indexRecord;
			indexRecord.durationMs = request.durationMs;
			indexRecord.peakDb = request.peakDb;
			indexRecord.sizeBytes = request.sizeBytes;
			indexRecord.fileSize = fileSize;
			strncpy(indexRecord.endReason, request.endReason, sizeof(indexRecord.endReason) - 1);
			strncpy(indexRecord.timestamp, isoTimestamp.c_str(), sizeof(indexRecord.timestamp) - 1);

			// Set upload completion time
			time_t uploadNow;
			time(&uploadNow);
			indexRecord.uploadedAtEpoch = uploadNow;
			String uploadedAtStr = formatIsoTimestamp(uploadNow, millis());
			strncpy(indexRecord.uploadedAtTimestamp, uploadedAtStr.c_str(), sizeof(indexRecord.uploadedAtTimestamp) - 1);

			/*
			// Mark entry as uploaded and append to index
			uploadQueue_markUploadedWithRecord(request.path, indexRecord);
			recorder_incrementUploadedCount();
			*/

			if (!uploadQueue_markUploadedWithRecord(request.path, indexRecord))
			{
				logWarnf("[Upload] Server OK but pending move failed (retry): %s\n", request.path);
				lastAttemptError = "pending_move_failed";
				break;
			}
			overallResult = true;
			recorder_incrementUploadedCount();

			// Track last upload time and increment upload count
			if (isEpochValid(request.recordedAtEpoch))
			{
				lastUploadEpoch = request.recordedAtEpoch;
			}
			else
			{
				// Fallback to current time if epoch is invalid
				time(&lastUploadEpoch);
			}
			totalUploadCount++;

			unsigned long uploadElapsedMs = millis() - attemptStartMs;
			if (uploadElapsedMs == 0)
			{
				uploadElapsedMs = 1;
			}
			double uploadSpeedX = 0.0;
			if (request.durationMs > 0)
			{
				uploadSpeedX = static_cast<double>(request.durationMs) / static_cast<double>(uploadElapsedMs);
			}
			logEventf("[Upload] ✅ Sent '%s' size=%lu bytes speed=%.2fx (elapsed %lums)\n",
					  uploadFileName.c_str(),
					  static_cast<unsigned long>(fileSize),
					  uploadSpeedX,
					  static_cast<unsigned long>(uploadElapsedMs));

			// Send upload success event
			DynamicJsonDocument uploadEventData(512);
			uploadEventData["message"] = "Audio file successfully uploaded";
			uploadEventData["filename"] = uploadFileName;
			uploadEventData["size_bytes"] = static_cast<unsigned long>(fileSize);
			uploadEventData["duration_ms"] = request.durationMs;
			uploadEventData["peak_db"] = request.peakDb;
			uploadEventData["speed_x"] = uploadSpeedX;
			String uploadEventMessage;
			serializeJson(uploadEventData, uploadEventMessage);
			sendEvent("audio_upload_success", uploadEventMessage);

			// Print upload success event to terminal in JSON format
			StaticJsonDocument<512> terminalDoc;
			terminalDoc["tm"] = getFormattedTimeWithTimezone();
			terminalDoc["ty"] = "event";
			terminalDoc["ms"] = "[Upload] ✅ Audio file successfully uploaded";
			terminalDoc["ev"] = "audio_upload_success";
			terminalDoc["file"] = uploadFileName;
			terminalDoc["size"] = static_cast<unsigned long>(fileSize);
			terminalDoc["dur"] = request.durationMs;
			if (request.peakDb > -120.0f)
			{
				terminalDoc["db"] = request.peakDb;
			}
			terminalDoc["speed"] = uploadSpeedX;
			terminalDoc["elapsed"] = static_cast<unsigned long>(uploadElapsedMs);
			terminalDoc["mc"] = getDeviceId();
			terminalDoc["si"] = getSessionId();
			String terminalJson;
			serializeJson(terminalDoc, terminalJson);
			serialWriteJsonAtomic(terminalJson);

			break;
		}

		endpoint.failureCount++;

		// Update health score and check circuit breaker
		network_updateEndpointHealthScore(idx);
		if (g_endpointHealthMetrics[idx].circuitBreakerOpen)
		{
		}

		bool markedDead = false;
		if (endpoint.failureCount >= kApiFailureThreshold)
		{
			endpoint.dead = true;
			endpoint.markedDeadAtMs = millis(); // Record when endpoint was marked dead
			markedDead = true;
			// Report dead endpoint using logger system
			logErrorf("[Upload] Endpoint marked DEAD: %s (failures=%u)\n",
					  endpoint.host ? endpoint.host : "(null)",
					  static_cast<unsigned>(endpoint.failureCount));
		}

		if (attemptError.isEmpty())
		{
			attemptError = "unknown";
		}
		lastAttemptError = attemptError;
		notifyApiFailure(endpoint.host, attemptError, endpoint.failureCount, markedDead);
		if (i + 1 < orderCount)
		{
			delayAfterEndpointFailure(idx);
		}
	}

	if (uploadMutex != nullptr)
	{
		xSemaphoreGive(uploadMutex);
	}

	if (!overallResult)
	{
		if (lastAttemptError.length() > 0)
		{
			logErrorf("[Upload] Unable to upload audio recording '%s' (all endpoints failed: %s)\n", sourcePath.c_str(), lastAttemptError.c_str());
		}
		else
		{
			logErrorf("[Upload] Unable to upload audio recording '%s' (all endpoints failed)\n", sourcePath.c_str());
		}
	}

	if (overallResult)
	{
		setUploadFailureReason(nullptr);
	}
	else if (g_lastUploadFailureReason[0] == '\0')
	{
		if (lastAttemptError.length() > 0)
		{
			setUploadFailureReason(lastAttemptError.c_str());
		}
		else
		{
			setUploadFailureReason("all_endpoints_failed");
		}
	}

	return overallResult;
}

bool uploadLogFile(const String &logFilePath)
{
	ensureUploadResources();

	if (!WiFi.isConnected())
	{
		return false;
	}

	ensureApiEndpoints();

	if (uploadMutex != nullptr)
	{
		g_mutexMetrics.totalAttempts++;
		if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
		{
			g_mutexMetrics.timeoutCount++;
			g_mutexMetrics.lastTimeoutMs = millis();
			g_mutexMetrics.consecutiveTimeouts++;

			// Calculate timeout rate
			if (g_mutexMetrics.totalAttempts > 0)
			{
				g_mutexMetrics.timeoutRate = static_cast<float>(g_mutexMetrics.timeoutCount) /
											 static_cast<float>(g_mutexMetrics.totalAttempts);
			}

			return false;
		}
		else
		{
			g_mutexMetrics.successCount++;
			g_mutexMetrics.consecutiveTimeouts = 0;

			// Calculate timeout rate
			if (g_mutexMetrics.totalAttempts > 0)
			{
				g_mutexMetrics.timeoutRate = static_cast<float>(g_mutexMetrics.timeoutCount) /
											 static_cast<float>(g_mutexMetrics.totalAttempts);
			}
		}
	}

	bool overallResult = false;

	// Only upload logs if SD card is available
	if (!isStorageModeSdCard())
	{
		if (uploadMutex != nullptr)
		{
			xSemaphoreGive(uploadMutex);
		}
		return false;
	}

	// Check if log file exists
	if (!SD_MMC.exists(logFilePath))
	{
		if (uploadMutex != nullptr)
		{
			xSemaphoreGive(uploadMutex);
		}
		return false;
	}

	// Check file size first
	File fileProbe = SD_MMC.open(logFilePath, FILE_READ);
	if (!fileProbe)
	{
		if (uploadMutex != nullptr)
		{
			xSemaphoreGive(uploadMutex);
		}
		return false;
	}

	size_t fileSize = fileProbe.size();
	fileProbe.close();

	if (fileSize == 0)
	{
		if (uploadMutex != nullptr)
		{
			xSemaphoreGive(uploadMutex);
		}
		return false;
	}

	// Extract filename from path (e.g., "2025-11-16-LOG.txt" from "/logs/2025/11/2025-11-16-LOG.txt")
	String filename = logFilePath.substring(logFilePath.lastIndexOf('/') + 1);
	if (filename.isEmpty())
	{
		filename = "log.txt";
	}

	// Convert filename format from "2025-11-16-LOG.txt" to "2025-11-16.LOG" as expected by API
	String apiFilename = filename;
	apiFilename.replace("-LOG.txt", ".LOG");
	apiFilename.replace("-LOG.TXT", ".LOG");
	apiFilename.replace(".txt", ".LOG");
	apiFilename.replace(".TXT", ".LOG");

	const String boundary = "----BoondockBoundary";
	const String lineBreak = "\r\n";
	const String &deviceId = getDeviceId();

	// Build multipart form data
	String macPart = "--" + boundary + lineBreak +
					 "Content-Disposition: form-data; name=\"mac_address\"" + lineBreak + lineBreak +
					 deviceId + lineBreak;

	String filenamePart = "--" + boundary + lineBreak +
						  "Content-Disposition: form-data; name=\"filename\"" + lineBreak + lineBreak +
						  apiFilename + lineBreak;

	String fileHeader = "--" + boundary + lineBreak +
						"Content-Disposition: form-data; name=\"file\"; filename=\"" + apiFilename + "\"" + lineBreak +
						"Content-Type: text/plain" + lineBreak + lineBreak;

	String closing = lineBreak + "--" + boundary + "--" + lineBreak;

	// Calculate content length
	const size_t contentLength = macPart.length() + filenamePart.length() + fileHeader.length() + fileSize + closing.length();

	size_t logOrder[kApiEndpointCount];
	size_t logOrderCount = 0;
	getHealthyEndpointIndicesRandomOrder(logOrder, kApiEndpointCount, logOrderCount);

	for (size_t li = 0; li < logOrderCount; ++li)
	{
		size_t idx = logOrder[li];
		ApiEndpointState &endpoint = apiEndpointStates[idx];
		if (!appSettings.upload.enabled[idx] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}

		WiFiClient plainClient;
		WiFiClient *clientPtr = &plainClient;

		// Get per-endpoint port
		uint16_t endpointPort = getUploadPort(idx);
		if (endpointPort == 0)
		{
			// Port must be specified - skip this endpoint
			continue;
		}

		// Reopen log file for this endpoint attempt
		File logFile = SD_MMC.open(logFilePath, FILE_READ);
		if (!logFile)
		{
			continue;
		}

		bool attemptSuccess = true;
		String attemptError;
		const unsigned long attemptStartMs = millis();

		if (!connectWiFiClientWithRetry(*clientPtr, endpoint.host, endpointPort, 1, 3000))
		{
			attemptSuccess = false;
			attemptError = "upload_server_unreachable";
		}
		else
		{
			String uploadUrl = String(DEFAULT_LOG_UPLOAD_PATH);
			String hostHeader = String(endpoint.host) + ":" + String(endpointPort);

			String requestLine = "POST " + uploadUrl + " HTTP/1.1";
			String headers = "Host: " + hostHeader + "\r\n"
													 "User-Agent: " +
							 getUserAgentString() + "\r\n"
													"Accept: application/json\r\n"
													"Content-Type: multipart/form-data; boundary=" +
							 boundary + "\r\n"
										"Content-Length: " +
							 String(static_cast<unsigned long>(contentLength)) + "\r\n"
																				 "Connection: close\r\n";

			clientPtr->print(requestLine + "\r\n");
			clientPtr->print(headers);
			clientPtr->print("\r\n");

			clientPtr->print(macPart);
			clientPtr->print(filenamePart);
			clientPtr->print(fileHeader);

			// Upload log file data
			size_t bytesRead = 0;
			uint32_t yieldCount = 0;
			while ((bytesRead = logFile.read(uploadBuffer, sizeof(uploadBuffer))) > 0)
			{
				clientPtr->write(uploadBuffer, bytesRead);
				if ((++yieldCount & (kUploadChunksPerYield - 1u)) == 0u)
				{
					vTaskDelay(1);
				}
			}

			clientPtr->print(closing);

			unsigned long responseTimer = millis();
			String response;
			while ((millis() - responseTimer) < 10000) // 10 second timeout for log upload
			{
				while (clientPtr->available())
				{
					char c = static_cast<char>(clientPtr->read());
					response += c;
					responseTimer = millis();
				}

				if (!clientPtr->connected() && clientPtr->available() == 0)
				{
					break;
				}

				delay(10);
			}

			if (response.isEmpty())
			{
				attemptSuccess = false;
				attemptError = "empty_response";
			}
			else
			{
				int statusCode = 0;
				int firstSpace = response.indexOf(' ');
				if (firstSpace >= 0)
				{
					int secondSpace = response.indexOf(' ', firstSpace + 1);
					if (secondSpace > firstSpace)
					{
						statusCode = response.substring(firstSpace + 1, secondSpace).toInt();
					}
				}

				if (statusCode < 200 || statusCode >= 300)
				{
					attemptSuccess = false;
					attemptError = statusCode == 0 ? "invalid_status" : String("http_") + statusCode;
				}
				else
				{
					// Success
					overallResult = true;

					// Close file before breaking out of loop
					if (logFile)
					{
						logFile.close();
					}

					break; // Success on first endpoint, no need to try others
				}
			}
		}

		if (logFile)
		{
			logFile.close();
		}

		if (clientPtr != nullptr)
		{
			clientPtr->stop();
		}

		const unsigned long responseTimeMs = millis() - attemptStartMs;
		network_recordEndpointRequest(idx, attemptSuccess, responseTimeMs);
		network_updateEndpointHealthScore(idx);

		if (!attemptSuccess)
		{
			endpoint.failureCount++;
			bool markedDead = false;
			if (endpoint.failureCount >= kApiFailureThreshold)
			{
				endpoint.dead = true;
				endpoint.markedDeadAtMs = millis(); // Record when endpoint was marked dead
				markedDead = true;
				// Report dead endpoint to Serial (always JSON format)
				String message = "Endpoint marked DEAD: ";
				message += (endpoint.host ? endpoint.host : "(null)");
				message += " (failures=";
				message += String(static_cast<unsigned>(endpoint.failureCount));
				message += ")";
				Serial.print("{\"ty\":\"error\",\"mc\":\"");
				Serial.print(getDeviceId());
				Serial.print("\",\"si\":\"");
				Serial.print(getSessionId());
				Serial.print("\",\"ms\":\"");
				// Escape JSON special characters
				for (size_t i = 0; i < message.length(); ++i)
				{
					char c = message.charAt(i);
					if (c == '"' || c == '\\')
					{
						Serial.print('\\');
					}
					Serial.print(c);
				}
				Serial.println("\"}");
			}

			if (attemptError.isEmpty())
			{
				attemptError = "unknown";
			}
		}

		if (!attemptSuccess && li + 1 < logOrderCount)
		{
			delayAfterEndpointFailure(idx);
		}
	}

	if (uploadMutex != nullptr)
	{
		xSemaphoreGive(uploadMutex);
	}

	return overallResult;
}

// Push full settings JSON to the API server using multipart/form-data:
// POST /api/v1/settings with fields:
//   mac_address = <deviceId>
//   settings    = <JSON string>
bool network_pushSettingsToServer(const String &settingsJson)
{
	if (!WiFi.isConnected())
	{
		return false;
	}

	ensureApiEndpoints();

	const String &deviceId = getDeviceId();
	const String boundary = "----BoondockSettingsBoundary";
	const String lineBreak = "\r\n";

	// Build multipart parts
	String macPart = "--" + boundary + lineBreak +
					 "Content-Disposition: form-data; name=\"mac_address\"" + lineBreak + lineBreak +
					 deviceId + lineBreak;

	String settingsPart = "--" + boundary + lineBreak +
						  "Content-Disposition: form-data; name=\"settings\"" + lineBreak + lineBreak +
						  settingsJson + lineBreak;

	String closing = "--" + boundary + "--" + lineBreak;

	const size_t contentLength = macPart.length() +
								 settingsPart.length() +
								 closing.length();

	bool overallResult = false;

	for (size_t idx = 0; idx < kApiEndpointCount; ++idx)
	{
		ApiEndpointState &endpoint = apiEndpointStates[idx];
		if (!appSettings.upload.enabled[idx] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}

		uint16_t endpointPort = getUploadPort(idx);
		if (endpointPort == 0)
		{
			endpointPort = DEFAULT_API_PORT;
		}

		WiFiClient client;
		const unsigned long attemptStartMs = millis();
		constexpr unsigned long kMaxApiOperationTimeMs = 5000; // 5 seconds max for connect + send + response

		// Log API endpoint attempt with settings info
		logDebugf("[Network] Attempting to push settings to API endpoint\n");
		logDebugf("[Network] Settings data length: %u bytes\n", static_cast<unsigned>(contentLength));

		// Check timeout before connection
		if ((millis() - attemptStartMs) >= kMaxApiOperationTimeMs)
		{
			logWarnf("[Settings] Timeout exceeded before connection, skipping endpoint %zu\n", idx);
			continue;
		}

		if (!connectWiFiClientWithRetry(client, endpoint.host, endpointPort, 1, 2000))
		{
			logDebugf("[Settings] Failed to connect to endpoint %zu for settings push", idx);
			delayAfterEndpointFailure(idx);
			continue;
		}

		logDebugf("[Settings] Connected to endpoint %zu for settings push", idx);

		String requestLine = "POST /api/v1/settings HTTP/1.1";
		String headers = "Host: " + String(endpoint.host) + ":" + String(endpointPort) + "\r\n"
																						 "User-Agent: " +
						 getUserAgentString() + "\r\n"
												"Accept: application/json\r\n"
												"Content-Type: multipart/form-data; boundary=" +
						 boundary + "\r\n"
									"Content-Length: " +
						 String(static_cast<unsigned long>(contentLength)) + "\r\n"
																			 "Connection: close\r\n";

		client.print(requestLine + "\r\n");
		client.print(headers);
		client.print("\r\n");

		client.print(macPart);
		client.print(settingsPart);
		client.print(closing);

		// Check timeout before waiting for response
		unsigned long timeRemaining = kMaxApiOperationTimeMs - (millis() - attemptStartMs);
		if (timeRemaining <= 0)
		{
			logWarnf("[Settings] Timeout exceeded before response wait, aborting endpoint %zu\n", idx);
			client.stop();
			continue;
		}

		// Limit response wait to remaining time (max 2 seconds)
		constexpr unsigned long kMaxResponseWaitMs = 2000;
		unsigned long responseTimeoutMs = (timeRemaining < kMaxResponseWaitMs) ? timeRemaining : kMaxResponseWaitMs;

		// Read response
		unsigned long responseTimer = millis();
		String response;
		uint32_t watchdogFeedCounter = 0;
		while ((millis() - responseTimer) < responseTimeoutMs)
		{
			// Check overall timeout
			if ((millis() - attemptStartMs) >= kMaxApiOperationTimeMs)
			{
				logWarnf("[Settings] Overall timeout exceeded during response wait, aborting endpoint %zu\n", idx);
				break;
			}

			// Feed watchdog periodically during response wait (every 10 iterations = ~100ms)
			if (++watchdogFeedCounter >= 10)
			{
				esp_task_wdt_reset();
				watchdogFeedCounter = 0;
			}

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

		bool attemptSuccess = true;
		if (response.isEmpty())
		{
			attemptSuccess = false;
			continue;
		}

		// Parse HTTP status
		int statusCode = 0;
		int firstSpace = response.indexOf(' ');
		if (firstSpace >= 0)
		{
			int secondSpace = response.indexOf(' ', firstSpace + 1);
			if (secondSpace > firstSpace)
			{
				statusCode = response.substring(firstSpace + 1, secondSpace).toInt();
			}
		}

		if (statusCode >= 200 && statusCode < 300)
		{
			overallResult = true;
			unsigned long responseTimeMs = millis() - attemptStartMs;
			network_recordEndpointRequest(idx, true, responseTimeMs);
			network_updateEndpointHealthScore(idx);
			logDebugf("[Settings] Settings pushed successfully to endpoint %zu (response_time: %lums)",
					  idx, responseTimeMs);
			break;
		}
		else
		{
			attemptSuccess = false;
		}

		unsigned long responseTimeMs = millis() - attemptStartMs;
		network_recordEndpointRequest(idx, attemptSuccess, responseTimeMs);
		network_updateEndpointHealthScore(idx);
	}

	return overallResult;
}

// Fetch settings JSON from API server:
// GET /api/v1/settings/{mac_address}
// On success, outSettingsJson will contain the response body (JSON).
bool network_pullSettingsFromServer(String &outSettingsJson)
{
	outSettingsJson = String();

	// Check WiFi connection status - use both isConnected() and status() for more reliable detection
	if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
	{
		logDebugf("[Network] Skipping settings pull - WiFi not connected (isConnected: %d, status: %d)\n",
				  WiFi.isConnected(), WiFi.status());
		return false;
	}

	ensureApiEndpoints();

	const String &deviceId = getDeviceId();

	for (size_t idx = 0; idx < kApiEndpointCount; ++idx)
	{
		ApiEndpointState &endpoint = apiEndpointStates[idx];
		if (!appSettings.upload.enabled[idx] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}

		uint16_t endpointPort = getUploadPort(idx);
		if (endpointPort == 0)
		{
			endpointPort = DEFAULT_API_PORT;
		}

		WiFiClient client;
		const unsigned long attemptStartMs = millis();

		// Log API endpoint attempt with endpoint URL and device ID
		String endpointUrl = String("http://") + String(endpoint.host) + ":" + String(endpointPort) + "/api/v1/settings/" + deviceId;
		logDebugf("[Network] Pulling settings from API: %s\n", deviceId.c_str());

		// Small delay before connection to avoid rate limiting (space out requests)
		esp_task_wdt_reset();
		delay(100);

		// Check timeout before connection
		constexpr unsigned long kMaxApiOperationTimeMs = 5000; // 5 seconds max for connect + send + response
		if ((millis() - attemptStartMs) >= kMaxApiOperationTimeMs)
		{
			logWarnf("[Event] Timeout exceeded before connection, skipping endpoint %zu\n", idx);
			continue;
		}

		if (!connectWiFiClientWithRetry(client, endpoint.host, endpointPort, 1, 2000))
		{
			logDebugf("[Settings] Failed to connect to endpoint %zu for settings pull", idx);
			delayAfterEndpointFailure(idx);
			continue;
		}

		logDebugf("[Settings] Connected to endpoint %zu for settings pull", idx);

		String requestPath = "/api/v1/settings/" + deviceId;
		String requestLine = "GET " + requestPath + " HTTP/1.1";
		String headers = "Host: " + String(endpoint.host) + ":" + String(endpointPort) + "\r\n"
																						 "User-Agent: " +
						 getUserAgentString() + "\r\n"
												"Accept: application/json\r\n"
												"Connection: close\r\n";

		client.print(requestLine + "\r\n");
		client.print(headers);
		client.print("\r\n");

		// Check timeout before waiting for response
		unsigned long timeRemaining = kMaxApiOperationTimeMs - (millis() - attemptStartMs);
		if (timeRemaining <= 0)
		{
			logWarnf("[Settings] Timeout exceeded before response wait, aborting endpoint %zu\n", idx);
			client.stop();
			continue;
		}

		// Limit response wait to remaining time (max 2 seconds)
		constexpr unsigned long kMaxResponseWaitMs = 2000;
		unsigned long responseTimeoutMs = (timeRemaining < kMaxResponseWaitMs) ? timeRemaining : kMaxResponseWaitMs;

		unsigned long responseTimer = millis();
		String response;
		uint32_t watchdogFeedCounter = 0;
		while ((millis() - responseTimer) < responseTimeoutMs)
		{
			// Check overall timeout
			if ((millis() - attemptStartMs) >= kMaxApiOperationTimeMs)
			{
				logWarnf("[Settings] Overall timeout exceeded during response wait, aborting endpoint %zu\n", idx);
				break;
			}

			// Feed watchdog periodically during response wait (every 10 iterations = ~100ms)
			if (++watchdogFeedCounter >= 10)
			{
				esp_task_wdt_reset();
				watchdogFeedCounter = 0;
			}

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

		bool attemptSuccess = true;
		if (response.isEmpty())
		{
			attemptSuccess = false;
			continue;
		}

		// Parse HTTP status and body
		int statusCode = 0;
		int firstSpace = response.indexOf(' ');
		if (firstSpace >= 0)
		{
			int secondSpace = response.indexOf(' ', firstSpace + 1);
			if (secondSpace > firstSpace)
			{
				statusCode = response.substring(firstSpace + 1, secondSpace).toInt();
			}
		}

		int bodyIndex = response.indexOf("\r\n\r\n");
		String body = bodyIndex >= 0 ? response.substring(bodyIndex + 4) : "";

		if (statusCode >= 200 && statusCode < 300 && body.length() > 0)
		{
			outSettingsJson = body;
			unsigned long responseTimeMs = millis() - attemptStartMs;
			network_recordEndpointRequest(idx, true, responseTimeMs);
			network_updateEndpointHealthScore(idx);
			return true;
		}

		unsigned long responseTimeMs = millis() - attemptStartMs;
		network_recordEndpointRequest(idx, false, responseTimeMs);
		network_updateEndpointHealthScore(idx);
	}

	// Unable to fetch settings from any endpoint
	logWarnf("[Network] Unable to fetch settings from server (all endpoints failed)\n");
	return false;
}

void network_invalidateApiEndpoints()
{
	apiEndpointsInitialized = false;
}

// Periodic retry mechanism for dead endpoints
// Retries dead endpoints every 1 minute when WiFi is connected
void network_retryDeadEndpoints()
{
	if (!isWiFiConnected())
	{
		return; // Only retry when WiFi is connected
	}

	ensureApiEndpoints();

	constexpr unsigned long kDeadEndpointRetryIntervalMs = 60000; // 1 minute
	unsigned long now = millis();

	for (size_t i = 0; i < kApiEndpointCount; ++i)
	{
		ApiEndpointState &endpoint = apiEndpointStates[i];

		// Only retry enabled endpoints that are marked dead
		if (!appSettings.upload.enabled[i] || !endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}

		// Check if enough time has passed since marking as dead
		if (endpoint.markedDeadAtMs == 0 || (now - endpoint.markedDeadAtMs) < kDeadEndpointRetryIntervalMs)
		{
			continue;
		}

		// Reset dead status to allow retry
		endpoint.dead = false;
		endpoint.failureCount = 0; // Reset failure count to give it a fresh chance
		endpoint.markedDeadAtMs = 0;
	}
}

bool network_areAllEndpointsDead()
{
	ensureApiEndpoints();

	size_t enabledCount = 0;
	size_t deadCount = 0;

	for (size_t i = 0; i < kApiEndpointCount; ++i)
	{
		ApiEndpointState &endpoint = apiEndpointStates[i];
		// Only count enabled endpoints with valid hosts
		if (appSettings.upload.enabled[i] && endpoint.host != nullptr && std::strlen(endpoint.host) > 0)
		{
			enabledCount++;
			if (endpoint.dead)
			{
				deadCount++;
			}
		}
	}

	// All endpoints are dead if we have enabled endpoints and all of them are dead
	// Note: Logging is handled by callers (e.g., main.cpp with rate limiting)
	return (enabledCount > 0 && deadCount == enabledCount);
}

namespace
{
// Serialize station connect attempts: concurrent WiFi.begin from e.g. UploadTask and network_loop()
// triggers ESP-IDF "wifi:sta is connecting, return error" and can confuse the STA state machine.
struct ConnectToWiFiMutexGuard
{
	SemaphoreHandle_t mux;
	bool took = false;

	explicit ConnectToWiFiMutexGuard(SemaphoreHandle_t m) : mux(m)
	{
		if (mux != nullptr)
		{
			took = (xSemaphoreTake(mux, portMAX_DELAY) == pdTRUE);
		}
	}

	~ConnectToWiFiMutexGuard()
	{
		if (took && mux != nullptr)
		{
			xSemaphoreGive(mux);
		}
	}
};
} // namespace

void connectToWiFi()
{
	static SemaphoreHandle_t connectMux = nullptr;
	if (connectMux == nullptr)
	{
		connectMux = xSemaphoreCreateMutex();
	}
	if (connectMux == nullptr)
	{
		logErrorf("[WiFi] connectToWiFi: mutex create failed");
		return;
	}
	ConnectToWiFiMutexGuard lock(connectMux);

	if (WiFi.isConnected())
	{
		return;
	}

	// Ensure WiFi is in station mode only (not AP or mixed mode)
	// This prevents mixed mode when credentials are configured
	WiFi.mode(WIFI_STA);

	for (size_t idx = 0; idx < kMaxWifiCredentials; ++idx)
	{
		const char *ssid_c = appSettings.wifi[idx].ssid;
		const char *pass_c = appSettings.wifi[idx].password;
		const unsigned long timeoutMs = appSettings.wifi[idx].connectTimeoutMs;

		if (ssid_c == nullptr)
		{
			continue;
		}

		String ssid = String(ssid_c);
		if (ssid.length() == 0)
		{
			continue;
		}

		// Apply WiFi TX power setting before connection attempt
		uint8_t esp32TxPower = mapWifiTxPowerLevel(appSettings.wifiTxPower);
		esp_wifi_set_max_tx_power(esp32TxPower);

		logDebugf("[WiFi] Attempting connection to SSID: %s (timeout: %lums)",
				  ssid_c, timeoutMs);

		WiFi.begin(ssid_c, pass_c);

		const unsigned long start = millis();
		while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
		{
			delay(500);
		}

		if (WiFi.status() == WL_CONNECTED)
		{
			logDebugf("[WiFi] Successfully connected to SSID: %s (took %lums)",
					  ssid_c, millis() - start);
			synchronizeClockWithNtp();
			// Reset dead endpoints when WiFi reconnects
			for (size_t i = 0; i < kApiEndpointCount; ++i)
			{
				if (apiEndpointStates[i].dead)
				{
					apiEndpointStates[i].dead = false;
					apiEndpointStates[i].failureCount = 0;
					apiEndpointStates[i].markedDeadAtMs = 0;
				}
			}
			// Small delay to allow network stack to stabilize after connection
			delay(100);

			// Network reconnect tracking is handled in main.cpp maintenance task
			// Connection success message is logged by WiFi event handler (ARDUINO_EVENT_WIFI_STA_GOT_IP)

			return;
		}
		else
		{
			logDebugf("[WiFi] Connection to SSID: %s failed (status: %d, elapsed: %lums)",
					  ssid_c, WiFi.status(), millis() - start);
		}

		WiFi.disconnect(true, true);
	}

	logErrorf("[WiFi] Connection failed - no credentials succeeded");
}

void network_reinitializeWiFi()
{
	// Re-register WiFi event handler (needed after WiFi.mode changes)
	WiFi.onEvent(handleWiFiEvent);
}

void network_reconnectWiFi()
{
	// Same logic as RECONNECT CLI command

	// Disconnect WiFi
	WiFi.disconnect(true, true);
	delay(500);

	// Stop WiFi
	WiFi.mode(WIFI_OFF);
	delay(500);

	// Start WiFi again
	WiFi.mode(WIFI_STA);
	// Re-register WiFi event handler
	network_reinitializeWiFi();
	delay(500);

	// Connect to WiFi
	connectToWiFi();

	// Wait a bit for connection to stabilize
	delay(1000);

	// Check connection status
	if (isWiFiConnected())
	{
		// Reset dead endpoints when WiFi reconnects
		network_invalidateApiEndpoints();
	}
	else
	{
		logWarnf("[Network] WiFi reconnection failed");
	}
}

// Queue event for async sending
void sendEvent(const String &type, const String &message, JsonObject *settings)
{

	/*
	// Initialize queue if needed
	if (eventQueue == nullptr)
	{
		eventQueue = xQueueCreate(kEventQueueSize, sizeof(EventQueueEntry));
		if (eventQueue == nullptr)
		{
			return;
		}
	}

	// Create queue entry
	// Create queue entry
	static EventQueueEntry entry;
	memset(&entry, 0, sizeof(entry));

	strncpy(entry.eventType, type.c_str(), sizeof(entry.eventType) - 1);
	entry.eventType[sizeof(entry.eventType) - 1] = '\0';

	strncpy(entry.eventMessage, message.c_str(), sizeof(entry.eventMessage) - 1);
	entry.eventMessage[sizeof(entry.eventMessage) - 1] = '\0';

	if (settings != nullptr)
	{
		String settingsStr;
		serializeJson(*settings, settingsStr);
		strncpy(entry.settingsJson, settingsStr.c_str(), sizeof(entry.settingsJson) - 1);
		entry.settingsJson[sizeof(entry.settingsJson) - 1] = '\0';
		entry.hasSettings = true;
	}
	else
	{
		entry.hasSettings = false;
		entry.settingsJson[0] = '\0';
	}
#if defined(ECHO)
	entry.enqueuedAtMs = millis();
	entry.eventCounter = ++g_eventCounterSeq;
	// Queue full -> drop oldest (with counter/log)
	if (uxQueueSpacesAvailable(eventQueue) == 0)
	{
		static EventQueueEntry dropped;
		if (xQueueReceive(eventQueue, &dropped, 0) == pdTRUE)
		{
			g_eventDroppedQueueFull++;
			logWarnf("[Event] dropped reason=queue_full dropped_counter=%lu dropped_type=%s total_queue_full_drops=%lu",
					 static_cast<unsigned long>(dropped.eventCounter),
					 dropped.eventType,
					 g_eventDroppedQueueFull);
		}
	}
	if (xQueueSend(eventQueue, &entry, 0) != pdTRUE)
	{
		g_eventDroppedQueueFull++;
		logWarnf("[Event] dropped reason=queue_push_failed counter=%lu type=%s total_queue_full_drops=%lu",
				 static_cast<unsigned long>(entry.eventCounter),
				 entry.eventType,
				 g_eventDroppedQueueFull);
	}
#else
	// Check if queue is full
	if (uxQueueSpacesAvailable(eventQueue) == 0)
	{
		// Queue full - drop oldest event
		static  EventQueueEntry dummy;
		xQueueReceive(eventQueue, &dummy, 0);
	}

	// Queue the event (non-blocking - if queue is full, it will be dropped)
	if (xQueueSend(eventQueue, &entry, 0) != pdTRUE)
	{
		// Queue full - drop event silently
	}

#endif
*/

	// Initialize queue if needed
	if (eventQueue == nullptr)
	{
		eventQueue = xQueueCreate(kEventQueueSize, sizeof(EventQueueEntry));
		if (eventQueue == nullptr)
		{
			return;
		}
	}

	// Check if queue is full
	if (uxQueueSpacesAvailable(eventQueue) == 0)
	{
		// Queue full - drop oldest event
		EventQueueEntry dummy;
		xQueueReceive(eventQueue, &dummy, 0);
	}

	// Create queue entry
	EventQueueEntry entry = {};
	strncpy(entry.eventType, type.c_str(), sizeof(entry.eventType) - 1);
	entry.eventType[sizeof(entry.eventType) - 1] = '\0';

	strncpy(entry.eventMessage, message.c_str(), sizeof(entry.eventMessage) - 1);
	entry.eventMessage[sizeof(entry.eventMessage) - 1] = '\0';

	if (settings != nullptr)
	{
		String settingsStr;
		serializeJson(*settings, settingsStr);
		strncpy(entry.settingsJson, settingsStr.c_str(), sizeof(entry.settingsJson) - 1);
		entry.settingsJson[sizeof(entry.settingsJson) - 1] = '\0';
		entry.hasSettings = true;
	}
	else
	{
		entry.hasSettings = false;
		entry.settingsJson[0] = '\0';
	}

	// Queue the event (non-blocking - if queue is full, it will be dropped)
	if (xQueueSend(eventQueue, &entry, 0) != pdTRUE)
	{
		// Queue full - drop event silently
	}
	
}

static void runEventSendForEntry(EventQueueEntry &entry)
{
	if (!isWiFiConnected())
	{
		logDebugf("[Event] Dropping event (WiFi not connected): type=%s", entry.eventType);
		return;
	}
	if (wifiConnectedAtMs > 0)
	{
		unsigned long timeSinceWiFiConnect = millis() - wifiConnectedAtMs;
		if (timeSinceWiFiConnect < kEventSendDelayAfterWiFiConnectMs)
		{
			xQueueSendToFront(eventQueue, &entry, 0);
			return;
		}
	}
	DynamicJsonDocument doc(2048);
	doc["event_type"] = entry.eventType;
	doc["mac_address"] = getDeviceId();
	DynamicJsonDocument messageDoc(1024);
	DeserializationError error = deserializeJson(messageDoc, entry.eventMessage);
	if (error == DeserializationError::Ok)
	{
		doc["event_data"] = messageDoc;
	}
	else
	{
		JsonObject eventData = doc.createNestedObject("event_data");
		eventData["message"] = entry.eventMessage;
	}
	if (entry.hasSettings && strlen(entry.settingsJson) > 0)
	{
		DynamicJsonDocument settingsDoc(1024);
		DeserializationError settingsError = deserializeJson(settingsDoc, entry.settingsJson);
		if (settingsError == DeserializationError::Ok)
		{
			doc["settings"] = settingsDoc;
		}
	}
	String jsonPayload;
	serializeJson(doc, jsonPayload);
	const unsigned long eventSendStartMs = millis();
	constexpr unsigned long kMaxEventSendTimeMs = 10000;
	bool sent = false;
	size_t eventOrder[kApiEndpointCount];
	size_t eventOrderCount = 0;
	getHealthyEndpointIndicesRandomOrder(eventOrder, kApiEndpointCount, eventOrderCount);
	for (size_t ei = 0; ei < eventOrderCount; ++ei)
	{
		size_t idx = eventOrder[ei];
		if ((millis() - eventSendStartMs) >= kMaxEventSendTimeMs)
		{
			esp_task_wdt_reset();
			break;
		}
		esp_task_wdt_reset();
		const ApiEndpointState &endpoint = apiEndpointStates[idx];
		if (!appSettings.upload.enabled[idx] || endpoint.dead || endpoint.host == nullptr || std::strlen(endpoint.host) == 0)
		{
			continue;
		}
		uint16_t endpointPort = getUploadPort(idx);
		if (endpointPort == 0)
		{
			continue;
		}

		WiFiClient &client = g_networkSharedWiFiClient;
		// Reuse shared object safely by closing prior connection first.
		client.stop();

		const unsigned long attemptStartMs = millis();
		constexpr unsigned long kMaxApiOperationTimeMs = 5000;
		logDebugf("[Network] Sending event to API endpoint: event_type=%s\n", entry.eventType);
		esp_task_wdt_reset();
		delay(100);
		if (!connectWiFiClientWithRetry(client, endpoint.host, endpointPort, 1, 2000))
		{
			logDebugf("[Event] Failed to connect to endpoint %zu for event: %s",
					  idx, entry.eventType);
			delayAfterEndpointFailure(idx);
			continue;
		}
		logDebugf("[Event] Connected to endpoint %zu for event: %s",
				  idx, entry.eventType);
		String requestLine = "POST " + String(DEFAULT_EVENT_PATH) + " HTTP/1.1";
		String headers = "Host: " + String(endpoint.host) + ":" + String(endpointPort) + "\r\n"
																						 "User-Agent: " +
						 getUserAgentString() + "\r\n"
												"Accept: application/json\r\n"
												"Content-Type: application/json\r\n"
												"Content-Length: " +
						 String(jsonPayload.length()) + "\r\n"
														"Connection: close\r\n";
		client.print(requestLine + "\r\n");
		client.print(headers);
		client.print("\r\n");
		client.print(jsonPayload);
		unsigned long timeRemaining = kMaxApiOperationTimeMs - (millis() - attemptStartMs);
		if (timeRemaining <= 0)
		{
			logWarnf("[Event] Timeout exceeded before response wait, aborting endpoint %zu\n", idx);
			client.stop();
			continue;
		}
		constexpr unsigned long kMaxResponseWaitMs = 2000;
		unsigned long responseTimeoutMs = (timeRemaining < kMaxResponseWaitMs) ? timeRemaining : kMaxResponseWaitMs;
		unsigned long responseTimer = millis();
		String response;
		uint32_t watchdogFeedCounter = 0;
		while ((millis() - responseTimer) < responseTimeoutMs)
		{
			if ((millis() - attemptStartMs) >= kMaxApiOperationTimeMs)
			{
				logWarnf("[Event] Overall timeout exceeded during response wait, aborting endpoint %zu\n", idx);
				break;
			}
			if (++watchdogFeedCounter >= 10)
			{
				esp_task_wdt_reset();
				watchdogFeedCounter = 0;
			}
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
		bool attemptSuccess = false;
		if (!response.isEmpty())
		{
			int statusCode = 0;
			int firstSpace = response.indexOf(' ');
			if (firstSpace >= 0)
			{
				int secondSpace = response.indexOf(' ', firstSpace + 1);
				if (secondSpace > firstSpace)
				{
					statusCode = response.substring(firstSpace + 1, secondSpace).toInt();
				}
			}
			int bodyIndex = response.indexOf("\r\n\r\n");
			String body = bodyIndex >= 0 ? response.substring(bodyIndex + 4) : "";
			body.trim();
			if (statusCode >= 200 && statusCode < 300)
			{
				if (!body.isEmpty())
				{
					DynamicJsonDocument responseDoc(512);
					DeserializationError jsonError = deserializeJson(responseDoc, body);
					if (jsonError == DeserializationError::Ok)
					{
						syncClockFromApiResponse(body);
						const char *message = responseDoc["message"] | "";
						bool hasSuccessMessage = (strstr(message, "Event received") != nullptr ||
												  strstr(message, "event received") != nullptr ||
												  strstr(message, "received") != nullptr);
						if (hasSuccessMessage)
						{
							attemptSuccess = true;
							sent = true;
							totalEventCount++;
							time(&lastEventEpoch);
							unsigned long responseTimeMs = millis() - attemptStartMs;
							network_recordEndpointRequest(idx, true, responseTimeMs);
							network_updateEndpointHealthScore(idx);
							logDebugf("[Network] Event confirmed by server: %s (status=%d, response_time=%lums)\n",
									  entry.eventType, statusCode, responseTimeMs);
							logDebugf("[Event] Event sent successfully: type=%s, endpoint=%zu, response_time=%lums",
									  entry.eventType, idx, responseTimeMs);
							break;
						}
						else
						{
							logWarnf("[Network] Event API returned status %d but unexpected message: %s\n",
									 statusCode, message);
						}
					}
					else
					{
						logWarnf("[Network] Event API returned status %d but invalid JSON: %s\n",
								 statusCode, jsonError.c_str());
					}
				}
				else
				{
					attemptSuccess = true;
					sent = true;
					totalEventCount++;
					time(&lastEventEpoch);
					unsigned long responseTimeMs = millis() - attemptStartMs;
					network_recordEndpointRequest(idx, true, responseTimeMs);
					network_updateEndpointHealthScore(idx);
					logDebugf("[Network] Event sent (status=%d, empty body, response_time=%lums)\n",
							  statusCode, responseTimeMs);
					break;
				}
			}
			else
			{
				logWarnf("[Network] Event API returned error status %d\n", statusCode);
			}
		}
		else
		{
			logWarnf("[Network] Event API returned empty response from %s:%u\n",
					 maskHostnameForLogging(endpoint.host), endpointPort);
		}
		if (!attemptSuccess)
		{
			unsigned long responseTimeMs = millis() - attemptStartMs;
			network_recordEndpointRequest(idx, false, responseTimeMs);
			network_updateEndpointHealthScore(idx);
		}
	}
	if (!sent)
	{
		if (deviceStartupMs > 0)
		{
			unsigned long timeSinceStartup = millis() - deviceStartupMs;
			if (timeSinceStartup >= kEventSendFailureGracePeriodMs)
			{
				logWarnf("[Network] Event failed to send to server (event_type: %s)\n", entry.eventType);
			}
		}
		else
		{
			logWarnf("[Network] Event failed to send to server (event_type: %s)\n", entry.eventType);
		}
	}
}

void network_drainEventsUntilMillis(unsigned long deadlineMs)
{
	if (eventQueue == nullptr)
	{
		return;
	}
	while (static_cast<long>(deadlineMs - millis()) > 0)
	{
		EventQueueEntry entry;
		if (xQueueReceive(eventQueue, &entry, 0) != pdTRUE)
		{
			return;
		}
		runEventSendForEntry(entry);
		esp_task_wdt_reset();
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	EventQueueEntry dropped = {};
	if (xQueueReceive(eventQueue, &dropped, 0) == pdTRUE)
	{
#if defined(ECHO)
/* 
		g_eventDroppedNoTime++;
		logWarnf("[Event] dropped reason=no_time_budget counter=%lu type=%s total_no_time_drops=%lu",
				 static_cast<unsigned long>(dropped.eventCounter),
				 dropped.eventType,
				 g_eventDroppedNoTime);
*/
#else
		logDebugf("[Event] dropped reason=no_time_budget type=%s", dropped.eventType);
#endif
	}
}

#if defined(ECHO)
void network_pumpEventsOnce()
{
	if (eventQueue == nullptr)
	{
		return;
	}
	EventQueueEntry entry;
	if (xQueueReceive(eventQueue, &entry, 0) != pdTRUE)
	{
		return;
	}
	runEventSendForEntry(entry);
}

void network_echoGetEventDropStats(unsigned long *droppedTooOld, unsigned long *droppedNoTime,
								   unsigned long *droppedQueueFull)
{
	if (droppedTooOld != nullptr)
	{
		*droppedTooOld = g_eventDroppedTooOld;
	}
	if (droppedNoTime != nullptr)
	{
		*droppedNoTime = g_eventDroppedNoTime;
	}
	if (droppedQueueFull != nullptr)
	{
		*droppedQueueFull = g_eventDroppedQueueFull;
	}
}

#endif

// Getter functions for upload and event statistics
time_t network_getLastUploadEpoch()
{
	return lastUploadEpoch;
}

unsigned long network_getTotalUploadCount()
{
	return totalUploadCount;
}

unsigned long network_getTotalUploadAttempts()
{
	return totalUploadAttempts;
}

float network_getOverallUploadSuccessRate()
{
	if (totalUploadAttempts == 0)
	{
		return -1.0f; // No attempts yet
	}
	return static_cast<float>(totalUploadCount) / static_cast<float>(totalUploadAttempts);
}

void network_incrementUploadAttempt()
{
	totalUploadAttempts++;
}

unsigned long network_getTotalEventCount()
{
	return totalEventCount;
}

time_t network_getLastEventEpoch()
{
	return lastEventEpoch;
}

// Network quality monitoring functions
void network_updateRssi()
{
	if (!WiFi.isConnected())
	{
		return;
	}

	int32_t rssi = WiFi.RSSI();
	unsigned long now = millis();

	if (g_networkQualityMetrics.rssiSampleCount == 0)
	{
		g_networkQualityMetrics.currentRssi = rssi;
		g_networkQualityMetrics.minRssi = rssi;
		g_networkQualityMetrics.maxRssi = rssi;
		g_networkQualityMetrics.averageRssi = static_cast<float>(rssi);
	}
	else
	{
		g_networkQualityMetrics.currentRssi = rssi;
		if (rssi < g_networkQualityMetrics.minRssi)
		{
			g_networkQualityMetrics.minRssi = rssi;
		}
		if (rssi > g_networkQualityMetrics.maxRssi)
		{
			g_networkQualityMetrics.maxRssi = rssi;
		}

		// Update running average
		float alpha = 0.1f; // Exponential moving average factor
		g_networkQualityMetrics.averageRssi =
			(1.0f - alpha) * g_networkQualityMetrics.averageRssi + alpha * static_cast<float>(rssi);
	}

	g_networkQualityMetrics.rssiSampleCount++;
	g_networkQualityMetrics.lastRssiUpdateMs = now;
}

void network_recordEndpointRequest(size_t endpointIndex, bool success, unsigned long responseTimeMs)
{
	if (endpointIndex >= kApiEndpointCount)
	{
		return;
	}

	EndpointHealthMetrics &metrics = g_endpointHealthMetrics[endpointIndex];
	metrics.totalRequests++;

	if (success)
	{
		metrics.successCount++;
		metrics.lastSuccessMs = millis();

		// Update response time statistics
		metrics.totalResponseTimeMs += responseTimeMs;
		if (metrics.totalRequests > 0)
		{
			metrics.averageResponseTimeMs = metrics.totalResponseTimeMs / metrics.totalRequests;
		}
		if (responseTimeMs < metrics.minResponseTimeMs)
		{
			metrics.minResponseTimeMs = responseTimeMs;
		}
		if (responseTimeMs > metrics.maxResponseTimeMs)
		{
			metrics.maxResponseTimeMs = responseTimeMs;
		}
	}
	else
	{
		metrics.failureCount++;
		metrics.lastFailureMs = millis();
		// Update network quality packet loss (not endpoint-specific)
		g_networkQualityMetrics.packetLossCount++;
	}

	g_networkQualityMetrics.totalPackets++;
	if (g_networkQualityMetrics.totalPackets > 0)
	{
		g_networkQualityMetrics.packetLossRate = static_cast<float>(g_networkQualityMetrics.packetLossCount) /
												 static_cast<float>(g_networkQualityMetrics.totalPackets);
	}

	// Update endpoint success rate
	if (metrics.totalRequests > 0)
	{
		metrics.successRate = static_cast<float>(metrics.successCount) /
							  static_cast<float>(metrics.totalRequests);
	}
}

void network_updateEndpointHealthScore(size_t endpointIndex)
{
	if (endpointIndex >= kApiEndpointCount)
	{
		return;
	}

	EndpointHealthMetrics &metrics = g_endpointHealthMetrics[endpointIndex];

	if (metrics.totalRequests == 0)
	{
		metrics.healthScore = 100.0f;
		return;
	}

	// Calculate health score based on multiple factors
	float successRate = static_cast<float>(metrics.successCount) /
						static_cast<float>(metrics.totalRequests);

	// Response time factor (penalize slow responses)
	float responseTimeFactor = 1.0f;
	if (metrics.averageResponseTimeMs > 5000) // > 5 seconds
	{
		responseTimeFactor = 0.5f;
	}
	else if (metrics.averageResponseTimeMs > 3000) // > 3 seconds
	{
		responseTimeFactor = 0.75f;
	}
	else if (metrics.averageResponseTimeMs > 1000) // > 1 second
	{
		responseTimeFactor = 0.9f;
	}

	// Packet loss factor (from network quality, not endpoint-specific)
	float packetLossFactor = 1.0f - g_networkQualityMetrics.packetLossRate;
	if (packetLossFactor < 0.0f)
	{
		packetLossFactor = 0.0f;
	}

	// Calculate composite health score
	metrics.healthScore = successRate * 100.0f * responseTimeFactor * packetLossFactor;

	// Check circuit breaker conditions
	unsigned long now = millis();
	if (metrics.healthScore < kMinHealthScoreForCircuitBreaker)
	{
		if (!metrics.circuitBreakerOpen)
		{
			metrics.circuitBreakerOpen = true;
			metrics.circuitBreakerOpenMs = now;
			logWarnf("[Network] Circuit breaker opened for endpoint %s (health score: %.1f)\n",
					 maskHostnameForLogging(metrics.host),
					 metrics.healthScore);
		}
	}
	else if (metrics.circuitBreakerOpen)
	{
		// Check if enough time has passed to reset circuit breaker
		if ((now - metrics.circuitBreakerOpenMs) >= kCircuitBreakerResetTimeMs)
		{
			logDebugf("[Network] Circuit breaker closed for endpoint %s (health score: %.1f)",
					  maskHostnameForLogging(metrics.host), metrics.healthScore);
			network_resetCircuitBreaker(endpointIndex);
		}
	}
}

bool network_isCircuitBreakerOpen(size_t endpointIndex)
{
	if (endpointIndex >= kApiEndpointCount)
	{
		return false;
	}

	EndpointHealthMetrics &metrics = g_endpointHealthMetrics[endpointIndex];

	// Auto-reset circuit breaker after timeout
	if (metrics.circuitBreakerOpen)
	{
		unsigned long now = millis();
		if ((now - metrics.circuitBreakerOpenMs) >= kCircuitBreakerResetTimeMs)
		{
			network_resetCircuitBreaker(endpointIndex);
			return false;
		}
	}

	return metrics.circuitBreakerOpen;
}

void network_resetCircuitBreaker(size_t endpointIndex)
{
	if (endpointIndex >= kApiEndpointCount)
	{
		return;
	}

	EndpointHealthMetrics &metrics = g_endpointHealthMetrics[endpointIndex];
	if (metrics.circuitBreakerOpen)
	{
		metrics.circuitBreakerOpen = false;
		metrics.circuitBreakerOpenMs = 0;
	}
}

uint32_t network_calculateAdaptiveBackoff(size_t endpointIndex, uint32_t baseBackoffMs)
{
	if (endpointIndex >= kApiEndpointCount)
	{
		return baseBackoffMs;
	}

	EndpointHealthMetrics &metrics = g_endpointHealthMetrics[endpointIndex];
	NetworkQualityMetrics &networkQuality = g_networkQualityMetrics;

	// Adjust backoff based on network quality (RSSI)
	float rssiFactor = 1.0f;
	if (networkQuality.averageRssi < -85) // Poor signal
	{
		rssiFactor = 2.0f; // Double backoff
	}
	else if (networkQuality.averageRssi < -75) // Fair signal
	{
		rssiFactor = 1.5f; // 1.5x backoff
	}

	// Adjust backoff based on endpoint health score
	float healthFactor = 1.0f;
	if (metrics.healthScore < 30.0f)
	{
		healthFactor = 3.0f; // Triple backoff for unhealthy endpoints
	}
	else if (metrics.healthScore < 50.0f)
	{
		healthFactor = 2.0f; // Double backoff
	}
	else if (metrics.healthScore < 70.0f)
	{
		healthFactor = 1.5f; // 1.5x backoff
	}

	uint32_t adaptiveBackoff = static_cast<uint32_t>(baseBackoffMs * rssiFactor * healthFactor);

	// Cap at reasonable maximum (60 seconds)
	constexpr uint32_t kMaxBackoffMs = 60000;
	if (adaptiveBackoff > kMaxBackoffMs)
	{
		adaptiveBackoff = kMaxBackoffMs;
	}

	return adaptiveBackoff;
}

// AP Mode Functions moved to boondock_server.cpp
