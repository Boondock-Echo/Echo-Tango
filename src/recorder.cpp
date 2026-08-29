#include "recorder.h"

#include <AudioKitHAL.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <audio_driver/es8388/es8388.h>
#include <esp_err.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <SD_MMC.h>
#include "common.h"
#include "logger.h"
#include "main.h"
#include "network.h"
#include "timekeeper.h"
#include "upload_queue.h"

#if defined(ECHO)
#include "echo_hw.h"
#include "mqtt_task.h"
#include "networkHandller.h"
#endif

// Forward declaration for storage error tracking
extern void storage_recordWriteError();
extern void storage_recordReadError();

// Forward declaration for health metrics (defined in main.cpp)
extern SystemHealthMetrics g_healthMetrics;

// Session cache for /pending exists + day-folder mkdir (reset via recorder_invalidatePendingDirectoryCache)
static bool g_sessionPendingRootEnsured = false;
static String g_sessionEnsuredPendingDayDir;

namespace
{
    AudioKit kit;
    String currentRecordingPath;
    File currentRecordingFile;
    bool isRecording = false;
    bool isSampleRecording = false;
    unsigned long recordingStartMs = 0;
    unsigned long lastSoundMs = 0;
    time_t recordingStartEpoch = 0;
    size_t recordedBytes = 0;

    // SD recording: batch FAT flushes (finalize always flushes before close)
    constexpr unsigned long kSdRecordingFlushIntervalMs = 400;
    constexpr size_t kSdRecordingFlushByteThreshold = 32 * 1024;
    unsigned long sdRecordingLastFlushMs = 0;
    size_t sdRecordingBytesSinceFlush = 0;

    // Live audio streaming callback
    LiveAudioCallback liveAudioCallback = nullptr;
    std::atomic<bool> liveAudioFeedEnabled{false};
    std::atomic<bool> recordingPausedForLiveSession{false};
    std::atomic<bool> mqttLineInRecordingEnabled{true};
    
    // PSRAM recording mode variables
    // Memory usage calculation:
    // - Per recording: 480000 bytes (audio) + 44 bytes (WAV header) = 480044 bytes
    // - Active recording buffer: 1 � 480044 bytes = 480044 bytes
    // - Queue slots (6): 6 � 480044 bytes = 2880264 bytes
    // - Total worst case: 7 � 480044 = 3360308 bytes � 3.36MB (leaves ~834KB headroom in 4MB PSRAM)
    uint8_t* psramRecordingBuffer = nullptr;
    size_t psramRecordingCapacity = 0;
    size_t psramRecordingOffset = 0;
    constexpr size_t kPsramRecordingMaxBytes = 480000; // 30 sec at 8kHz mono 16-bit

    // Session statistics tracking
    int sessionRecordingCount = 0;
    uint64_t sessionTotalDurationMs = 0;
    int sessionUploadedCount = 0;
    int sessionErrorCount = 0;
    time_t sessionStartEpoch = 0;
    
    // Last recording tracking
    time_t lastRecordingEpoch = 0;
    uint32_t lastRecordingDurationMs = 0;
    
    // Audio level tracking for VU meter
    float currentAudioLevel = 0.0f;
    float currentAudioDb = -120.0f;
    float minAudioLevel = 100.0f;
    float maxAudioLevel = 0.0f;
    float minAudioDb = 0.0f;
    float maxAudioDb = -120.0f;
    float audioLevelSum = 0.0f;
    int audioLevelSampleCount = 0;
    
    // Peak sample and dynamic range tracking
    int16_t currentPeakSample = 0;
    float currentDynamicRangeUtil = 0.0f;
    int16_t maxPeakSample = 0;
    float maxDynamicRangeUtil = 0.0f;

    constexpr size_t kCodecGainLevelCount = 10;
    constexpr int8_t kCodecGainDbValues[kCodecGainLevelCount] = {-1, 0, 3, 6, 9, 12, 15, 18, 21, 24};
    constexpr es_mic_gain_t kCodecGainEnums[kCodecGainLevelCount] = {
        MIC_GAIN_MIN,
        MIC_GAIN_0DB,
        MIC_GAIN_3DB,
        MIC_GAIN_6DB,
        MIC_GAIN_9DB,
        MIC_GAIN_12DB,
        MIC_GAIN_15DB,
        MIC_GAIN_18DB,
        MIC_GAIN_21DB,
        MIC_GAIN_24DB};
    bool codecReady = false;
    uint8_t lastAppliedGainLevel = 255;

    bool lastAppliedSpeakerEnabled = false;
    uint8_t lastAppliedSpeakerVolume = 255;

    struct BeepRequest
    {
        int lengthMs = 0;
        int delayMs = 0;
        int count = 0;
        bool good = true;
    };

    QueueHandle_t g_beepQueue = nullptr;
    volatile bool g_isBeeping = false;

    enum class PlaybackCommand : uint8_t
    {
        Next = 0,
        Prev = 1,
    };

    struct PlaybackRequest
    {
        PlaybackCommand cmd = PlaybackCommand::Next;
    };

    QueueHandle_t g_playbackQueue = nullptr;
    volatile bool g_isPlayingFile = false;

    struct WavPlayRequest
    {
        char path[128];
        uint32_t playAtMs = 0; // millis() deadline; 0 means immediate
    };
    QueueHandle_t g_wavPlayQueue = nullptr;

#if defined(ECHO)
    struct CloudPlayRequest
    {
        char fileName[28];
        bool transmit = false;
    };
    QueueHandle_t g_cloudPlayQueue = nullptr;

    void ensureCloudPlayQueue()
    {
        if (g_cloudPlayQueue == nullptr)
        {
            g_cloudPlayQueue = xQueueCreate(1, sizeof(CloudPlayRequest));
        }
    }

    class CloudPlayResourceGuard
    {
    public:
        CloudPlayResourceGuard()
            : uploadWasPaused_(networkHandler_isUploadPaused()),
              recordingWasPaused_(recorder_isRecordingPausedForLiveSession())
        {
            networkHandler_setUploadPaused(true);
            recorder_setRecordingPausedForLiveSession(true);
            esp_task_wdt_reset();
        }

        ~CloudPlayResourceGuard()
        {
            // Brief pause before resuming uploads so WiFi/TCP can settle after download + playback.
            constexpr uint32_t kUploadResumeDelayMs = 2000;
            for (uint32_t elapsed = 0; elapsed < kUploadResumeDelayMs; elapsed += 100)
            {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_task_wdt_reset();
            }
            networkHandler_setUploadPaused(uploadWasPaused_);
            recorder_setRecordingPausedForLiveSession(recordingWasPaused_);
            esp_task_wdt_reset();
        }

    private:
        bool uploadWasPaused_;
        bool recordingWasPaused_;
    };

    struct RepeaterSimplexRequest
    {
        char path[128];
    };
    QueueHandle_t g_repeaterSimplexQueue = nullptr;
    bool g_pttAsserted = false;

    void ensureRepeaterSimplexQueue()
    {
        if (g_repeaterSimplexQueue == nullptr)
        {
            // Coalesce: only keep the latest request.
            g_repeaterSimplexQueue = xQueueCreate(1, sizeof(RepeaterSimplexRequest));
        }
    }

    void setPttOut(bool asserted)
    {
        if (g_pttAsserted == asserted)
        {
            return;
        }
        g_pttAsserted = asserted;
        digitalWrite(static_cast<uint8_t>(GPIO_PTT_OUT), asserted ? HIGH : LOW);
    }
#endif

    void ensureWavPlayQueue()
    {
        if (g_wavPlayQueue == nullptr)
        {
            // Coalesce: only keep the latest request.
            g_wavPlayQueue = xQueueCreate(1, sizeof(WavPlayRequest));
        }
    }

    struct MorseRequest
    {
        char text[128];
        uint16_t wpm = 18;
        uint16_t toneHz = 700;
        uint8_t volume = 60;
        uint8_t repeat = 1;
        bool cancel = false;
    };
    QueueHandle_t g_morseQueue = nullptr;
    volatile bool g_morseCancelFlag = false;

    void ensureMorseQueue()
    {
        if (g_morseQueue == nullptr)
        {
            // Coalesce: only keep the latest request.
            g_morseQueue = xQueueCreate(1, sizeof(MorseRequest));
        }
    }

    const char* morseForChar(char c)
    {
        // Letters
        switch (c)
        {
            case 'A': return ".-";
            case 'B': return "-...";
            case 'C': return "-.-.";
            case 'D': return "-..";
            case 'E': return ".";
            case 'F': return "..-.";
            case 'G': return "--.";
            case 'H': return "....";
            case 'I': return "..";
            case 'J': return ".---";
            case 'K': return "-.-";
            case 'L': return ".-..";
            case 'M': return "--";
            case 'N': return "-.";
            case 'O': return "---";
            case 'P': return ".--.";
            case 'Q': return "--.-";
            case 'R': return ".-.";
            case 'S': return "...";
            case 'T': return "-";
            case 'U': return "..-";
            case 'V': return "...-";
            case 'W': return ".--";
            case 'X': return "-..-";
            case 'Y': return "-.--";
            case 'Z': return "--..";
            // Numbers
            case '0': return "-----";
            case '1': return ".----";
            case '2': return "..---";
            case '3': return "...--";
            case '4': return "....-";
            case '5': return ".....";
            case '6': return "-....";
            case '7': return "--...";
            case '8': return "---..";
            case '9': return "----.";
            // Common punctuation
            case '.': return ".-.-.-";
            case ',': return "--..--";
            case '?': return "..--..";
            case '/': return "-..-.";
            case '=': return "-...-";
            case '+': return ".-.-.";
            case '-': return "-....-";
            case '(': return "-.--.";
            case ')': return "-.--.-";
            case ':': return "---...";
            case ';': return "-.-.-.";
            case '\'': return ".----.";
            case '"': return ".-..-.";
            case '!': return "-.-.--";
            case '@': return ".--.-.";
            default: return nullptr;
        }
    }

