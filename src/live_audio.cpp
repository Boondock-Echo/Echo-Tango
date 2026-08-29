#include "live_audio.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <algorithm>

#include "config.h"
#include "logger.h"

namespace
{
    // Buffer configuration
    // At 8000 Hz sample rate, 1 second = 8000 samples = 16000 bytes
    // We'll allocate for at least 2 seconds to handle network delays
    constexpr size_t kLiveAudioBufferSamples = 16000; // 2 seconds at 8kHz
    constexpr size_t kLiveAudioBufferBytes = kLiveAudioBufferSamples * sizeof(int16_t);
    
    // PSRAM buffer
    int16_t* liveAudioBuffer = nullptr;
    size_t writeIndex = 0;
    size_t readIndex = 0;
    size_t bufferCount = 0; // Number of samples currently in buffer
    size_t totalDroppedSamples = 0;
    
    // Client management
    volatile int activeClientCount = 0;
    SemaphoreHandle_t bufferMutex = nullptr;
    
    // Initialize buffer in PSRAM
    bool initializeBuffer()
    {
        if (liveAudioBuffer != nullptr)
        {
            return true; // Already initialized
        }
        
        // Allocate in PSRAM
        liveAudioBuffer = (int16_t*)heap_caps_malloc(kLiveAudioBufferBytes, MALLOC_CAP_SPIRAM);
        if (liveAudioBuffer == nullptr)
        {
            logErrorf("[LiveAudio] Failed to allocate PSRAM buffer (%zu bytes)\n", kLiveAudioBufferBytes);
            return false;
        }
        
        // Initialize mutex
        if (bufferMutex == nullptr)
        {
            bufferMutex = xSemaphoreCreateMutex();
            if (bufferMutex == nullptr)
            {
                logErrorf("[LiveAudio] Failed to create mutex\n");
                heap_caps_free(liveAudioBuffer);
                liveAudioBuffer = nullptr;
                return false;
            }
        }
        
        // Clear buffer
        memset(liveAudioBuffer, 0, kLiveAudioBufferBytes);
        writeIndex = 0;
        readIndex = 0;
        bufferCount = 0;
        totalDroppedSamples = 0;
        
        logInfof("[LiveAudio] Initialized PSRAM buffer: %zu samples (%zu bytes)\n", 
                 kLiveAudioBufferSamples, kLiveAudioBufferBytes);
        return true;
    }
}

void live_audio_init()
{
    if (!initializeBuffer())
    {
        logErrorf("[LiveAudio] Initialization failed\n");
    }
}

bool live_audio_feed(const int16_t* samples, size_t sampleCount)
{
    if (samples == nullptr || sampleCount == 0)
    {
        return false;
    }
    
    // Only feed if there are active clients
    if (activeClientCount == 0)
    {
        return false; // No clients, don't waste CPU
    }
    
    if (liveAudioBuffer == nullptr || bufferMutex == nullptr)
    {
        return false;
    }
    
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return false; // Couldn't acquire mutex
    }
    
    // Check if adding these samples would cause overrun
    size_t newCount = bufferCount + sampleCount;
    if (newCount > kLiveAudioBufferSamples)
    {
        // Buffer overrun - drop oldest samples
        size_t samplesToDrop = newCount - kLiveAudioBufferSamples;
        readIndex = (readIndex + samplesToDrop) % kLiveAudioBufferSamples;
        bufferCount = kLiveAudioBufferSamples - sampleCount; // Will be kLiveAudioBufferSamples after adding
        totalDroppedSamples += samplesToDrop;
    }
    
    // Write new samples (handle wrap-around)
    for (size_t i = 0; i < sampleCount; ++i)
    {
        liveAudioBuffer[writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % kLiveAudioBufferSamples;
    }
    
    // Update count
    if (newCount <= kLiveAudioBufferSamples)
    {
        bufferCount = newCount;
    }
    else
    {
        bufferCount = kLiveAudioBufferSamples;
    }
    
    xSemaphoreGive(bufferMutex);
    return true;
}

size_t live_audio_read(int16_t* destination, size_t maxSamples)
{
    if (destination == nullptr || maxSamples == 0 || liveAudioBuffer == nullptr)
    {
        return 0;
    }
    
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0; // Couldn't acquire mutex
    }
    
    size_t samplesToRead = std::min(bufferCount, maxSamples);
    if (samplesToRead == 0)
    {
        xSemaphoreGive(bufferMutex);
        return 0;
    }
    
    // Read samples (handle wrap-around)
    size_t firstCopy = std::min(samplesToRead, kLiveAudioBufferSamples - readIndex);
    memcpy(destination, liveAudioBuffer + readIndex, firstCopy * sizeof(int16_t));
    
    if (samplesToRead > firstCopy)
    {
        // Need to wrap around
        memcpy(destination + firstCopy, liveAudioBuffer, (samplesToRead - firstCopy) * sizeof(int16_t));
    }
    
    readIndex = (readIndex + samplesToRead) % kLiveAudioBufferSamples;
    bufferCount -= samplesToRead;
    
    xSemaphoreGive(bufferMutex);
    return samplesToRead;
}

bool live_audio_isActive()
{
    return activeClientCount > 0;
}

void live_audio_registerClient()
{
    activeClientCount++;
    logDebugf("[LiveAudio] Client registered (total: %d)\n", activeClientCount);
}

void live_audio_unregisterClient()
{
    if (activeClientCount > 0)
    {
        activeClientCount--;
        logDebugf("[LiveAudio] Client unregistered (total: %d)\n", activeClientCount);
    }
}

LiveAudioBufferStatus live_audio_getStatus()
{
    LiveAudioBufferStatus status = {};
    
    if (liveAudioBuffer == nullptr)
    {
        return status;
    }
    
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        status.totalCapacity = kLiveAudioBufferSamples;
        status.availableSamples = bufferCount;
        status.usedSamples = bufferCount;
        status.bufferUtilization = (bufferCount * 100.0f) / kLiveAudioBufferSamples;
        status.droppedSamples = totalDroppedSamples;
        xSemaphoreGive(bufferMutex);
    }
    
    return status;
}



