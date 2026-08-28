#pragma once

#include <Arduino.h>

#ifndef FIRMWARE_PREFIX
#define FIRMWARE_PREFIX "TANGO"  // Default fallback
#endif
#define FIRMWARE FIRMWARE_PREFIX "-v2.0.0-beta.01"
#define CONFIG_VERSION "1.0.0"

// Human-readable product name for UI (browser title, sidebar, API field "product")
#if defined(TANGO)
#define PRODUCT_BROWSER_TITLE "Boondock Tango"
#elif defined(ECHO)
#define PRODUCT_BROWSER_TITLE "Boondock Echo"
#endif

// Uppercase product line for serial banners (AP mode, etc.)
#if defined(TANGO)
#define PRODUCT_AP_ENV_LABEL "TANGO"
#elif defined(ECHO)
#define PRODUCT_AP_ENV_LABEL "ECHO"
#endif

// Stereo record input channel (left/right) is configurable on TANGO only; Echo uses a fixed path.
#if defined(TANGO)
#define BOONDOCK_HAS_RECORD_INPUT_CHANNEL 1
#endif

#define DEFAULT_AUDIO_UPLOAD_HOSTS_DOMAIN {"api.oh.boondock.cloud", "api.or.boondock.cloud", "api.vi.boondock.cloud", "apitest.boondock.cloud"}
#define DEFAULT_AUDIO_UPLOAD_REGIONS {"🌐 Boondock - Ohio", "🌐 Boondock - Oregon", "🌐 Boondock - Virginia", "💻 Custom"}

#define DEFAULT_AUDIO_UPLOAD_HOSTS_IP {"3.128.235.120", "35.85.105.6", "52.1.103.236", "52.38.112.84"}
#define DEFAULT_AUDIO_UPLOAD_HOSTS_PORT {7001, 7001, 7001, 7001}

#define DEFAULT_AUDIO_UPLOAD_HOST_COUNT 4

#define DEFAULT_API_PORT 7001

#define DEFAULT_AUDIO_UPLOAD_PATH "/api/v2/audio/s3"
#define DEFAULT_EVENT_PATH "/api/v1/events"
#define DEFAULT_LOG_UPLOAD_PATH "/api/v1/upload/logs"
#define DEFAULT_FIRMWARE_CHECK_PATH "/api/v1/firmware/check"
#define DEFAULT_AUDIO_UPLOAD_USE_SSL false
#define DEFAULT_UPLOAD_QUEUE_DEPTH 4
#define DEFAULT_UPLOAD_CONVERT_TO_MP3 true

// Optional CDN sync for system files (SPA + notification sounds).
// Disabled by default; set these via build flags (e.g. platformio.ini) to enable.
#ifndef SYSTEM_ASSETS_CDN_HOST
#define SYSTEM_ASSETS_CDN_HOST ""
#endif
#ifndef SYSTEM_ASSETS_CDN_PORT
#define SYSTEM_ASSETS_CDN_PORT 80
#endif
#ifndef SYSTEM_ASSETS_CDN_BASE_PATH
#define SYSTEM_ASSETS_CDN_BASE_PATH ""
#endif

// Upload timeouts (increase if uploads fail with "Upload timeout" or "Empty response" on slow links)
#define UPLOAD_BODY_TIMEOUT_MS       60000UL  // Max time to send request body (45s; ~480KB @ ~11 KB/s)
#define UPLOAD_TOTAL_TIMEOUT_MS     60000UL  // Max time for all endpoint attempts (60s)
#define UPLOAD_RESPONSE_WAIT_MS      5000UL  // Max time to wait for HTTP response after body


#define AUDIO_SAMPLE_BUFFERS 1024
// Audio threshold sensitivity mapping constants
// These define the dB range for the audio threshold setting (0-60)
// Recorder trigger dB is calculated as setting - 80, so 0 maps to -80 dB,
// the default 30 maps to -50 dB, and 60 maps to -20 dB.
#define AUDIO_DB_SENSITIVITY_HIGH -20.0f   // Higher dB value (less negative) - low sensitivity (setting=0)
#define AUDIO_DB_SENSITIVITY_LOW -80.0f    // Lower dB value (more negative) - high sensitivity (setting=100)
#define AUDIO_DB_SENSITIVITY_MID -50.0f    // Middle dB value for default setting (30)
#define AUDIO_DB_SMOOTHING_WINDOW 20

#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_WIFI_CONNECT_TIMEOUT_MS 30000UL
#define DEFAULT_TX_POWER 10
#define DEFAULT_HOSTNAME "boondock"
#define MAX_HOSTNAME_LENGTH 63
#define DEFAULT_AUDIO_SAMPLE_RATE 8000
#define DEFAULT_AUDIO_BUFFER_SAMPLES AUDIO_SAMPLE_BUFFERS
#define DEFAULT_AUDIO_PRE_RECORD_MS 3000
#define DEFAULT_AUDIO_MIN_PRE_RECORD_MS 0
#define DEFAULT_AUDIO_MAX_PRE_RECORD_MS 10000

