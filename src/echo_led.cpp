#if defined(ECHO)

#include "echo_led.h"

#include <Adafruit_NeoPixel.h>

#include "echo_hw.h"
#include "echo_led_legacy.h"
#include "common.h"
#include "logger.h"
#include "main.h"
#include "network.h"
#include "recorder.h"
#include "boondock_server.h"
#include <WiFi.h>

namespace
{
    enum LEDState
    {
        LED_OFF = 0,
        LED_SOLID,
        LED_BREATHING,
        LED_FLASHING
    };

    struct LEDInfo
    {
        LEDState state = LED_OFF;
        uint32_t color = 0;
        unsigned long lastUpdate = 0;
        int breathCycleMs = 3000;
        int flashOnDurationMs = 500;
        int flashOffDurationMs = 500;
    };

    Adafruit_NeoPixel g_pixels(NUMPIXELS, static_cast<uint8_t>(GPIO_NEOPIXEL), NEO_GRB + NEO_KHZ800);
    LEDInfo g_ledInfo[NUMPIXELS];

    LEDState g_currentStates[NUMPIXELS] = {LED_OFF, LED_OFF, LED_OFF};
    uint32_t g_currentColors[NUMPIXELS] = {0, 0, 0};
    int g_currentOnMs[NUMPIXELS] = {0, 0, 0};
    int g_currentOffMs[NUMPIXELS] = {0, 0, 0};

    volatile bool g_deviceReady = false;

    uint32_t dimColor(uint32_t color, uint8_t brightness)
    {
        const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
        const uint8_t b = static_cast<uint8_t>(color & 0xFF);
        const uint8_t rd = static_cast<uint8_t>((static_cast<uint16_t>(r) * brightness) >> 8);
        const uint8_t gd = static_cast<uint8_t>((static_cast<uint16_t>(g) * brightness) >> 8);
        const uint8_t bd = static_cast<uint8_t>((static_cast<uint16_t>(b) * brightness) >> 8);
        return g_pixels.Color(rd, gd, bd);
    }

    void initLEDControl()
    {
        for (int i = 0; i < NUMPIXELS; i++)
        {
            g_ledInfo[i] = {};
            g_ledInfo[i].breathCycleMs = 3000;
            g_ledInfo[i].flashOnDurationMs = 500;
            g_ledInfo[i].flashOffDurationMs = 500;
        }
    }

    void setLEDStateInternal(int ledIndex, LEDState state, uint32_t color, int onDurationMs, int offDurationMs)
    {
        if (ledIndex < 0 || ledIndex >= NUMPIXELS)
        {
            return;
        }

        if (g_currentStates[ledIndex] == state &&
            g_currentColors[ledIndex] == color &&
            g_currentOnMs[ledIndex] == onDurationMs &&
            g_currentOffMs[ledIndex] == offDurationMs)
        {
            return;
        }

        g_currentStates[ledIndex] = state;
        g_currentColors[ledIndex] = color;
        g_currentOnMs[ledIndex] = onDurationMs;
        g_currentOffMs[ledIndex] = offDurationMs;

        auto& info = g_ledInfo[ledIndex];
        info.state = state;
        info.color = color;

        switch (state)
        {
        case LED_OFF:
            break;
        case LED_SOLID:
            break;
        case LED_BREATHING:
            info.breathCycleMs = (onDurationMs > 0) ? onDurationMs : 3000;
            break;
        case LED_FLASHING:
            info.flashOnDurationMs = (onDurationMs > 0) ? onDurationMs : 500;
            info.flashOffDurationMs = (offDurationMs > 0) ? offDurationMs : 500;
            info.lastUpdate = millis();
            break;
        default:
            break;
        }
    }