    void playSilenceMs(uint32_t ms)
    {
        if (!codecReady || ms == 0) return;
        constexpr int kSampleRate = 8000;
        const uint32_t totalSamples = (ms * kSampleRate) / 1000UL;
        static int16_t stereoZeros[256 * 2];
        memset(stereoZeros, 0, sizeof(stereoZeros));
        uint32_t written = 0;
        while (written < totalSamples && !g_morseCancelFlag)
        {
            const uint32_t chunk = std::min<uint32_t>(256, totalSamples - written);
            (void)kit.write(reinterpret_cast<const uint8_t*>(stereoZeros), chunk * 2 * sizeof(int16_t), pdMS_TO_TICKS(50));
            written += chunk;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    void playToneMs(uint32_t ms, uint16_t toneHz)
    {
        if (!codecReady || ms == 0) return;
        constexpr int kSampleRate = 8000;
        const uint32_t totalSamples = (ms * kSampleRate) / 1000UL;
        float phase = 0.0f;
        const float phaseInc = 2.0f * PI * static_cast<float>(toneHz) / static_cast<float>(kSampleRate);
        constexpr int16_t kAmp = 12000;

        static int16_t stereo[256 * 2];
        uint32_t written = 0;
        while (written < totalSamples && !g_morseCancelFlag)
        {
            const uint32_t frames = std::min<uint32_t>(256, totalSamples - written);
            for (uint32_t i = 0; i < frames; ++i)
            {
                const int16_t s = static_cast<int16_t>(sinf(phase) * static_cast<float>(kAmp));
                phase += phaseInc;
                if (phase > 2.0f * PI) phase -= 2.0f * PI;
                stereo[i * 2] = s;
                stereo[i * 2 + 1] = s;
            }
            (void)kit.write(reinterpret_cast<const uint8_t*>(stereo), frames * 2 * sizeof(int16_t), pdMS_TO_TICKS(50));
            written += frames;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    void playMorseBlocking(const MorseRequest& req)
    {
        if (!codecReady)
        {
            return;
        }

        // Standard Morse timing: dot = 1200 / WPM (ms)
        const uint16_t wpm = std::max<uint16_t>(5, std::min<uint16_t>(60, req.wpm));
        const uint32_t dotMs = 1200UL / static_cast<uint32_t>(wpm);
        const uint32_t dashMs = dotMs * 3UL;
        const uint32_t intraElementGapMs = dotMs;      // between dots/dashes
        const uint32_t charGapMs = dotMs * 3UL;        // between letters
        const uint32_t wordGapMs = dotMs * 7UL;        // between words

        const uint16_t toneHz = std::max<uint16_t>(200, std::min<uint16_t>(2000, req.toneHz));

        // Force speaker on for playback, but honor provided volume (0-100).
        uint8_t vol = req.volume;
        if (vol > 100) vol = 100;
        kit.setMute(false);
        kit.setVolume(vol);

        auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };

        for (uint8_t r = 0; r < std::max<uint8_t>(1, req.repeat); ++r)
        {
            if (g_morseCancelFlag) break;

            bool firstChar = true;
            for (size_t i = 0; i < sizeof(req.text) && req.text[i] != '\0'; ++i)
            {
                if (g_morseCancelFlag) break;

                char c = req.text[i];
                if (isSpace(c))
                {
                    playSilenceMs(wordGapMs);
                    firstChar = true;
                    continue;
                }

                if (!firstChar)
                {
                    playSilenceMs(charGapMs);
                }
                firstChar = false;

                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                const char* code = morseForChar(c);
                if (code == nullptr)
                {
                    // Unknown: treat as word break.
                    playSilenceMs(wordGapMs);
                    firstChar = true;
                    continue;
                }

                for (size_t k = 0; code[k] != '\0'; ++k)
                {
                    if (g_morseCancelFlag) break;
                    const char sym = code[k];
                    if (sym == '.')
                    {
                        playToneMs(dotMs, toneHz);
                    }
                    else if (sym == '-')
                    {
                        playToneMs(dashMs, toneHz);
                    }
                    // Gap between elements (but not after last element; char gap handles that)
                    if (code[k + 1] != '\0')
                    {
                        playSilenceMs(intraElementGapMs);
                    }
                }
            }

            // Small pause between repeats.
            if (r + 1 < req.repeat)
            {
                playSilenceMs(wordGapMs);
            }
        }

        // Restore mute state per user setting.
        kit.setMute(!appSettings.speakerEnabled);
    }

    struct RecentRecording
    {
        char path[128];
        uint32_t durationMs = 0;
        time_t epoch = 0;
        bool played = false;
        bool valid = false;
    };

    RecentRecording* g_recent = nullptr;
    size_t g_recentCap = 0;
    size_t g_recentHead = 0; // next insert position
    int g_recentCursor = -1; // navigation cursor for next/prev

    void ensurePlaybackQueue()
    {
        if (g_playbackQueue == nullptr)
        {
            // Coalesce navigation: only keep the latest request.
            g_playbackQueue = xQueueCreate(1, sizeof(PlaybackRequest));
        }
    }

    void ensureRecentList()
    {
        if (g_recent != nullptr)
        {
            return;
        }
        g_recentCap = 64;
#if defined(ESP32)
        g_recent = static_cast<RecentRecording*>(heap_caps_malloc(sizeof(RecentRecording) * g_recentCap, MALLOC_CAP_SPIRAM));
#endif
        if (g_recent == nullptr)
        {
            g_recent = static_cast<RecentRecording*>(malloc(sizeof(RecentRecording) * g_recentCap));
        }
        if (g_recent != nullptr)
        {
            memset(g_recent, 0, sizeof(RecentRecording) * g_recentCap);
            g_recentHead = 0;
            g_recentCursor = -1;
        }
    }

    void recentAddRecording(const String& wavPath, uint32_t durationMs, time_t epoch)
    {
        ensureRecentList();
        if (g_recent == nullptr || g_recentCap == 0)
        {
            return;
        }

        RecentRecording& r = g_recent[g_recentHead];
        memset(&r, 0, sizeof(r));
        strncpy(r.path, wavPath.c_str(), sizeof(r.path) - 1);
        r.durationMs = durationMs;
        r.epoch = epoch;
        r.played = false;
        r.valid = true;

        g_recentCursor = static_cast<int>(g_recentHead); // move cursor to most recent
        g_recentHead = (g_recentHead + 1) % g_recentCap;
    }

    int findNextUnplayedFrom(int startIdx)
    {
        if (g_recent == nullptr || g_recentCap == 0)
        {
            return -1;
        }
        for (size_t step = 0; step < g_recentCap; ++step)
        {
            const int idx = (startIdx + static_cast<int>(step) + 1) % static_cast<int>(g_recentCap);
            const RecentRecording& r = g_recent[idx];
            if (r.valid && !r.played && r.path[0] != '\0')
            {
                return idx;
            }
        }
        return -1;
    }

    int findPrevAnyFrom(int startIdx)
    {
        if (g_recent == nullptr || g_recentCap == 0)
        {
            return -1;
        }
        for (size_t step = 0; step < g_recentCap; ++step)
        {
            int idx = startIdx - static_cast<int>(step) - 1;
            while (idx < 0) idx += static_cast<int>(g_recentCap);
            const RecentRecording& r = g_recent[idx];
            if (r.valid && r.path[0] != '\0')
            {
                return idx;
            }
        }
        return -1;
    }

    bool playWavFileBlocking(const char* path, int16_t volumeOverride = -1, bool speakerOutput = true)
    {
        if (!codecReady || path == nullptr || path[0] == '\0')
        {
            return false;
        }
        if (!ensureStorage())
        {
            return false;
        }

        auto resolvePathIfMovedOrTypo = [&](const char* original, String& outResolved) -> bool
        {
            if (original == nullptr || original[0] == '\0')
            {
                return false;
            }

            // 1) As-is
            if (SD_MMC.exists(original))
            {
                outResolved = String(original);
                return true;
            }

            // 2) Fix common extension typo: ".wwav" -> ".wav"
            String candidate = String(original);
            if (candidate.endsWith(".wwav"))
            {
                candidate = candidate.substring(0, candidate.length() - 5) + ".wav";
                if (SD_MMC.exists(candidate))
                {
                    outResolved = candidate;
                    return true;
                }
            }

            // 3) If it was recorded in /pending but already moved to /inbox after upload, try predicted inbox path.
            if (candidate.startsWith("/pending/") && candidate.endsWith(".wav"))
            {
                String inboxPredicted;
                if (pendingWavToPredictedInboxPath(candidate, inboxPredicted) && inboxPredicted.length() > 0)
                {
                    if (SD_MMC.exists(inboxPredicted))
                    {
                        outResolved = inboxPredicted;
                        return true;
                    }
                }
            }

            return false;
        };

        String resolvedPath;
        if (!resolvePathIfMovedOrTypo(path, resolvedPath))
        {
            logWarnf("[Playback] File not found: %s", path);
            return false;
        }

        File f = SD_MMC.open(resolvedPath.c_str(), FILE_READ);
        if (!f)
        {
            logWarnf("[Playback] Failed to open: %s", resolvedPath.c_str());
            return false;
        }

        // Skip WAV header (44 bytes for PCM16 mono in our recorder).
        if (f.size() <= 44)
        {
            f.close();
            return false;
        }
        (void)f.seek(44);
        esp_task_wdt_reset();

        // Playback volume: speaker path unmutes the codec; transmit path keeps line-out only (PA off).
        uint8_t vol = appSettings.speakerVolume;
        if (volumeOverride >= 0 && volumeOverride <= 100)
        {
            vol = static_cast<uint8_t>(volumeOverride);
        }
        if (vol > 100) vol = 100;
        if (speakerOutput)
        {
            kit.setMute(false);
            kit.setVolume(vol);
        }
        else
        {
            kit.setSpeakerActive(false);
            kit.setMute(false);
            kit.setVolume(vol);
        }

        static uint8_t ioBuf[1024];
        uint32_t wdtFeedCounter = 0;
        while (f.available())
        {
            const size_t n = f.read(ioBuf, sizeof(ioBuf));
            if (n == 0)
            {
                break;
            }

            // Convert mono int16 samples into stereo (L=R) in-place using a separate buffer.
            const size_t monoBytes = n - (n % 2);
            const size_t monoSamples = monoBytes / 2;
            static int16_t stereoBuf[1024]; // 1024 int16 = 512 stereo frames max
            const size_t frames = std::min(monoSamples, sizeof(stereoBuf) / (2 * sizeof(int16_t)));
            const int16_t* mono = reinterpret_cast<const int16_t*>(ioBuf);
            for (size_t i = 0; i < frames; ++i)
            {
                stereoBuf[i * 2] = mono[i];
                stereoBuf[i * 2 + 1] = mono[i];
            }

            (void)kit.write(reinterpret_cast<const uint8_t*>(stereoBuf), frames * 2 * sizeof(int16_t), pdMS_TO_TICKS(200));
            vTaskDelay(pdMS_TO_TICKS(1));

            if (++wdtFeedCounter >= 16)
            {
                esp_task_wdt_reset();
                wdtFeedCounter = 0;
            }
        }
        f.close();

        if (speakerOutput)
        {
            kit.setMute(!appSettings.speakerEnabled);
        }
        else
        {
            kit.setSpeakerActive(true);
            kit.setMute(!appSettings.speakerEnabled);
            if (appSettings.speakerEnabled)
            {
                kit.setVolume(appSettings.speakerVolume);
            }
        }
        return true;
    }

    void ensureBeepQueue()
    {
        if (g_beepQueue == nullptr)
        {
            g_beepQueue = xQueueCreate(8, sizeof(BeepRequest));
        }
    }

    void playBeepBlocking(const BeepRequest& req)
    {
        if (!codecReady)
        {
            return;
        }

        // Legacy behavior: unmute for beeps, then restore mute per speakerEnabled.
        const bool restoreEnabled = appSettings.speakerEnabled;
        uint8_t restoreVol = appSettings.speakerVolume;
        if (restoreVol > 100) restoreVol = 100;

        kit.setMute(false);
        kit.setVolume(restoreVol);

        constexpr int kSampleRate = 8000;
        const float freq = req.good ? 1000.0f : 1500.0f;
        const int rampSamples = kSampleRate / 100; // 10ms
        const int fadeSamples = kSampleRate / 50;  // 20ms
        const int totalSamples = (req.lengthMs * kSampleRate) / 1000;
        if (totalSamples <= 0)
        {
            return;
        }

        float phase = 0.0f;
        const float phaseInc = 2.0f * PI * freq / static_cast<float>(kSampleRate);
        constexpr int16_t kAmp = 12000;

        auto writeOneStereoSample = [&](float gain)
        {
            const int16_t s = static_cast<int16_t>(sinf(phase) * static_cast<float>(kAmp) * gain);
            phase += phaseInc;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;
            int16_t stereo[2] = {s, s};
            (void)kit.write(reinterpret_cast<const uint8_t*>(stereo), sizeof(stereo), pdMS_TO_TICKS(50));
        };

        for (int j = 0; j < req.count; ++j)
        {
            // Ramp up
            for (int i = 0; i < rampSamples; ++i)
            {
                writeOneStereoSample(static_cast<float>(i) / static_cast<float>(rampSamples));
            }

            // Sustain
            int written = rampSamples;
            const int sustainTarget = std::max(0, totalSamples - fadeSamples);
            while (written < sustainTarget)
            {
                writeOneStereoSample(1.0f);
                ++written;
            }

            // Fade out
            for (int i = fadeSamples - 1; i >= 0; --i)
            {
                writeOneStereoSample(static_cast<float>(i) / static_cast<float>(fadeSamples));
            }

            vTaskDelay(pdMS_TO_TICKS(std::max(0, req.delayMs)));
        }

        kit.setVolume(restoreVol);
        kit.setMute(!restoreEnabled);
    }

    float dbSmoothingBuffer[kDbSmoothingWindow] = {0.0f};
    size_t dbSmoothingCount = 0;
    size_t dbSmoothingIndex = 0;
    float dbSmoothingSum = 0.0f;

    float currentRecordingMaxDb = -120.0f;
    uint32_t currentPreRecordMsApplied = 0;

    constexpr uint32_t kPreRecordBufferWindowMs = 5000;
    constexpr uint32_t kPreRecordMaxSampleRate = 8000;
    constexpr size_t kPreRecordBufferSamples = (kPreRecordMaxSampleRate * kPreRecordBufferWindowMs) / 1000;
    static_assert(kPreRecordBufferSamples > 0, "Pre-record buffer must reserve samples");

    // Pre-record buffers are large; keep them out of DRAM .bss to avoid link-time overflows.
    // Prefer PSRAM when available; fall back to heap if needed.
    int16_t* preRecordRing = nullptr;
    int16_t* preRecordScratch = nullptr;
    size_t preRecordRingWriteIndex = 0;
    size_t preRecordRingCount = 0;

    // Startup inhibit window (avoid recording boot beeps/WAVs picked up by the mic).
    volatile uint32_t g_recordInhibitUntilMs = 0;

    void appendAudioSamples(const int16_t *samples, size_t byteCount);
    void finalizeRecording(bool upload, const char *endReason);

    bool ensurePreRecordBuffers()
    {
        if (preRecordRing != nullptr && preRecordScratch != nullptr)
        {
            return true;
        }

        // If one is allocated and the other isn't, clean up and retry.
        if (preRecordRing != nullptr || preRecordScratch != nullptr)
        {
            if (preRecordRing) { heap_caps_free(preRecordRing); preRecordRing = nullptr; }
            if (preRecordScratch) { heap_caps_free(preRecordScratch); preRecordScratch = nullptr; }
            preRecordRingWriteIndex = 0;
            preRecordRingCount = 0;
        }

        const size_t bytes = kPreRecordBufferSamples * sizeof(int16_t);

#if defined(ESP32)
        preRecordRing = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        preRecordScratch = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
#endif
        if (preRecordRing == nullptr)
        {
            preRecordRing = static_cast<int16_t*>(malloc(bytes));
        }
        if (preRecordScratch == nullptr)
        {
            preRecordScratch = static_cast<int16_t*>(malloc(bytes));
        }

        if (preRecordRing == nullptr || preRecordScratch == nullptr)
        {
            if (preRecordRing) { free(preRecordRing); preRecordRing = nullptr; }
            if (preRecordScratch) { free(preRecordScratch); preRecordScratch = nullptr; }
            preRecordRingWriteIndex = 0;
            preRecordRingCount = 0;
            logErrorf("[Record] Failed to allocate pre-record buffers (bytes=%zu). Pre-record disabled.", bytes);
            return false;
        }

        memset(preRecordRing, 0, bytes);
        memset(preRecordScratch, 0, bytes);
        preRecordRingWriteIndex = 0;
        preRecordRingCount = 0;
        return true;
    }

    void pushPreRecordSamples(const int16_t *samples, size_t sampleCount)
    {
        if (samples == nullptr || sampleCount == 0)
        {
            return;
        }
        if (!ensurePreRecordBuffers())
        {
            return;
        }

        for (size_t i = 0; i < sampleCount; ++i)
        {
            preRecordRing[preRecordRingWriteIndex] = samples[i];
            preRecordRingWriteIndex = (preRecordRingWriteIndex + 1) % kPreRecordBufferSamples;
            if (preRecordRingCount < kPreRecordBufferSamples)
            {
                ++preRecordRingCount;
            }
        }
    }

    size_t copyRecentPreRecordSamples(int16_t *destination, size_t sampleCount)
    {
        if (destination == nullptr || sampleCount == 0 || preRecordRingCount == 0)
        {
            return 0;
        }
        if (!ensurePreRecordBuffers())
        {
            return 0;
        }

        const size_t available = std::min(preRecordRingCount, sampleCount);
        size_t startIndex = (preRecordRingWriteIndex + kPreRecordBufferSamples - available) % kPreRecordBufferSamples;
        const size_t firstCopy = std::min(available, kPreRecordBufferSamples - startIndex);
        
        std::memcpy(destination, preRecordRing + startIndex, firstCopy * sizeof(int16_t));
        if (available > firstCopy)
        {
            std::memcpy(destination + firstCopy, preRecordRing, (available - firstCopy) * sizeof(int16_t));
        }
        return available;
    }

    uint32_t appendPreRecordAudio()
    {
        if (!currentRecordingFile)
        {
            return 0;
        }
        if (!ensurePreRecordBuffers())
        {
            return 0;
        }

        const uint32_t configuredMs = appSettings.audio.preRecordMs;
        const uint32_t sampleRate = appSettings.audio.sampleRate;
        if (configuredMs == 0 || sampleRate == 0 || preRecordRingCount == 0)
        {
            return 0;
        }

        size_t requestedSamples = static_cast<size_t>((static_cast<uint64_t>(sampleRate) * configuredMs) / 1000ULL);
        if (requestedSamples == 0)
        {
            return 0;
        }

        requestedSamples = std::min(requestedSamples, kPreRecordBufferSamples);
        const size_t samplesToCopy = std::min(preRecordRingCount, requestedSamples);
        if (samplesToCopy == 0)
        {
            return 0;
        }

        const size_t copiedSamples = copyRecentPreRecordSamples(preRecordScratch, samplesToCopy);
        if (copiedSamples == 0)
        {
            return 0;
        }

        const float preRollDb = calculateDb(preRecordScratch, copiedSamples);
        if (preRollDb > currentRecordingMaxDb)
        {
            currentRecordingMaxDb = preRollDb;
        }

        appendAudioSamples(preRecordScratch, copiedSamples * sizeof(int16_t));

        const uint32_t renderedMs = static_cast<uint32_t>((static_cast<uint64_t>(copiedSamples) * 1000ULL) / sampleRate);
        return renderedMs;
    }

    float pushDbAndGetAverage(float db)
    {
        if (dbSmoothingCount < kDbSmoothingWindow)
        {
            dbSmoothingBuffer[dbSmoothingCount++] = db;
            dbSmoothingSum += db;
        }
        else
        {
            dbSmoothingSum -= dbSmoothingBuffer[dbSmoothingIndex];
            dbSmoothingBuffer[dbSmoothingIndex] = db;
            dbSmoothingSum += db;
            dbSmoothingIndex = (dbSmoothingIndex + 1) % kDbSmoothingWindow;
        }

        const size_t count = dbSmoothingCount < kDbSmoothingWindow ? dbSmoothingCount : kDbSmoothingWindow;
        return count > 0 ? dbSmoothingSum / static_cast<float>(count) : db;
    }

    void resetSoundDetection()
    {
        dbSmoothingCount = 0;
        dbSmoothingIndex = 0;
        dbSmoothingSum = 0.0f;
    }

    void clearPreRecordBuffers()
    {
        preRecordRingWriteIndex = 0;
        preRecordRingCount = 0;
    }

    void appendAudioSamples(const int16_t *samples, size_t byteCount)
    {
        // Check if storage mode switched from SD to PSRAM during recording
        if (isRecording && currentRecordingFile && isStorageModePsram())
        {
            logWarnf("[Record] Storage mode switched to PSRAM - stopping current SD recording");
            logDebugf("[Record] Storage mode switch detected (from SD to PSRAM, recorded_bytes: %zu)", 
                      recordedBytes);
            currentRecordingFile.close();
            currentRecordingFile = File();
            finalizeRecording(false, "storage_mode_switch");
            return;
        }

        // If recording but no PSRAM buffer and in PSRAM mode, allocate it
        if (isRecording && isRecordingModePsram() && psramRecordingBuffer == nullptr)
        {
            uint32_t maxRecordingMs = std::min(appSettings.audio.maxRecordingMs, kPsramMaxRecordingMs);
            size_t maxBytes = (appSettings.audio.sampleRate * sizeof(int16_t) * maxRecordingMs) / 1000;
            maxBytes = std::min(maxBytes, kPsramRecordingMaxBytes);
            size_t allocSize = sizeof(WaveHeader) + maxBytes;
            psramRecordingBuffer = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM));
            if (psramRecordingBuffer != nullptr)
            {
                psramRecordingCapacity = maxBytes + sizeof(WaveHeader);
                logDebugf("[Record] PSRAM buffer allocated after mode switch (size: %zu bytes)", 
                          psramRecordingCapacity);
                psramRecordingOffset = sizeof(WaveHeader);
                currentRecordingPath = "PSRAM";
                recordedBytes = 0;
            }
            else
            {
                logErrorf("[Record] Failed to allocate PSRAM buffer after mode switch");
                finalizeRecording(false, "psram_alloc_failed");
                return;
            }
        }
        
        // PSRAM mode - write to PSRAM buffer
        if (isRecordingModePsram())
        {
            if (psramRecordingBuffer == nullptr)
            {
                return;
            }
            
            // Check if we have space
            if (psramRecordingOffset + byteCount > psramRecordingCapacity)
            {
                // Buffer full - truncate
                size_t available = psramRecordingCapacity - psramRecordingOffset;
                if (available > 0)
                {
                    memcpy(psramRecordingBuffer + psramRecordingOffset, samples, available);
                    psramRecordingOffset += available;
                    recordedBytes += available;
                }
                return;
            }
            
            memcpy(psramRecordingBuffer + psramRecordingOffset, samples, byteCount);
            psramRecordingOffset += byteCount;
            recordedBytes += byteCount;
            return;
        }
        
        // SD card mode - write once; on failure finalize this recording and switch to PSRAM (avoids corrupt retry)
        if (!currentRecordingFile)
        {
            return;
        }
        
        size_t written = currentRecordingFile.write(reinterpret_cast<const uint8_t *>(samples), byteCount);
        if (written == byteCount)
        {
            recordedBytes += written;
            sdRecordingBytesSinceFlush += written;
            const unsigned long now = millis();
            if (sdRecordingBytesSinceFlush >= kSdRecordingFlushByteThreshold ||
                (now - sdRecordingLastFlushMs) >= kSdRecordingFlushIntervalMs)
            {
                currentRecordingFile.flush();
                sdRecordingBytesSinceFlush = 0;
                sdRecordingLastFlushMs = now;
            }
            return;
        }
        
        // Partial or failed write: count what was written, then finalize (header + close + rename) and switch to PSRAM
        if (written > 0)
        {
            recordedBytes += written;
            logDebugf("[Record] Audio write partial (written: %zu/%zu bytes), finalizing recording", written, byteCount);
        }
        else
        {
            logDebugf("[Record] Audio write failed (0 bytes), finalizing recording");
        }
        storage_recordWriteError();
        // Finalize so the .tmp gets valid WAV header, close and rename; do not enqueue for upload (incomplete)
        finalizeRecording(false, "sd_write_failure");
        // Switch to PSRAM so subsequent recordings use PSRAM until SD is remounted
        storage_switchToPsramOnFailure();
    }

    // Create recording path under /pending/YYYY/MM/DD/ (same layout as inbox) with .tmp extension.
    // Files use .tmp while being written, renamed to .wav in the same folder when complete.
    String createPendingRecordingPath()
    {
        time_t now = 0;
        time(&now);
        
        if (isEpochValid(now))
        {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            char path[96];
            strftime(path, sizeof(path), "/pending/%Y/%m/%d/%Y-%m-%d-%H-%M-%S.tmp", &timeinfo);
            return String(path);
        }
        else
        {
            // No valid clock: fixed tree + unique millis suffix in filename (still parseable for inbox move)
            char path[96];
            snprintf(path, sizeof(path), "/pending/1970/01/01/1970-01-01-00-00-00_%lu.tmp",
                     static_cast<unsigned long>(millis()));
            return String(path);
        }
    }

    void finalizeRecording(bool upload, const char *endReason)
    {
        if (!isRecording)
        {
            return;
        }

        String finishedRecordingPath = currentRecordingPath;
        uint32_t durationMs = 0;
        if (recordedBytes > 0 && appSettings.audio.sampleRate > 0)
        {
            durationMs = static_cast<uint32_t>((static_cast<uint64_t>(recordedBytes) * 1000ULL) /
                                               (appSettings.audio.sampleRate * sizeof(int16_t)));
        }

        const char *stopReason = (endReason != nullptr) ? endReason : "unknown";
        
        logDebugf("[Record] Finalizing recording (upload: %d, reason: %s, bytes: %zu, duration: %ums)", 
                  upload, stopReason, recordedBytes, durationMs);

        // PSRAM mode - finalize recording to PSRAM queue
        if (isRecordingModePsram())
        {
            if (psramRecordingBuffer != nullptr && recordedBytes > 0)
            {
                // Check if file should be discarded (too small)
                bool discard = upload && appSettings.audio.discardSmallFilesEnabled && 
                               durationMs < appSettings.audio.discardSmallFilesMinMs;
                
                if (discard)
                {
                    psramPool_return(psramRecordingBuffer);
                }
                else
                {
                    // Write WAV header at the beginning of buffer
                    const WaveHeader header = makeWaveHeader(recordedBytes, appSettings.audio.sampleRate);
                    memcpy(psramRecordingBuffer, &header, sizeof(header));
                    logDebugf("[Record] WAV header written to PSRAM buffer (size: %zu bytes)", 
                              sizeof(WaveHeader));
                    
                    // Add to PSRAM upload queue
                    size_t totalSize = sizeof(WaveHeader) + recordedBytes;
                    logDebugf("[Record] Adding to PSRAM queue (available_slots: %u, total_size: %zu)", 
                              psramQueue_getAvailableSlots(), totalSize);
                    if (!psramQueue_addRecording(psramRecordingBuffer, totalSize, durationMs, 
                                                  currentRecordingMaxDb, recordingStartEpoch, recordingStartMs))
                    {
                        // Queue full - free the buffer
                        logErrorf("[Record] PSRAM audio dropped without uploading due to queue being full (discarding recording)");
                        psramPool_return(psramRecordingBuffer);
                    }
                    else
                    {
                        // Calculate queue size from available slots
                        size_t availableSlots = psramQueue_getAvailableSlots();
                        size_t queueSize = (availableSlots < kPsramQueueMaxEntries) ? 
                                          (kPsramQueueMaxEntries - availableSlots) : 0;
                        logDebugf("[Record] Recording queued successfully (queue_size: %u)", 
                                  static_cast<unsigned>(queueSize));
                    }
                }
                
                psramRecordingBuffer = nullptr;
                psramRecordingCapacity = 0;
                psramRecordingOffset = 0;
            }
            
            finishedRecordingPath = "PSRAM";
        }
        else
        {
            // SD card mode - Write WAV header and close file; serialize /pending rename/remove
            if (currentRecordingFile)
            {
                const WaveHeader header = makeWaveHeader(recordedBytes, appSettings.audio.sampleRate);
                currentRecordingFile.seek(0);
                currentRecordingFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
                currentRecordingFile.flush();
                currentRecordingFile.close();
            }
            bool discardedSmallFile = false;
            bool renameToWavOk = !finishedRecordingPath.endsWith(".tmp");
            if (finishedRecordingPath.endsWith(".tmp"))
            {
                const String wavPath = finishedRecordingPath.substring(0, finishedRecordingPath.length() - 4) + ".wav";
                logDebugf("[Record] Renaming file from .tmp to .wav: %s -> %s",
                          finishedRecordingPath.c_str(), wavPath.c_str());
                constexpr int kRenameLockAttempts = 3;
                constexpr uint32_t kRenameRetryDelayMs = 100;
                for (int attempt = 0; attempt < kRenameLockAttempts && !renameToWavOk; ++attempt)
                {
                    if (uploadQueue_lockPendingDir(3000))
                    {
                        if (SD_MMC.rename(finishedRecordingPath, wavPath))
                        {
                            finishedRecordingPath = wavPath;
                            renameToWavOk = true;
                            logDebugf("[Record] File renamed successfully: %s", wavPath.c_str());
                        }
                        else
                        {
                            logErrorf("[Record] Failed to rename %s to .wav\n", finishedRecordingPath.c_str());
                        }
                        uploadQueue_unlockPendingDir();
                    }
                    else
                    {
                        logErrorf("[Record] Failed to lock /pending directory for rename (attempt %d/%d)\n",
                                  attempt + 1, kRenameLockAttempts);
                    }
                    if (!renameToWavOk && attempt + 1 < kRenameLockAttempts)
                    {
                        delay(kRenameRetryDelayMs);
                    }
                }
            }
            if (upload && appSettings.audio.discardSmallFilesEnabled && durationMs < appSettings.audio.discardSmallFilesMinMs &&
                renameToWavOk)
            {
                if (uploadQueue_lockPendingDir(3000))
                {
                    discardedSmallFile = true;
                    logDebugf("[Record] Discarding small file: %s (duration: %ums < min: %ums)",
                              finishedRecordingPath.c_str(), durationMs,
                              appSettings.audio.discardSmallFilesMinMs);
                    if (SD_MMC.remove(finishedRecordingPath))
                    {
                        logDebugf("[Record] Small file deleted successfully: %s",
                                  finishedRecordingPath.c_str());
                    }
                    else
                    {
                        logDebugf("[Record] Failed to delete small file: %s",
                                  finishedRecordingPath.c_str());
                    }
                    uploadQueue_unlockPendingDir();
                }
                else
                {
                    logErrorf("[Record] Failed to lock /pending directory for small-file discard\n");
                }
            }
            if (isRecordingModeSdCard() &&
                ((discardedSmallFile && finishedRecordingPath.startsWith("/pending/")) ||
                 (!discardedSmallFile && finishedRecordingPath.endsWith(".wav") &&
                  finishedRecordingPath.startsWith("/pending/"))))
            {
                uploadQueue_invalidateFilesystemPendingCountCache();
            }
            if (upload && !finishedRecordingPath.isEmpty() && !discardedSmallFile && renameToWavOk &&
                finishedRecordingPath.startsWith("/pending/") && finishedRecordingPath.endsWith(".wav"))
            {
                // Add to in-memory queue for prioritized upload (never enqueue .tmp — upload cannot consume tmp)
                time_t recordedEpoch = isEpochValid(recordingStartEpoch) ? recordingStartEpoch : calculateEpochFromMillis(recordingStartMs);
                unsigned long recordedMs = recordingStartMs;
                if (!sdCardMemoryQueue_addRecording(finishedRecordingPath.c_str(), recordedEpoch, recordedMs))
                {
                    logWarnf("[Record] Failed to add recording to memory queue (queue may be full): %s", finishedRecordingPath.c_str());
                }
            }
            else if (upload && finishedRecordingPath.endsWith(".tmp"))
            {
                logWarnf("[Record] Leaving .tmp on SD (rename failed); not enqueued for upload: %s",
                         finishedRecordingPath.c_str());
            }
            // Append /recordings/.../summary.json — catalog of recordings (predicted inbox path), not tied to upload.
            // Only when device is configured to record to SD card.
            if (isRecordingModeSdCard() && !discardedSmallFile && finishedRecordingPath.length() > 0 &&
                finishedRecordingPath.startsWith("/pending/") && finishedRecordingPath.endsWith(".wav"))
            {
                String predictedInbox;
                if (pendingWavToPredictedInboxPath(finishedRecordingPath, predictedInbox))
                {
                    const uint64_t wavFileSizeBytes =
                        static_cast<uint64_t>(sizeof(WaveHeader)) + static_cast<uint64_t>(recordedBytes);
                    RecordingsSummaryLine rsl;
                    rsl.pendingPath = finishedRecordingPath;
                    rsl.inboxPath = predictedInbox;
                    rsl.durationMs = durationMs;
                    rsl.endReason = String(stopReason);
                    rsl.peakDb = currentRecordingMaxDb;
                    rsl.sizeBytes = wavFileSizeBytes;
                    rsl.sampleRate = appSettings.audio.sampleRate;
                    recordings_appendSummaryLine(rsl);
                }
            }
            // File stays in /pending - upload task will pick it up (from memory queue first, then filesystem scan)
        }

        // Add to in-memory recent recordings list for SD playback navigation.
        if (finishedRecordingPath.startsWith("/pending/") && finishedRecordingPath.endsWith(".wav"))
        {
            const time_t recordedEpoch = isEpochValid(recordingStartEpoch) ? recordingStartEpoch : calculateEpochFromMillis(recordingStartMs);
            recentAddRecording(finishedRecordingPath, durationMs, recordedEpoch);
        }

#if defined(ECHO)
        // Repeater modes (ECHO-only)
        // - Simplex: after recording completes, transmit the recorded message.
        // - Duplex: TX is keyed during live audio; ensure we un-key once recording ends.
        if (appSettings.repeaterEnabled)
        {
            if (appSettings.repeaterMode == 2)
            {
                setPttOut(false);
            }
            else
            {
                if (upload && isStorageModeSdCard() && !finishedRecordingPath.isEmpty())
                {
                    ensureRepeaterSimplexQueue();
                    if (g_repeaterSimplexQueue != nullptr)
                    {
                        RepeaterSimplexRequest rr;
                        memset(&rr, 0, sizeof(rr));
                        strncpy(rr.path, finishedRecordingPath.c_str(), sizeof(rr.path) - 1);
                        (void)xQueueOverwrite(g_repeaterSimplexQueue, &rr);
                    }
                }
            }
        }
#endif

        // Update session stats
        sessionRecordingCount++;
        sessionTotalDurationMs += durationMs;
        g_healthMetrics.totalRecordedDurationSec += (durationMs / 1000);
        
        // Track last recording info
        lastRecordingEpoch = isEpochValid(recordingStartEpoch) ? recordingStartEpoch : calculateEpochFromMillis(recordingStartMs);
        lastRecordingDurationMs = durationMs;

        String summary = "[Record] ?? Stop recording : Audio level = ";
        summary += String(static_cast<double>(currentRecordingMaxDb), 2);
        summary += " dB, Duration = ";
        summary += String(static_cast<unsigned long>(durationMs));
        summary += " ms, End Reason = ";
        summary += stopReason;
        summary += ", Path = ";
        summary += finishedRecordingPath;
        logInfof("%s", summary.c_str());

        // Send record_end event
        DynamicJsonDocument endDoc(512);
        endDoc["path"] = finishedRecordingPath;
        if (isEpochValid(recordingStartEpoch))
        {
            endDoc["timestamp"] = formatIsoTimestamp(recordingStartEpoch, recordingStartMs % 1000);
        }
        endDoc["durationMs"] = durationMs;
        endDoc["sample_rate"] = appSettings.audio.sampleRate;
        if (currentRecordingMaxDb > -120.0f)
        {
            endDoc["peakDb"] = currentRecordingMaxDb;
        }
        if (stopReason != nullptr)
        {
            endDoc["endReason"] = String(stopReason);
        }
        String endJson;
        serializeJson(endDoc, endJson);
        sendEvent("record_end", endJson);
        
        // Print recording stop event to terminal in JSON format
        StaticJsonDocument<512> terminalDoc;
        terminalDoc["tm"] = getFormattedTimeWithTimezone();
        terminalDoc["ty"] = "event";
        terminalDoc["ms"] = "[Record] 🔴 Recording stopped";
        terminalDoc["ev"] = "record_end";
        terminalDoc["path"] = finishedRecordingPath;
        if (isEpochValid(recordingStartEpoch))
        {
            terminalDoc["ts"] = formatIsoTimestamp(recordingStartEpoch, recordingStartMs % 1000);
        }
        terminalDoc["dur"] = durationMs;
        terminalDoc["sr"] = appSettings.audio.sampleRate;
        if (currentRecordingMaxDb > -120.0f)
        {
            terminalDoc["db"] = currentRecordingMaxDb;
        }
        if (stopReason != nullptr)
        {
            terminalDoc["reason"] = String(stopReason);
        }
        terminalDoc["mc"] = getDeviceId();
        terminalDoc["si"] = getSessionId();
        String terminalJson;
        serializeJson(terminalDoc, terminalJson);
        serialWriteJsonAtomic(terminalJson);

        // Cleanup
        isRecording = false;
        isSampleRecording = false;
        recordedBytes = 0;
        currentRecordingPath = String();
        currentRecordingMaxDb = -120.0f;
        recordingStartEpoch = 0;
        currentPreRecordMsApplied = 0;
        resetSoundDetection();
    }

    void startRecording(float detectedDb, unsigned long detectedAtMs, bool prependPreRecord = true)
    {
        String message = "[Record] 🔴 Start Audio recording : Audio level = ";
        message += String(static_cast<double>(detectedDb), 2);
        message += " dB, Max = ";
        message += String(static_cast<unsigned long>(appSettings.audio.maxRecordingMs));
        message += " ms";
        logEventf("%s", message.c_str());

        if (!ensureStorage())
        {
            logErrorf("[Record] Storage unavailable, cannot start recording");
            return;
        }

        // PSRAM mode - allocate buffer from PSRAM
        if (isRecordingModePsram())
        {
            logDebugf("[Record] Starting PSRAM recording (max_bytes: %zu, queue_slots: %u)", 
                      (appSettings.audio.sampleRate * sizeof(int16_t) * std::min(appSettings.audio.maxRecordingMs, kPsramMaxRecordingMs)) / 1000,
                      psramQueue_getAvailableSlots());
            
            // Check if queue has available slots
            if (psramQueue_getAvailableSlots() == 0)
            {
                // Queue full - drop oldest entry to make room for new recording
                psramQueue_dropOldestEntry();
            }
            
            // Calculate max recording size based on max duration
            uint32_t maxRecordingMs = std::min(appSettings.audio.maxRecordingMs, kPsramMaxRecordingMs);
            size_t maxBytes = (appSettings.audio.sampleRate * sizeof(int16_t) * maxRecordingMs) / 1000;
            maxBytes = std::min(maxBytes, kPsramRecordingMaxBytes);

            // Take buffer from pool; if pool empty, drop oldest and retry
            psramRecordingBuffer = psramPool_take();
            while (psramRecordingBuffer == nullptr && psramQueue_getPendingCount() > 0)
            {
                psramQueue_dropOldestEntry();
                psramRecordingBuffer = psramPool_take();
            }
            if (psramRecordingBuffer == nullptr)
            {
                logErrorf("[Record] Failed to get PSRAM buffer from pool");
                logErrorf("[Record] PSRAM queue pending=%zu free_psram=%u", psramQueue_getPendingCount(), (unsigned)ESP.getFreePsram());
                return;
            }

            psramRecordingCapacity = maxBytes + sizeof(WaveHeader);
            psramRecordingOffset = sizeof(WaveHeader); // Leave space for header
            currentRecordingPath = "PSRAM";
             
        }
        else
        {
            // SD card mode - brief /pending lock for mkdir + open (retried if upload scan held mutex)
            bool pendingLocked = false;
            constexpr int kBeginLockAttempts = 3;
            constexpr uint32_t kBeginLockRetryDelayMs = 100;
            for (int attempt = 0; attempt < kBeginLockAttempts; ++attempt)
            {
                if (uploadQueue_lockPendingDir(3000))
                {
                    pendingLocked = true;
                    break;
                }
                if (attempt + 1 < kBeginLockAttempts)
                {
                    delay(kBeginLockRetryDelayMs);
                }
            }
            if (!pendingLocked)
            {
                logErrorf("[Record] Failed to lock /pending directory");
                return;
            }
            if (!g_sessionPendingRootEnsured)
            {
                if (!SD_MMC.exists(kPendingDir))
                {
                    if (!SD_MMC.mkdir(kPendingDir))
                    {
                        logErrorf("[Record] Failed to create /pending directory");
                        uploadQueue_unlockPendingDir();
                        return;
                    }
                    logDebugf("[Record] Created /pending directory");
                }
                g_sessionPendingRootEnsured = true;
            }
            currentRecordingPath = createPendingRecordingPath();
            {
                const int ls = currentRecordingPath.lastIndexOf('/');
                if (ls > 0)
                {
                    const String parentDir = currentRecordingPath.substring(0, ls);
                    if (parentDir != g_sessionEnsuredPendingDayDir)
                    {
                        if (!storage_ensureDirectoryPath(parentDir.c_str()))
                        {
                            logErrorf("[Record] Failed to create pending day directory: %s", parentDir.c_str());
                            uploadQueue_unlockPendingDir();
                            return;
                        }
                        g_sessionEnsuredPendingDayDir = parentDir;
                    }
                }
            }
            logDebugf("[Record] Starting SD card recording (path: %s)", 
                      currentRecordingPath.c_str());
            constexpr int kMaxStorageRetries = 3;
            bool fileOpened = false;
            for (int attempt = 0; attempt < kMaxStorageRetries && !fileOpened; ++attempt)
            {
                if (attempt > 0)
                {
                    delay(100 * attempt);
                    storage_recordWriteError();
                }
                currentRecordingFile = SD_MMC.open(currentRecordingPath, FILE_WRITE);
                if (currentRecordingFile)
                {
                    logDebugf("[Record] File opened for recording (path: %s, attempt: %d)", 
                              currentRecordingPath.c_str(), attempt + 1);
                    fileOpened = true;
                    break;
                }
            }
            uploadQueue_unlockPendingDir();
            if (!fileOpened)
            {
                logErrorf("[Record] Failed to open file for recording");
                return;
            }

            // Write placeholder header
            const WaveHeader header = makeWaveHeader(0, appSettings.audio.sampleRate);
            const size_t written = currentRecordingFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
            currentRecordingFile.flush();
            if (written != sizeof(header))
            {
                logErrorf("[Record] Failed to write WAV header");
                currentRecordingFile.close();
                storage_recordWriteError();
                return;
            }
            sdRecordingLastFlushMs = millis();
            sdRecordingBytesSinceFlush = 0;
            logDebugf("[Record] WAV header written to file (size: %zu bytes)", sizeof(header));
        }

        isRecording = true;
        recordingStartMs = detectedAtMs;
        lastSoundMs = detectedAtMs;
        
        time(&recordingStartEpoch);
        if (!isEpochValid(recordingStartEpoch))
        {
            recordingStartEpoch = 0;
        }
        recordedBytes = 0;
        currentRecordingMaxDb = -120.0f;
        currentRecordingMaxDb = std::max(currentRecordingMaxDb, detectedDb);
        currentPreRecordMsApplied = 0;

        if (prependPreRecord)
        {
            const uint32_t preAppliedMs = appendPreRecordAudio();
            if (preAppliedMs > 0)
            {
                currentPreRecordMsApplied = preAppliedMs;
                if (recordingStartMs >= preAppliedMs)
                {
                    recordingStartMs -= preAppliedMs;
                }
                else
                {
                    recordingStartMs = 0;
                }
                logDebugf("[Record] Pre-record audio applied (%ums)", preAppliedMs);
            }
        }

        logDebugf("[Record] Recording started successfully (mode: %s, path: %s, pre_record: %ums)", 
                  isRecordingModePsram() ? "PSRAM" : "SD", 
                  currentRecordingPath.c_str(), 
                  currentPreRecordMsApplied);

        DynamicJsonDocument recDoc(256);
        recDoc["path"] = currentRecordingPath;
        recDoc["timestamp"] = formatIsoTimestamp(recordingStartEpoch, recordingStartMs % 1000);
        recDoc["sample_rate"] = appSettings.audio.sampleRate;
        String recJson;
        serializeJson(recDoc, recJson);
        sendEvent("record_begin", recJson);
        
        // Print recording start event to terminal in JSON format
        StaticJsonDocument<512> terminalDoc;
        terminalDoc["tm"] = getFormattedTimeWithTimezone();
        terminalDoc["ty"] = "event";
        terminalDoc["ms"] = "[Record] 🟢 Recording started";
        terminalDoc["ev"] = "record_begin";
        terminalDoc["path"] = currentRecordingPath;
        terminalDoc["ts"] = formatIsoTimestamp(recordingStartEpoch, recordingStartMs % 1000);
        terminalDoc["sr"] = appSettings.audio.sampleRate;
        terminalDoc["mc"] = getDeviceId();
        terminalDoc["si"] = getSessionId();
        String terminalJson;
        serializeJson(terminalDoc, terminalJson);
        serialWriteJsonAtomic(terminalJson);
    }

    void monitorAndRecordAudio()
    {
        recorder_applySpeakerSettings();

#if defined(ECHO)
        // Cloud recording playback (MQTT play_cloud / play_transmit): download + output, pause upload/record.
        if (g_cloudPlayQueue != nullptr)
        {
            CloudPlayRequest cpr;
            if (xQueueReceive(g_cloudPlayQueue, &cpr, 0) == pdTRUE)
            {
                if (cpr.fileName[0] != '\0')
                {
                    CloudPlayResourceGuard guard;
                    const bool wantTransmit = cpr.transmit;
                    const bool keyRadio = wantTransmit && appSettings.transmitEnabled;
                    const int16_t playVolume = wantTransmit
                        ? static_cast<int16_t>(appSettings.transmitVolume)
                        : -1;
                    const char *playLabel = wantTransmit ? "Cloud transmit" : "Cloud play";

                    logWarnf("[Playback] %s start: %s", playLabel, cpr.fileName);
                    esp_task_wdt_reset();

                    if (wantTransmit && !appSettings.transmitEnabled)
                    {
                        logWarnf("[Playback] TX disabled; playing without PTT: %s", cpr.fileName);
                        recorder_beep(250, 250, 2, false);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_task_wdt_reset();
                    }

                    const String path = downloadFile(String(cpr.fileName), getDeviceId());
                    esp_task_wdt_reset();
                    if (path.length() > 0)
                    {
                        File probe = SD_MMC.open(path.c_str(), FILE_READ);
                        if (probe)
                        {
                            const size_t sz = probe.size();
                            probe.close();
                            if (sz > 44)
                            {
                                uint32_t durMs = static_cast<uint32_t>((sz - 44) / 32);
                                durMs = std::min(durMs + 3000u, 300000u);
                                recorder_inhibitRecordingForMs(durMs);
                            }
                        }
                        g_isPlayingFile = true;
                        if (keyRadio)
                        {
                            setPttOut(true);
                        }
                        (void)playWavFileBlocking(path.c_str(), playVolume, !wantTransmit);
                        if (keyRadio)
                        {
                            setPttOut(false);
                        }
                        g_isPlayingFile = false;
                        logWarnf("[Playback] %s done: %s", playLabel, path.c_str());
                    }
                    else
                    {
                        logWarnf("[Playback] Cloud download failed: %s", cpr.fileName);
                        StaticJsonDocument<512> terminalDoc;
                        terminalDoc["tm"] = getFormattedTimeWithTimezone();
                        terminalDoc["ty"] = "event";
                        terminalDoc["ms"] = wantTransmit
                            ? "[Playback] Cloud transmit download failed"
                            : "[Playback] Playback download failed - cannot play on speaker";
                        terminalDoc["ev"] = wantTransmit ? "play_transmit_failed" : "play_cloud_failed";
                        terminalDoc["file"] = cpr.fileName;
                        terminalDoc["mc"] = getDeviceId();
                        terminalDoc["si"] = getSessionId();
                        String terminalJson;
                        serializeJson(terminalDoc, terminalJson);
                        serialWriteJsonAtomic(terminalJson);
                    }
                    mqtt_clearCommandInProgress();
                }
                else
                {
                    mqtt_clearCommandInProgress();
                }
            }
        }

        // Simplex repeater pending playback (TX after record ends).
        if (appSettings.repeaterEnabled && appSettings.repeaterMode == 1 && g_repeaterSimplexQueue != nullptr)
        {
            RepeaterSimplexRequest rr;
            if (xQueueReceive(g_repeaterSimplexQueue, &rr, 0) == pdTRUE)
            {
                if (rr.path[0] != '\0')
                {
                    if (!appSettings.transmitEnabled)
                    {
                        logWarnf("[Repeater] Simplex requested but TX is disabled; skipping: %s", rr.path);
                    }
                    else
                    {
                        g_isPlayingFile = true;
                        setPttOut(true);
                        (void)playWavFileBlocking(
                            rr.path,
                            static_cast<int16_t>(appSettings.transmitVolume),
                            false);
                        setPttOut(false);
                        g_isPlayingFile = false;
                    }
                }
            }
        }
#endif

        // Priority: play one pending beep before processing audio.
        if (g_beepQueue != nullptr)
        {
            BeepRequest req;
            if (xQueueReceive(g_beepQueue, &req, 0) == pdTRUE)
            {
                g_isBeeping = true;
                playBeepBlocking(req);
                g_isBeeping = false;
            }
        }

        // Playback requests (NEXT/PREV). These override live passthrough while playing.
        if (g_playbackQueue != nullptr)
        {
            PlaybackRequest pr;
            if (xQueueReceive(g_playbackQueue, &pr, 0) == pdTRUE)
            {
                ensureRecentList();
                int idx = (g_recentCursor >= 0) ? g_recentCursor : static_cast<int>(g_recentHead);
                if (pr.cmd == PlaybackCommand::Next)
                {
                    idx = findNextUnplayedFrom(idx);
                }
                else
                {
                    idx = findPrevAnyFrom(idx);
                }

                if (idx < 0 || g_recent == nullptr || !g_recent[idx].valid)
                {
                    // No messages: two "bad" beeps
                    BeepRequest br;
                    br.lengthMs = 250;
                    br.delayMs = 250;
                    br.count = 2;
                    br.good = false;
                    playBeepBlocking(br);
                }
                else
                {
                    g_recentCursor = idx;
                    g_recent[idx].played = true;

                    // For now: depict "next message" with a short beep, then play.
                    BeepRequest br;
                    br.lengthMs = 200;
                    br.delayMs = 200;
                    br.count = 1;
                    br.good = true;
                    playBeepBlocking(br);
                    g_isPlayingFile = true;
                    (void)playWavFileBlocking(g_recent[idx].path);
                    g_isPlayingFile = false;
                }
            }
        }

        // Notification WAV playback requests (play-at/after received time).
        if (g_wavPlayQueue != nullptr)
        {
            WavPlayRequest wr;
            if (xQueuePeek(g_wavPlayQueue, &wr, 0) == pdTRUE)
            {
                const uint32_t nowMs = millis();
                if (wr.playAtMs == 0 || static_cast<int32_t>(nowMs - wr.playAtMs) >= 0)
                {
                    (void)xQueueReceive(g_wavPlayQueue, &wr, 0);
                    if (wr.path[0] != '\0')
                    {
                        g_isPlayingFile = true;
                        (void)playWavFileBlocking(wr.path);
                        g_isPlayingFile = false;
                    }
                }
            }
        }

        // Morse playback request.
        if (g_morseQueue != nullptr)
        {
            MorseRequest mr;
            if (xQueueReceive(g_morseQueue, &mr, 0) == pdTRUE)
            {
                if (mr.cancel)
                {
                    g_morseCancelFlag = true;
                }
                else
                {
                    g_morseCancelFlag = false;
                    g_isPlayingFile = true;
                    playMorseBlocking(mr);
                    g_isPlayingFile = false;
                    g_morseCancelFlag = false;
                }
            }
        }

        // If we are in the inhibit window, drain input and skip sound detection/recording.
        // This prevents startup beeps/WAVs from triggering recordings.
        const uint32_t nowMsInhibit = millis();
        if (g_recordInhibitUntilMs != 0 && static_cast<int32_t>(nowMsInhibit - g_recordInhibitUntilMs) < 0)
        {
            if (isRecording)
            {
                finalizeRecording(false, "startup_inhibit");
            }
            resetSoundDetection();
            clearPreRecordBuffers();
            (void)kit.read(audioBuffer, sizeof(audioBuffer), pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(5));
            return;
        }

        const size_t bytesRead = kit.read(audioBuffer, sizeof(audioBuffer), pdMS_TO_TICKS(200));
        if (bytesRead == 0)
        {
            return;
        }

        const size_t monoSamples = splitAudioBufferForRecording(audioBuffer, bytesRead, recordingBuffer, kMonoBufferSamples);
        if (monoSamples == 0)
        {
            return;
        }

        // Local speaker or line-out (radio TX) playback of captured input.
        // Duplex repeater TX routes to line-out only (PA off); normal monitoring uses the speaker.
        if (!g_isBeeping && !g_isPlayingFile)
        {
#if defined(ECHO)
            const bool duplexTransmitMode = appSettings.repeaterEnabled &&
                                            appSettings.repeaterMode == 2 &&
                                            appSettings.transmitEnabled;
#else
            const bool duplexTransmitMode = false;
#endif
            const bool useSpeaker = appSettings.speakerEnabled && !duplexTransmitMode;
            const bool useLineOutOnly = duplexTransmitMode;
            if (useSpeaker || useLineOutOnly)
            {
                static int16_t stereoOut[kMonoBufferSamples * 2];
                const size_t outSamples = std::min(monoSamples, static_cast<size_t>(kMonoBufferSamples));
                for (size_t i = 0; i < outSamples; ++i)
                {
                    const int16_t s = recordingBuffer[i];
                    stereoOut[i * 2] = s;
                    stereoOut[i * 2 + 1] = s;
                }
                if (useLineOutOnly)
                {
                    uint8_t vol = appSettings.transmitVolume;
                    if (vol > 100) vol = 100;
                    kit.setSpeakerActive(false);
                    kit.setMute(false);
                    kit.setVolume(vol);
                }
                // Best-effort write; keep timeout small so recording isn't impacted.
                (void)kit.write(stereoOut, outSamples * 2 * sizeof(int16_t), pdMS_TO_TICKS(10));
                if (useLineOutOnly)
                {
                    kit.setSpeakerActive(true);
                    kit.setMute(!appSettings.speakerEnabled);
                    if (appSettings.speakerEnabled)
                    {
                        kit.setVolume(appSettings.speakerVolume);
                    }
                }
            }
        }

        const size_t monoBytes = monoSamples * sizeof(int16_t);
        const float level = calculateAudioLevel(recordingBuffer, monoSamples);
        const float chunkDb = calculateDb(recordingBuffer, monoSamples);
        const float smoothedDb = pushDbAndGetAverage(chunkDb);
        if (chunkDb > currentRecordingMaxDb)
        {
            currentRecordingMaxDb = chunkDb;
        }
        
        // Calculate peak sample and dynamic range utilization
        float utilizationPercent = 0.0f;
        const int16_t peakSample = calculatePeakSample(recordingBuffer, monoSamples, utilizationPercent);
        
        // Update audio level tracking for VU meter
        currentAudioLevel = level;
        currentAudioDb = smoothedDb;
        if (level < minAudioLevel) minAudioLevel = level;
        if (level > maxAudioLevel) maxAudioLevel = level;
        if (smoothedDb < minAudioDb) minAudioDb = smoothedDb;
        if (smoothedDb > maxAudioDb) maxAudioDb = smoothedDb;
        audioLevelSum += level;
        audioLevelSampleCount++;
        
        // Update peak sample and dynamic range tracking
        currentPeakSample = peakSample;
        currentDynamicRangeUtil = utilizationPercent;
        if (peakSample > maxPeakSample) maxPeakSample = peakSample;
        if (utilizationPercent > maxDynamicRangeUtil) maxDynamicRangeUtil = utilizationPercent;

        const unsigned long now = millis();
        const float thresholdDb = appSettings.audio.audioThreshold - 80;
        const bool isSound = (smoothedDb >= thresholdDb) && (level > 0.0f);

#if defined(ECHO)
        // Duplex repeater: key TX as soon as sound starts (line-out already mirrors input).
        if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2 && appSettings.transmitEnabled && !g_isBeeping && !g_isPlayingFile)
        {
            if (isSound)
            {
                setPttOut(true);
            }
        }
#endif

        // WebSocket live PCM: every captured mono chunk including silence/sub-threshold; not gated on isRecording or isSound.
        if (liveAudioFeedEnabled.load(std::memory_order_relaxed) && liveAudioCallback != nullptr)
        {
            liveAudioCallback(recordingBuffer, monoSamples);
        }

        if (recordingPausedForLiveSession.load(std::memory_order_relaxed))
        {
            if (isRecording)
            {
                finalizeRecording(false, "live_audio_pause");
#if defined(ECHO)
                if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2)
                {
                    setPttOut(false);
                }
#endif
            }
            pushPreRecordSamples(recordingBuffer, monoSamples);
            return;
        }

#if defined(ECHO)
        if (!mqttLineInRecordingEnabled.load(std::memory_order_relaxed))
        {
            if (isRecording)
            {
                finalizeRecording(false, "mqtt_record_line_in");
                if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2)
                {
                    setPttOut(false);
                }
            }
            pushPreRecordSamples(recordingBuffer, monoSamples);
            return;
        }
