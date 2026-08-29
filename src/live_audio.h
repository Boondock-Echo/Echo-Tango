#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

// Live audio buffer management
// Uses PSRAM circular buffer to store at least 1 second of audio data
// Drops older packages on buffer overrun to prioritize latest audio

// Initialize live audio buffer (call once at startup)
void live_audio_init();

// Feed audio samples into the live buffer
// samples: mono 16-bit PCM samples
// sampleCount: number of samples
// Returns true if samples were added, false if buffer is full and old data was dropped
bool live_audio_feed(const int16_t* samples, size_t sampleCount);

// Get available samples from buffer (reads from oldest to newest)
// destination: buffer to write samples to
// maxSamples: maximum number of samples to read
// Returns number of samples actually read
size_t live_audio_read(int16_t* destination, size_t maxSamples);

// Check if live audio is active (has clients connected)
bool live_audio_isActive();

// Register a client (call when WebSocket connects)
void live_audio_registerClient();

// Unregister a client (call when WebSocket disconnects)
void live_audio_unregisterClient();

// Get current buffer status
struct LiveAudioBufferStatus {
    size_t totalCapacity;      // Total buffer capacity in samples
    size_t availableSamples;   // Available samples to read
    size_t usedSamples;        // Currently used samples
    float bufferUtilization;  // Utilization percentage (0-100)
    size_t droppedSamples;     // Total samples dropped due to overrun
};

LiveAudioBufferStatus live_audio_getStatus();



