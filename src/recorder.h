#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void startAudioCodec();
void updateCodecGainFromSettings();
void recordTask(void *pvParameters);

// Immediately stop any active recording session, ensuring file handles are closed.
void recorder_stopActiveRecording(const char *reason = "manual");

// Clear session cache for /pending exists + day-folder mkdir (call on SD failure/remount paths).
void recorder_invalidatePendingDirectoryCache();

// Get recording statistics for current session
struct RecordingStats {
    int recordingCount = 0;
    uint64_t totalDurationMs = 0;
    int uploadedCount = 0;
    int errorCount = 0;
};

RecordingStats recorder_getSessionStats();

// Increment error count (called when upload fails)
void recorder_incrementErrorCount();

// Increment uploaded count (called when upload succeeds)
void recorder_incrementUploadedCount();

// Get current audio level statistics for VU meter
struct AudioLevelStats {
    float currentLevel = 0.0f; // Current audio level (0-100)
    float currentDb = -120.0f; // Current audio level in dB
    float minLevel = 100.0f; // Minimum level in session
    float maxLevel = 0.0f; // Maximum level in session
    float minDb = 0.0f; // Minimum dB in session
    float maxDb = -120.0f; // Maximum dB in session
    float averageLevel = 0.0f; // Average level in session
    int16_t peakSample = 0; // Current peak sample value (0-32767)
    float dynamicRangeUtil = 0.0f; // Current dynamic range utilization (0-100%)
    int16_t maxPeakSample = 0; // Maximum peak sample in session
    float maxDynamicRangeUtil = 0.0f; // Maximum dynamic range utilization in session
};

AudioLevelStats recorder_getAudioLevelStats();

// Check if recording is currently active
bool recorder_isRecording();

// Start a sample recording without threshold checks (for SAMPLE command)
bool recorder_startSampleRecording();

// Get last recording information
time_t recorder_getLastRecordingEpoch();
uint32_t recorder_getLastRecordingDurationMs();

// Get current recording path (for cleanup operations to avoid interfering with active recording)
// Returns empty string if not recording or in PSRAM mode
String recorder_getCurrentRecordingPath();

// Live audio streaming callback - called with latest audio samples
// This allows external code to receive audio data for streaming
typedef void (*LiveAudioCallback)(const int16_t* samples, size_t sampleCount);
void recorder_setLiveAudioCallback(LiveAudioCallback callback);
void recorder_setLiveAudioFeedEnabled(bool enabled);

// Pause automatic recording without stopping mic streaming (browser Live Audio UX).
void recorder_setRecordingPausedForLiveSession(bool paused);
bool recorder_isRecordingPausedForLiveSession();

// MQTT record_line_in: 1 = allow line-in VOX recording; 0 = stop and block new recordings until 1.
void recorder_setLineInRecordingEnabled(bool enabled);
bool recorder_isLineInRecordingEnabled();

// Apply speaker settings (mute/volume) to the codec.
// Safe to call repeatedly; no-op if codec not ready.
void recorder_applySpeakerSettings();

// Button feedback beeps (legacy-style).
// Schedules the beep to be played on the AudioKit output.
void recorder_beep(int beepLengthMs, int beepDelayMs, int beepCount, bool beepGood);

// Placeholder playback APIs (to be implemented later).
bool recorder_playAudioFilePlaceholder(const String& pathOrId);

// Download cloud recording and play on speaker (ECHO / MQTT play_cloud).
// Runs on RecordTask: pauses uploads/recording for the session and feeds the watchdog.
void recorder_requestPlayCloud(const String& fileName);

// Same as play_cloud but keys PTT (GPIO 18) during playback when audio.transmitEnabled is set.
// Audio goes to line-out (radio) only; the onboard speaker stays muted during transmit.
void recorder_requestPlayTransmit(const String& fileName);

// SD playback controls (ECHO-style message navigation).
// These enqueue requests handled by the recorder task (AudioKit context).
void recorder_requestPlayNextRecording();
void recorder_requestPlayPrevRecording();

// Notification/system sounds.
// These are played by the recorder task (AudioKit context) to avoid I2S conflicts.
void recorder_requestPlayWavAtOrAfter(const String& wavPath, time_t receivedEpochUtcSeconds);

// Temporarily inhibit recording trigger/monitoring for a duration (ms).
// Useful to prevent boot beeps/WAVs from being re-recorded at startup.
void recorder_inhibitRecordingForMs(uint32_t durationMs);

// Morse code (CW) playback on speaker.
// Enqueues a best-effort playback request handled by the recorder task.
void recorder_requestPlayMorse(const String& text, uint16_t wpm, uint16_t toneHz, uint8_t volume, uint8_t repeat);
void recorder_cancelMorse();