#endif

        if (!isRecording)
        {
            if (!isSound)
            {
#if defined(ECHO)
                // Duplex repeater: if no sound and we're idle, release TX.
                if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2)
                {
                    setPttOut(false);
                }
#endif
                pushPreRecordSamples(recordingBuffer, monoSamples);
                return;
            }
            startRecording(smoothedDb, now);
            if (!isRecording)
            {
                pushPreRecordSamples(recordingBuffer, monoSamples);
                return;
            }
        }

        size_t peakRecordingBytes;
        if (isRecordingModePsram())
        {
            const uint32_t maxMs = std::min(appSettings.audio.maxRecordingMs, kPsramMaxRecordingMs);
            peakRecordingBytes = static_cast<size_t>(
                (static_cast<uint64_t>(appSettings.audio.sampleRate) * sizeof(int16_t) *
                 static_cast<uint64_t>(maxMs)) /
                1000ULL);
            peakRecordingBytes = std::min(peakRecordingBytes, kPsramRecordingMaxBytes);
        }
        else
        {
            peakRecordingBytes = static_cast<size_t>(
                (static_cast<uint64_t>(appSettings.audio.sampleRate) * sizeof(int16_t) *
                 static_cast<uint64_t>(appSettings.audio.maxRecordingMs)) /
                1000ULL);
        }

        appendAudioSamples(recordingBuffer, monoBytes);

        if (isSound)
        {
            lastSoundMs = now;
        }

        const unsigned long elapsed = now - recordingStartMs;
        const bool silenceExpired = (elapsed >= appSettings.audio.minRecordingMs) &&
                                    ((now - lastSoundMs) >= appSettings.audio.silenceThresholdMs);

        bool maxDurationReached = false;
        if (isRecordingModePsram())
        {
            const size_t preRollBytes =
                (static_cast<uint64_t>(appSettings.audio.sampleRate) * sizeof(int16_t) *
                 static_cast<uint64_t>(currentPreRecordMsApplied)) /
                1000ULL;
            const size_t liveBytes =
                recordedBytes > preRollBytes ? (recordedBytes - preRollBytes) : 0;
            maxDurationReached = liveBytes >= peakRecordingBytes;
        }
        else
        {
            uint32_t maxRecordingMs = appSettings.audio.maxRecordingMs;
            maxDurationReached = elapsed >= maxRecordingMs;
        }

        // Sample recordings ignore silence detection - only stop at max duration.
        if (silenceExpired && !isSampleRecording)
        {
            finalizeRecording(true, "silence");
#if defined(ECHO)
            if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2)
            {
                setPttOut(false);
            }