    void updateLEDs()
    {
        const unsigned long now = millis();

        for (int i = 0; i < NUMPIXELS; i++)
        {
            const auto& info = g_ledInfo[i];
            switch (info.state)
            {
            case LED_OFF:
                g_pixels.setPixelColor(i, 0);
                break;
            case LED_SOLID:
                g_pixels.setPixelColor(i, info.color);
                break;
            case LED_BREATHING:
            {
                const int cycle = (info.breathCycleMs > 0) ? info.breathCycleMs : 3000;
                const unsigned long progress = now % static_cast<unsigned long>(cycle);
                const float ratio = static_cast<float>(progress) / static_cast<float>(cycle);
                const float sinValue = sinf(ratio * 2.0f * PI);
                const uint8_t brightness = static_cast<uint8_t>(((sinValue + 1.0f) / 2.0f) * 255.0f);
                g_pixels.setPixelColor(i, dimColor(info.color, brightness));
                break;
            }
            case LED_FLASHING:
            {
                const int onMs = (info.flashOnDurationMs > 0) ? info.flashOnDurationMs : 500;
                const int offMs = (info.flashOffDurationMs > 0) ? info.flashOffDurationMs : 500;
                const int cycleMs = onMs + offMs;
                const unsigned long elapsed = now - info.lastUpdate;
                const bool isOn = (elapsed % static_cast<unsigned long>(cycleMs)) < static_cast<unsigned long>(onMs);
                g_pixels.setPixelColor(i, isOn ? info.color : 0);
                break;
            }
            default:
                g_pixels.setPixelColor(i, 0);
                break;
            }
        }

        g_pixels.show();
    }

