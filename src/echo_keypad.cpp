#if defined(ECHO)

#include "echo_keypad.h"

#include "echo_hw.h"
#include "logger.h"
#include "common.h"
#include "recorder.h"
#include "settings.h"
#include "main.h"

#include <Arduino.h>
#include "PinButton.h"

namespace
{
    PinButton g_buttonNext(static_cast<int>(GPIO_KEY_NEXT));
    PinButton g_buttonPrev(static_cast<int>(GPIO_KEY_PREV));
    PinButton g_buttonPtt(static_cast<int>(GPIO_KEY_PTT));
    PinButton g_buttonVol(static_cast<int>(GPIO_KEY_VOL));

    volatile bool g_volPressed = false;
    volatile bool g_nextPressed = false;
    volatile bool g_prevPressed = false;
    volatile bool g_pttPressed = false;

    bool consume(volatile bool& flag)
    {
        if (flag)
        {
            flag = false;
            return true;
        }
        return false;
    }

    void configurePins()
    {
        pinMode(static_cast<uint8_t>(GPIO_KEY_PREV), INPUT_PULLUP);
        pinMode(static_cast<uint8_t>(GPIO_KEY_NEXT), INPUT_PULLUP);
        pinMode(static_cast<uint8_t>(GPIO_KEY_VOL), INPUT_PULLUP);
        pinMode(static_cast<uint8_t>(GPIO_KEY_PTT), INPUT_PULLUP);

        pinMode(static_cast<uint8_t>(GPIO_PTT_OUT), OUTPUT);
        digitalWrite(static_cast<uint8_t>(GPIO_PTT_OUT), LOW);
    }

    void printButtonEvent(const char* button, const char* event)
    {
        (void)button;
        (void)event;
    }

    void applyAndPersistSpeakerSettings(const char* reason)
    {
        (void)reason;
        recorder_applySpeakerSettings();
        system_notifySettingsChanged();
        // Best-effort async persist; settings_save() will debounce internally.
        (void)settings_save();
        (void)settings_commitAsync();
    }

    void updateButtons()
    {
        g_buttonNext.update();
        g_buttonPrev.update();
        g_buttonPtt.update();
        g_buttonVol.update();

        if (g_buttonVol.isSingleClick())
        {
            g_volPressed = true;
            // Legacy: short "good" beep on volume change
            recorder_beep(250, 250, 1, true);
            uint8_t v = appSettings.speakerVolume;
            if (v > 100) v = 100;
            v = (v >= 100) ? 0 : static_cast<uint8_t>(v + 10);
            appSettings.speakerVolume = v;
            applyAndPersistSpeakerSettings("vol_single");
        }
        if (g_buttonVol.isDoubleClick())
        {
            printButtonEvent("VOL", "double");
        }
        if (g_buttonVol.isLongClick())
        {
            appSettings.speakerEnabled = !appSettings.speakerEnabled;
            // Legacy: long press toggles, beep indicates new state
            recorder_beep(1000, 1000, 1, appSettings.speakerEnabled);
            applyAndPersistSpeakerSettings("vol_long");
        }

        if (g_buttonNext.isSingleClick())
        {
            g_nextPressed = true;
            printButtonEvent("NEXT", "single");
        }
        if (g_buttonNext.isDoubleClick())
        {
            printButtonEvent("NEXT", "double");
        }
        if (g_buttonNext.isLongClick())
        {
            printButtonEvent("NEXT", "long");
        }

        if (g_buttonPrev.isSingleClick())
        {
            g_prevPressed = true;
            printButtonEvent("PREV", "single");
        }
        if (g_buttonPrev.isDoubleClick())
        {
            printButtonEvent("PREV", "double");
        }
        if (g_buttonPrev.isLongClick())
        {
            printButtonEvent("PREV", "long");
        }

        if (g_buttonPtt.isClick())
        {
            g_pttPressed = true;
            // Legacy: PTT press beep "good"
            recorder_beep(250, 250, 1, true);
            printButtonEvent("PTT", "click");
        }
        if (g_buttonPtt.isReleased())
        {
            // Legacy: PTT release beep "bad"
            recorder_beep(250, 250, 1, false);
            printButtonEvent("PTT", "released");
        }
    }
} // namespace

bool echoKeypad_consumeVolPressed() { return consume(g_volPressed); }
bool echoKeypad_consumeNextPressed() { return consume(g_nextPressed); }
bool echoKeypad_consumePrevPressed() { return consume(g_prevPressed); }
bool echoKeypad_consumePttPressed() { return consume(g_pttPressed); }

void echoKeypadTask(void* pvParameters)
{
    (void)pvParameters;

    configurePins();

    // Warm-up delay (let boot settle)
    vTaskDelay(pdMS_TO_TICKS(50));

    while (true)
    {
        updateButtons();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#endif // defined(ECHO)