#endif
            pushPreRecordSamples(recordingBuffer, monoSamples);
            return;
        }

        if (maxDurationReached)
        {
            finalizeRecording(true, "max_duration");
#if defined(ECHO)
            if (appSettings.repeaterEnabled && appSettings.repeaterMode == 2)
            {
                setPttOut(false);
            }
#endif
            // For sample recordings, don't start a new recording automatically
            if (isSound && !isSampleRecording)
            {
                startRecording(smoothedDb, now, false);
            }
            pushPreRecordSamples(recordingBuffer, monoSamples);
            return;
        }

        pushPreRecordSamples(recordingBuffer, monoSamples);
    }

    uint8_t codecGainLevelFromDb(int gainDb)
    {
        uint8_t bestIndex = 0;
        int bestDiff = gainDb - kCodecGainDbValues[0];
        if (bestDiff < 0)
        {
            bestDiff = -bestDiff;
        }

        for (size_t i = 1; i < kCodecGainLevelCount; ++i)
        {
            int diff = gainDb - kCodecGainDbValues[i];
            if (diff < 0)
            {
                diff = -diff;
            }
            if (diff < bestDiff)
            {
                bestDiff = diff;
                bestIndex = static_cast<uint8_t>(i);
            }
        }

        return bestIndex;
    }

    void applyCodecGainLevel(uint8_t level)
    {
        if (level >= kCodecGainLevelCount)
        {
            level = static_cast<uint8_t>(kCodecGainLevelCount - 1);
        }

        es_mic_gain_t target = kCodecGainEnums[level];
        esp_err_t err = es8388_set_mic_gain(target);
        if (err == ESP_OK)
        {
            lastAppliedGainLevel = level;
            const int8_t gainDb = kCodecGainDbValues[level];
           
        }
         
    }
}