    void startupSweep()
    {
        g_pixels.fill(g_pixels.Color(255, 0, 0));
        g_pixels.show();
        vTaskDelay(pdMS_TO_TICKS(500));

        g_pixels.fill(g_pixels.Color(0, 255, 0));
        g_pixels.show();
        vTaskDelay(pdMS_TO_TICKS(500));

        g_pixels.fill(g_pixels.Color(0, 0, 255));
        g_pixels.show();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    void applyStatusToLEDs()
    {
        // Legacy-compatible LED mapping (ported from LEGACY-ECHO-2024 update_pixel()).
        // We derive the legacy states from current firmware signals + new settings.

        const bool storageOk = ensureStorage();
        const bool usingSd = isStorageModeSdCard();
        const bool sdWanted = appSettings.sdCard.useSdCard;

        const bool hasCreds = network_hasAnyWiFiCredentials();
        const bool wifiConnected = WiFi.isConnected();
        const bool isAPMode = boondock_server_isAPModeActive();

        // Legacy flags/settings
        const bool offline = appSettings.offlineMode;
        const bool solidStyle = (appSettings.ledStyle == static_cast<uint8_t>(EchoLedStyle::Solid));
        const uint8_t startupMode = appSettings.startupMode;

        // 1) DND mode in legacy forced all OFF (not currently implemented) -> skip

        // 2) SD init error (legacy APP_STATE_INIT_SD_ERROR)
        if (!storageOk || (sdWanted && !usingSd))
        {
            setLEDStateInternal(LED_APP, LED_FLASHING, RED, 2000, 1000);
            setLEDStateInternal(LED_NETWORK, LED_FLASHING, RED, 2000, 1000);
            setLEDStateInternal(LED_RADIO, LED_FLASHING, RED, 2000, 1000);
            return;
        }

        // 3) Setup mode (legacy BOOONDOCK_MODE_SETUP) => all breathing WHITE
        if (!g_deviceReady || isAPMode || startupMode == static_cast<uint8_t>(EchoStartupMode::Setup))
        {
            setLEDStateInternal(LED_APP, LED_BREATHING, WHITE, 2000, 0);
            setLEDStateInternal(LED_NETWORK, LED_BREATHING, WHITE, 2000, 0);
            setLEDStateInternal(LED_RADIO, LED_BREATHING, WHITE, 2000, 0);
            return;
        }

        // 4) Updating state (legacy NETWORK_STATE_UPDATING) - map to "uploading" here
        if (system_isUploading())
        {
            setLEDStateInternal(LED_APP, LED_BREATHING, GREEN, 2000, 0);
            setLEDStateInternal(LED_NETWORK, LED_BREATHING, GREEN, 2000, 0);
            setLEDStateInternal(LED_RADIO, LED_BREATHING, GREEN, 2000, 0);
            return;
        }

        // APP LED (legacy applicationState switch)
        // We derive a minimal applicationState:
        // - INIT when not ready handled above
        // - INIT_WIFI_ERROR when creds exist but wifi not connected and not offline
        // - default => based on startupMode + ledStyle
        if (!wifiConnected && hasCreds && !offline)
        {
            // APP_STATE_INIT_WIFI_ERROR
            setLEDStateInternal(LED_APP, LED_FLASHING, RED, 3000, 1000);
        }
        else
        {
            // default cases based on startup mode
            const uint8_t mode = startupMode;
            const bool wantSolid = solidStyle;

            if (mode == static_cast<uint8_t>(EchoStartupMode::Offline))
            {
                setLEDStateInternal(LED_APP, wantSolid ? LED_SOLID : LED_FLASHING, RED, 100, 3000);
            }
            else if (mode == static_cast<uint8_t>(EchoStartupMode::Simplex))
            {
                setLEDStateInternal(LED_APP, wantSolid ? LED_SOLID : LED_FLASHING, BLUE, 100, 3000);
            }
            else if (mode == static_cast<uint8_t>(EchoStartupMode::Duplex))
            {
                setLEDStateInternal(LED_APP, wantSolid ? LED_SOLID : LED_FLASHING, PURPLE, 100, 3000);
            }
            else if (mode == static_cast<uint8_t>(EchoStartupMode::OnlineLimited) || (hasCreds && !wifiConnected))
            {
                setLEDStateInternal(LED_APP, wantSolid ? LED_SOLID : LED_FLASHING, GRAY, 100, 3000);
            }
            else
            {
                setLEDStateInternal(LED_APP, wantSolid ? LED_SOLID : LED_FLASHING, GREEN, 100, 3000);
            }
        }

        // RADIO LED (legacy radioState switch) - map using current signals
        if (!hasCreds && isAPMode)
        {
            // Treat AP-only mode as radio off
            setLEDStateInternal(LED_RADIO, LED_OFF, 0, 0, 0);
        }
        else if (recorder_isRecording())
        {
            // RADIO_STATE_MIC_RECORDING
            setLEDStateInternal(LED_RADIO, LED_SOLID, RED, 0, 0);
        }
        else
        {
            // RADIO_STATE_IDLE (legacy also used record_line_in and voicemail)
            setLEDStateInternal(LED_RADIO, LED_OFF, 0, 0, 0);
        }

        // NETWORK LED (legacy network state mapping)
        if (wifiConnected)
        {
            if (system_getUploadQueueSize() > 0)
            {
                // Legacy had SENDING=GREEN; we treat any queued work as SENDING.
                setLEDStateInternal(LED_NETWORK, LED_SOLID, GREEN, 1000, 2000);
            }
            else if (solidStyle)
            {
                setLEDStateInternal(LED_NETWORK, LED_SOLID, BLUE, 100, 3000);
            }
            else
            {
                setLEDStateInternal(LED_NETWORK, LED_FLASHING, BLUE, 100, 3000);
            }
        }
        else
        {
            if (offline)
            {
                setLEDStateInternal(LED_NETWORK, LED_OFF, 0, 0, 0);
            }
            else
            {
                setLEDStateInternal(LED_NETWORK, LED_FLASHING, RED, 3000, 1000);
            }
        }
    }

} // namespace

void echoLed_notifyDeviceReady()
{
    g_deviceReady = true;
}

void echoLedTask(void* pvParameters)
{
    (void)pvParameters;

    logInfof("[EchoLED] Starting NeoPixel task");

    g_pixels.begin();
    g_pixels.setBrightness(DEFAULT_LED_BRIGHTNESS);

    initLEDControl();
    startupSweep();

    // Default initial: breathing white
    setLEDStateInternal(LED_APP, LED_BREATHING, WHITE, 1000, 0);
    setLEDStateInternal(LED_RADIO, LED_BREATHING, WHITE, 1000, 0);
    setLEDStateInternal(LED_NETWORK, LED_BREATHING, WHITE, 1000, 0);
    g_pixels.show();

    while (true)
    {
        applyStatusToLEDs();
        updateLEDs();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#endif // defined(ECHO)