#define DEFAULT_AUDIO_THRESHOLD 50  // dB   
#define DEFAULT_AUDIO_MIN_RECORDING_MS 1000
#define DEFAULT_AUDIO_MAX_RECORDING_MS 30000
#define DEFAULT_AUDIO_MAX_SD_RECORDING_MS 180000
#define DEFAULT_AUDIO_SILENCE_THRESHOLD_MS 1000
#define DEFAULT_AUDIO_MIN_SILENCE_THRESHOLD_MS 100
#define DEFAULT_AUDIO_MAX_SILENCE_THRESHOLD_MS 10000
#define DEFAULT_AUDIO_CODEC_GAIN_DB 0
#if defined(BOONDOCK_HAS_RECORD_INPUT_CHANNEL)
/** Stored value matches codec stereo slot: 0 = first sample (L in I2S order), 1 = second (R). Web UI swaps Left/Right labels vs these indices. */
#define DEFAULT_AUDIO_RECORD_INPUT_CHANNEL 1
#endif
#define DEFAULT_AUDIO_DISCARD_SMALL_FILES_ENABLED false
#define DEFAULT_AUDIO_DISCARD_SMALL_FILES_MIN_MS 1000

// Speaker defaults (ECHO has onboard speaker; other environments default off unless user enables)
#define DEFAULT_SPEAKER_VOLUME 50
#if defined(ECHO)
#define DEFAULT_SPEAKER_ENABLED true
#else
#define DEFAULT_SPEAKER_ENABLED false
#endif

// Transmit defaults (legacy TX feature not implemented yet; store settings for future)
#define DEFAULT_AUDIO_TRANSMIT_ENABLED true
#define DEFAULT_AUDIO_TRANSMIT_VOLUME 50

// CW (Morse) defaults (ECHO UI)
#define DEFAULT_CW_WPM 18
#define DEFAULT_CW_TONE_HZ 600
#define DEFAULT_CW_VOLUME 60
#define DEFAULT_CW_REPEAT 1

// Legacy ECHO LED behavior defaults
#define DEFAULT_LED_STYLE 0  // 0 = flashing, 1 = solid
#define DEFAULT_STARTUP_MODE 1
#define DEFAULT_OFFLINE_MODE false

#define DEFAULT_TIMEZONE_OFFSET_HOURS 0  // UTC offset in hours (0 = UTC, -5 = EST, +5 = IST, etc.)
#define DEFAULT_MAINTENANCE_HOUR 3       // Default maintenance hour (3 AM local time)
#define DEFAULT_MAINTENANCE_MINUTE 0     // Default maintenance minute (3:00 AM local time)

// Log filter defaults
// Serial: Fatal, Error, Warning enabled; Info, Debug, Event disabled
#define DEFAULT_LOG_SERIAL_FATAL true
#define DEFAULT_LOG_SERIAL_ERROR true
#define DEFAULT_LOG_SERIAL_WARNING true
#define DEFAULT_LOG_SERIAL_INFO false
#define DEFAULT_LOG_SERIAL_DEBUG false
#define DEFAULT_LOG_SERIAL_EVENT false
// File: Fatal, Error, Warning, Info, Event enabled; Debug disabled
#define DEFAULT_LOG_FILE_FATAL true
#define DEFAULT_LOG_FILE_ERROR true
#define DEFAULT_LOG_FILE_WARNING true
#define DEFAULT_LOG_FILE_INFO true
#define DEFAULT_LOG_FILE_DEBUG false
#define DEFAULT_LOG_FILE_EVENT true

// Memory safety reboot thresholds
#define MEMORY_CRITICAL_HEAP_FREE_KB 30        // 30 KB - trigger reboot if heap free drops below this
#define MEMORY_CRITICAL_MIN_FREE_HEAP_KB 20    // 20 KB - trigger reboot if min free heap drops below this
#define MEMORY_CRITICAL_LARGEST_BLOCK_KB 50    // 50 KB - trigger reboot if largest free block drops below this
#define MEMORY_MONITOR_INTERVAL_MS 5000        // Check memory every 5 seconds

// Memory warning thresholds (before critical - allows proactive cleanup)
#define MEMORY_WARNING_HEAP_FREE_KB 50         // 50 KB - warn if heap free drops below this
#define MEMORY_WARNING_MIN_FREE_HEAP_KB 35     // 35 KB - warn if min free heap drops below this
#define MEMORY_WARNING_LARGEST_BLOCK_KB 75     // 75 KB - warn if largest free block drops below this
#define MEMORY_WARNING_COOLDOWN_MS 60000       // Only warn once per minute to avoid spam