void updateCodecGainFromSettings()
{
    const uint8_t level = codecGainLevelFromDb(appSettings.audio.codecGainDb);

    if (!codecReady)
    {
        return;
    }

    if (lastAppliedGainLevel == level)
    {
        return;
    }

    applyCodecGainLevel(level);
}

void recorder_stopActiveRecording(const char *reason)
{
    if (!reason)
    {
        reason = "manual";
    }

    if (!isRecording)
    {
        return;
    }

    finalizeRecording(false, reason);
}

void startAudioCodec()
{
    auto cfg = kit.defaultConfig();
    cfg.adc_input = AUDIO_HAL_ADC_INPUT_LINE1;
    cfg.sample_rate = AUDIO_HAL_08K_SAMPLES;
    cfg.sd_active = false;

    codecReady = false;
    lastAppliedGainLevel = 255;

    if (!kit.begin(cfg))
    {
        return;
    }

    codecReady = true;
    recorder_applySpeakerSettings();
    updateCodecGainFromSettings();
}

void recorder_applySpeakerSettings()
{
    if (!codecReady)
    {
        return;
    }

    const bool enabled = appSettings.speakerEnabled;
    uint8_t volume = appSettings.speakerVolume;
    if (volume > 100) volume = 100;

    if (enabled == lastAppliedSpeakerEnabled && volume == lastAppliedSpeakerVolume)
    {
        return;
    }

    lastAppliedSpeakerEnabled = enabled;
    lastAppliedSpeakerVolume = volume;

    kit.setVolume(volume);
    kit.setMute(!enabled);
    logInfof("[Speaker] %s, volume=%u", enabled ? "enabled" : "disabled", static_cast<unsigned>(volume));
}

