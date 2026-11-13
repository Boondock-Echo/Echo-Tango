#pragma once

// MCP7941x real-time clock. Compiled into the TANGO build only.
// ECHO has no external RTC: GPIO 21 is the speaker amplifier and GPIO 22 is the NeoPixel.

#include <Arduino.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if defined(TANGO)

// Battery-backed clock on the TANGO board.
// SDA = GPIO 21, SCL = GPIO 22, 7-bit address 0x6F.
// The bus is Arduino Wire (I2C controller 0). The ES8388 stays on Wire1
// (I2C controller 1, GPIO 33 / GPIO 32). Time in the chip is UTC.
class Mcp7941x
{
public:
    static constexpr int kSdaPin = 21;
    static constexpr int kSclPin = 22;
    static constexpr uint8_t kAddress = 0x6F;
    static constexpr uint32_t kClockHz = 100000;

    Mcp7941x();

    bool begin();
    void end();
    bool isPresent() const;

    // False when the oscillator is stopped, the registers are not valid BCD,
    // or the chip does not acknowledge.
    bool readUtc(time_t &epochOut);

    // Starts the oscillator and enables the backup battery.
    // Year must fall in 2000-2099.
    bool writeUtc(time_t epoch);

private:
    bool lockBus();
    void unlockBus();
    bool beginUnlocked();
    bool readRegs(uint8_t reg, uint8_t *buf, size_t len);
    bool writeRegs(uint8_t reg, const uint8_t *buf, size_t len);
    bool readUtcUnlocked(time_t &epochOut);
    bool writeUtcUnlocked(time_t epoch);

    SemaphoreHandle_t lock_ = nullptr;
    bool started_ = false;
    bool present_ = false;
};

Mcp7941x &mcp7941x();

#endif // defined(TANGO)