// SD card settings
#define SD_MMC_MAX_OPEN_FILES 8                // Maximum number of simultaneously open files on SD card

// Default SD enablement: TANGO ships with SD off until enabled in Advanced; ECHO default on (matches AppSettings init).
#if defined(TANGO)
#define DEFAULT_SD_USE_SD_CARD false
#define DEFAULT_SD_RECORD_TO_SD_CARD false
#else
#define DEFAULT_SD_USE_SD_CARD true
#define DEFAULT_SD_RECORD_TO_SD_CARD true
#endif

// Time constants in milliseconds
#define ONE_SECOND_MS          (1UL * 1000UL)
#define FIVE_SECONDS_MS        (5UL * 1000UL)
#define THIRTY_SECONDS_MS      (30UL * 1000UL)
#define ONE_MINUTE_MS          (60UL * 1000UL)
#define FIVE_MINUTES_MS        (5UL * 60UL * 1000UL)
#define TEN_MINUTES_MS         (10UL * 60UL * 1000UL)
#define FIFTEEN_MINUTES_MS     (15UL * 60UL * 1000UL)
#define THIRTY_MINUTES_MS      (30UL * 60UL * 1000UL)
#define ONE_HOUR_MS            (60UL * 60UL * 1000UL)
#define SIX_HOURS_MS           (6UL * 60UL * 60UL * 1000UL)
#define TWENTY_FOUR_HOURS_MS   (24UL * 60UL * 60UL * 1000UL)

// Cloud path: TCP to API vs WiFi link-only. Used to trigger WiFi stack recovery when STA has IP but cloud is unreachable.
#define CLOUD_PATH_FAILURE_THRESHOLD 6U
// Minimum time since last successful TCP connect to API before WiFi recovery may run (avoids flapping on brief blips).
#define CLOUD_PATH_MIN_TIME_SINCE_LAST_SUCCESS_MS (120UL * 1000UL)
// Grace after boot when we have never had a successful cloud TCP connect (allows threshold to apply after initial attempts).
#define CLOUD_PATH_BOOT_GRACE_MS (45UL * 1000UL)
// Minimum time between network_reconnectWiFi() calls triggered by cloud-path recovery.
#define CLOUD_PATH_RECOVERY_MIN_INTERVAL_MS FIVE_MINUTES_MS
// If no TCP success for this long, run an idle connectivity probe (when WiFi is up).
#define CLOUD_PATH_IDLE_NO_SUCCESS_MS (3UL * 60UL * 1000UL)
// How often to run the idle probe at most.
#define CLOUD_PATH_PROBE_INTERVAL_MS (90UL * 1000UL)
// Secondary TCP probe (well-known host) before WiFi reset: if this succeeds, assume WAN OK and skip reset (likely API outage).
#define CLOUD_PATH_SECONDARY_PROBE_HOST "1.1.1.1"
#define CLOUD_PATH_SECONDARY_PROBE_PORT 443
#define CLOUD_PATH_SECONDARY_PROBE_TIMEOUT_MS 2000UL
// If cloud TCP has not succeeded for this long, schedule a full device reboot (after recording completes), unless WAN probe succeeds (API-only outage).
#define CLOUD_PATH_FULL_REBOOT_AFTER_MS ONE_HOUR_MS
// At most once per interval, evaluate full-reboot (includes secondary WAN probe when past deadline).
#define CLOUD_PATH_FULL_REBOOT_EVAL_INTERVAL_MS (60UL * 1000UL)

#if defined(ECHO)
#define ECHO_MQTT_BROKER_HOST "mqtt.boondockecho.com"
#define ECHO_MQTT_BROKER_PORT   1883
#define ECHO_MQTT_USERNAME "webuser"

#define MQTT_WILL_TIMEOUT 60
#define MQTT_EVENT_QOS          1

/* Network States */
#define NETWORK_STATE_OFF 0
#define NETWORK_STATE_INIT 1
#define NETWORK_STATE_IDLE 2
#define NETWORK_STATE_IDLE_AP 3
#define NETWORK_STATE_SENDING 4
#define NETWORK_STATE_RECEIVING 5
#define NETWORK_STATE_UPDATING 6
#define NETWORK_STATE_AP_SENDING 7
#define NETWORK_STATE_AP_RECEIVING 8
#define NETWORK_STATE_OFFLINE 9
#define NETWORK_STATE_MIXED 10

/** 12-char MAC + NUL; filled by MQTT task from `getDeviceId()` (see `mqtt_task.cpp`). */
extern char device_id[13];

#endif