void recorder_beep(int beepLengthMs, int beepDelayMs, int beepCount, bool beepGood)
{
    if (beepCount <= 0 || beepLengthMs <= 0)
    {
        return;
    }

    ensureBeepQueue();
    if (g_beepQueue == nullptr)
    {
        return;
    }

    BeepRequest req;
    req.lengthMs = beepLengthMs;
    req.delayMs = beepDelayMs;
    req.count = beepCount;
    req.good = beepGood;
    (void)xQueueSend(g_beepQueue, &req, 0);
}

void recorder_requestPlayNextRecording()
{
    ensurePlaybackQueue();
    if (g_playbackQueue == nullptr)
    {
        return;
    }
    PlaybackRequest r;
    r.cmd = PlaybackCommand::Next;
    // Coalesce if user is mashing buttons.
    (void)xQueueOverwrite(g_playbackQueue, &r);
}

void recorder_requestPlayPrevRecording()
{
    ensurePlaybackQueue();
    if (g_playbackQueue == nullptr)
    {
        return;
    }
    PlaybackRequest r;
    r.cmd = PlaybackCommand::Prev;
    // Coalesce if user is mashing buttons.
    (void)xQueueOverwrite(g_playbackQueue, &r);
}

void recorder_requestPlayWavAtOrAfter(const String& wavPath, time_t receivedEpochUtcSeconds)
{
    if (wavPath.length() == 0)
    {
        return;
    }
    ensureWavPlayQueue();
    if (g_wavPlayQueue == nullptr)
    {
        return;
    }

    WavPlayRequest wr;
    memset(&wr, 0, sizeof(wr));
    strncpy(wr.path, wavPath.c_str(), sizeof(wr.path) - 1);
    wr.playAtMs = 0;

    // If time is valid and received time is in the future, delay until that time.
    if (receivedEpochUtcSeconds > 0)
    {
        const time_t nowEpoch = time(nullptr);
        if (nowEpoch > 0 && receivedEpochUtcSeconds > nowEpoch)
        {
            const uint32_t deltaMs = static_cast<uint32_t>(
                std::min<int64_t>(static_cast<int64_t>(receivedEpochUtcSeconds - nowEpoch) * 1000LL, 60LL * 1000LL));
            wr.playAtMs = millis() + deltaMs;
        }
    }

    // Coalesce: overwrite any prior request.
    (void)xQueueOverwrite(g_wavPlayQueue, &wr);
}

void recorder_inhibitRecordingForMs(uint32_t durationMs)
{
    if (durationMs == 0)
    {
        return;
    }
    const uint32_t now = millis();
    const uint32_t until = now + durationMs;
    // Extend only (never shorten) to avoid races with multiple startup sounds.
    if (g_recordInhibitUntilMs == 0 || static_cast<int32_t>(until - g_recordInhibitUntilMs) > 0)
    {
        g_recordInhibitUntilMs = until;
    }
}

void recorder_requestPlayMorse(const String& text, uint16_t wpm, uint16_t toneHz, uint8_t volume, uint8_t repeat)
{
    ensureMorseQueue();
    if (g_morseQueue == nullptr)
    {
        return;
    }

    MorseRequest mr;
    memset(&mr, 0, sizeof(mr));
    mr.cancel = false;
    strncpy(mr.text, text.c_str(), sizeof(mr.text) - 1);
    mr.wpm = wpm;
    mr.toneHz = toneHz;
    mr.volume = volume;
    mr.repeat = repeat;

    (void)xQueueOverwrite(g_morseQueue, &mr);
}

void recorder_cancelMorse()
{
    ensureMorseQueue();
    if (g_morseQueue == nullptr)
    {
        return;
    }
    MorseRequest mr;
    memset(&mr, 0, sizeof(mr));
    mr.cancel = true;
    (void)xQueueOverwrite(g_morseQueue, &mr);
}

bool recorder_playAudioFilePlaceholder(const String& pathOrId)
{
    logWarnf("[Playback] Placeholder: play audio file '%s'", pathOrId.c_str());
    return false;
}

void recorder_requestPlayCloud(const String& fileName)
{
#if defined(ECHO)
    if (fileName.length() == 0)
    {
        return;
    }
    ensureCloudPlayQueue();
    if (g_cloudPlayQueue == nullptr)
    {
        return;
    }

    CloudPlayRequest cpr;
    memset(&cpr, 0, sizeof(cpr));
    strncpy(cpr.fileName, fileName.c_str(), sizeof(cpr.fileName) - 1);
    cpr.transmit = false;
    (void)xQueueOverwrite(g_cloudPlayQueue, &cpr);
    logWarnf("[Playback] Cloud play queued: %s", cpr.fileName);
#else
    (void)fileName;
#endif
}

void recorder_requestPlayTransmit(const String& fileName)
{
#if defined(ECHO)
    if (fileName.length() == 0)
    {
        return;
    }
    ensureCloudPlayQueue();
    if (g_cloudPlayQueue == nullptr)
    {
        return;
    }

    CloudPlayRequest cpr;
    memset(&cpr, 0, sizeof(cpr));
    strncpy(cpr.fileName, fileName.c_str(), sizeof(cpr.fileName) - 1);
    cpr.transmit = true;
    (void)xQueueOverwrite(g_cloudPlayQueue, &cpr);
    logWarnf("[Playback] Cloud transmit queued: %s", cpr.fileName);
#else
    (void)fileName;
#endif
}

void recordTask(void *pvParameters)
{
    (void)pvParameters;
    
    uint32_t watchdogFeedCounter = 0;
    constexpr uint32_t kWatchdogFeedInterval = 20; // Feed every 20 iterations (~20ms)

    while (true)
    {
        // Recording is independent of upload success or cloud connectivity: with credentials configured,
        // we keep capturing (including SD mode per settings) while files queue in /pending until upload works again.
        // Check if WiFi credentials are configured
        // When in AP mode (no credentials), skip recording entirely and just feed watchdog
        if (!network_hasAnyWiFiCredentials())
        {
            // No WiFi credentials - feed watchdog and wait (no recording in AP mode)
            if (++watchdogFeedCounter >= kWatchdogFeedInterval)
            {
                esp_task_wdt_reset();
                watchdogFeedCounter = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait 100ms when in AP mode
            continue; // Skip monitorAndRecordAudio() - no recording without WiFi credentials
        }
        
        // WiFi credentials available - proceed with recording
        monitorAndRecordAudio();
        
        // Feed watchdog every 20 iterations (more frequently)
        if (++watchdogFeedCounter >= kWatchdogFeedInterval)
        {
            esp_task_wdt_reset();
            watchdogFeedCounter = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void recorder_incrementErrorCount()
{
    sessionErrorCount++;
}

void recorder_incrementUploadedCount()
{
    sessionUploadedCount++;
}

RecordingStats recorder_getSessionStats()
{
    RecordingStats stats;
    
    // Check if we need to reset session (new day)
    time_t now;
    time(&now);
    struct tm nowTm;
    gmtime_r(&now, &nowTm);
    struct tm sessionTm;
    gmtime_r(&sessionStartEpoch, &sessionTm);
    
    if (sessionStartEpoch == 0 || 
        nowTm.tm_year != sessionTm.tm_year || 
        nowTm.tm_yday != sessionTm.tm_yday) {
        // New day, reset session counters
        sessionStartEpoch = now;
        sessionRecordingCount = 0;
        sessionTotalDurationMs = 0;
        sessionUploadedCount = 0;
        sessionErrorCount = 0;
        // Reset audio level stats
        minAudioLevel = 100.0f;
        maxAudioLevel = 0.0f;
        minAudioDb = 0.0f;
        maxAudioDb = -120.0f;
        audioLevelSum = 0.0f;
        audioLevelSampleCount = 0;
        maxPeakSample = 0;
        maxDynamicRangeUtil = 0.0f;
    }
    
    stats.recordingCount = sessionRecordingCount;
    stats.totalDurationMs = sessionTotalDurationMs;
    stats.uploadedCount = sessionUploadedCount;
    stats.errorCount = sessionErrorCount;
    
    return stats;
}

AudioLevelStats recorder_getAudioLevelStats()
{
    AudioLevelStats stats;
    stats.currentLevel = currentAudioLevel;
    stats.currentDb = currentAudioDb;
    stats.minLevel = minAudioLevel;
    stats.maxLevel = maxAudioLevel;
    stats.minDb = minAudioDb;
    stats.maxDb = maxAudioDb;
    if (audioLevelSampleCount > 0) {
        stats.averageLevel = audioLevelSum / static_cast<float>(audioLevelSampleCount);
    }
    stats.peakSample = currentPeakSample;
    stats.dynamicRangeUtil = currentDynamicRangeUtil;
    stats.maxPeakSample = maxPeakSample;
    stats.maxDynamicRangeUtil = maxDynamicRangeUtil;
    return stats;
}

bool recorder_isRecording()
{
    return isRecording;
}

time_t recorder_getLastRecordingEpoch()
{
    return lastRecordingEpoch;
}

uint32_t recorder_getLastRecordingDurationMs()
{
    return lastRecordingDurationMs;
}

String recorder_getCurrentRecordingPath()
{
    // Only return path if recording in SD card mode (PSRAM mode returns "PSRAM" which we ignore)
    if (isRecording && isStorageModeSdCard() && !currentRecordingPath.isEmpty() && currentRecordingPath != "PSRAM") {
        return currentRecordingPath;
    }
    return String();
}

bool recorder_startSampleRecording()
{
#if defined(ECHO)
    if (!mqttLineInRecordingEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
#endif

    if (isRecording)
    {
        return false;
    }
    
    if (!ensureStorage())
    {
        logErrorf("[Sample] Storage unavailable, cannot start recording");
        return false;
    }
    
    float detectedDb = currentAudioDb;
    if (detectedDb < -120.0f)
    {
        detectedDb = -50.0f;
    }
    
    const unsigned long now = millis();
    isSampleRecording = true;
    startRecording(detectedDb, now);
    
    if (isRecording)
    {
        return true;
    }
    else
    {
        isSampleRecording = false;
        return false;
    }
}

void recorder_setLiveAudioCallback(LiveAudioCallback callback)
{
    liveAudioCallback = callback;
}

void recorder_setLiveAudioFeedEnabled(bool enabled)
{
    liveAudioFeedEnabled.store(enabled, std::memory_order_relaxed);
}

void recorder_setRecordingPausedForLiveSession(bool paused)
{
    recordingPausedForLiveSession.store(paused, std::memory_order_relaxed);
}

bool recorder_isRecordingPausedForLiveSession()
{
    return recordingPausedForLiveSession.load(std::memory_order_relaxed);
}

void recorder_setLineInRecordingEnabled(bool enabled)
{
    const bool wasEnabled = mqttLineInRecordingEnabled.exchange(enabled, std::memory_order_relaxed);
    if (wasEnabled == enabled)
    {
        return;
    }
    logWarnf("[Record] MQTT record_line_in -> %s", enabled ? "enabled" : "disabled");
    if (!enabled)
    {
        recorder_stopActiveRecording("mqtt_record_line_in");
    }
}

bool recorder_isLineInRecordingEnabled()
{
    return mqttLineInRecordingEnabled.load(std::memory_order_relaxed);
}

void recorder_invalidatePendingDirectoryCache()
{
    g_sessionPendingRootEnsured = false;
    g_sessionEnsuredPendingDayDir = String();
}

