#include "mcp7941x.h"

#if defined(TANGO)

#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
    // Pack a decimal 0-99 value into the BCD byte the chip registers store.
    uint8_t binToBcd(uint8_t value)
    {
        return static_cast<uint8_t>(((value / 10U) << 4) | (value % 10U));
    }

    // Unpack a BCD register byte into decimal. False if either nibble is not 0-9.
    bool bcdToBin(uint8_t value, uint8_t &out)
    {
        const uint8_t hi = static_cast<uint8_t>(value >> 4);
        const uint8_t lo = static_cast<uint8_t>(value & 0x0F);
        if (hi > 9 || lo > 9)
        {
            return false;
        }
        out = static_cast<uint8_t>(hi * 10U + lo);
        return true;
    }

    // Howard Hinnant days_from_civil. month is 1-12. Result is a Unix UTC epoch.
    time_t utcEpochFromParts(int year, int month, int day, int hour, int minute, int second)
    {
        year -= (month <= 2);
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(year - era * 400);
        const int monthShift = month + (month > 2 ? -3 : 9);
        const unsigned doy = static_cast<unsigned>((153 * monthShift + 2) / 5 + day - 1);
        const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        const int64_t days = static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
        return static_cast<time_t>(days * 86400LL + static_cast<int64_t>(hour) * 3600LL +
                                   static_cast<int64_t>(minute) * 60LL + second);
    }
}

// The one shared clock driver for the TANGO board.
Mcp7941x &mcp7941x()
{
    static Mcp7941x instance;
    return instance;
}

// Create the mutex that keeps I2C reads and writes from overlapping.
Mcp7941x::Mcp7941x()
{
    lock_ = xSemaphoreCreateMutex();
}

// Take the I2C mutex. If the mutex was never created, allow the call through.
bool Mcp7941x::lockBus()
{
    if (lock_ == nullptr)
    {
        return true;
    }
    return xSemaphoreTake(lock_, pdMS_TO_TICKS(100)) == pdTRUE;
}

// Release the I2C mutex taken by lockBus().
void Mcp7941x::unlockBus()
{
    if (lock_ != nullptr)
    {
        xSemaphoreGive(lock_);
    }
}

// True after begin() found the chip, until end() releases it.
bool Mcp7941x::isPresent() const
{
    return present_;
}

// Read len bytes starting at register reg. Repeated start, then a stop.
bool Mcp7941x::readRegs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (buf == nullptr || len == 0)
    {
        return false;
    }

    Wire.beginTransmission(kAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
    {
        return false;
    }

    const size_t got = Wire.requestFrom(static_cast<uint16_t>(kAddress), len, static_cast<bool>(true));
    if (got != len)
    {
        return false;
    }

    for (size_t i = 0; i < len; ++i)
    {
        buf[i] = Wire.read();
    }
    return true;
}

// Write len bytes starting at register reg, then send a stop.
bool Mcp7941x::writeRegs(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (buf == nullptr || len == 0)
    {
        return false;
    }

    Wire.beginTransmission(kAddress);
    Wire.write(reg);
    Wire.write(buf, len);
    return Wire.endTransmission(true) == 0;
}

// Start Wire on GPIO 21/22, confirm the chip answers, turn on battery backup,
// and select the onboard crystal. Caller already holds the mutex.
bool Mcp7941x::beginUnlocked()
{
    if (started_ && present_)
    {
        return true;
    }

    if (started_)
    {
        Wire.end();
        started_ = false;
    }
    present_ = false;

    if (!Wire.begin(kSdaPin, kSclPin, kClockHz))
    {
        return false;
    }
    Wire.setTimeOut(50);
    started_ = true;

    uint8_t weekday = 0;
    if (!readRegs(0x03, &weekday, 1))
    {
        Wire.end();
        started_ = false;
        return false;
    }

    // VBATEN (bit 3) switches the chip to the backup battery when VCC drops.
    if ((weekday & 0x08) == 0)
    {
        weekday = static_cast<uint8_t>(weekday | 0x08);
        (void)writeRegs(0x03, &weekday, 1);
    }

    // Crystal, not an external clock. Leave the multifunction pin quiet.
    uint8_t control = 0;
    if (readRegs(0x07, &control, 1))
    {
        const uint8_t next = static_cast<uint8_t>(control & ~0x78);
        if (next != control)
        {
            (void)writeRegs(0x07, &next, 1);
        }
    }

    present_ = true;
    return true;
}

// Lock the bus and start the chip. Safe to call again if it is already running.
bool Mcp7941x::begin()
{
    if (!lockBus())
    {
        return false;
    }
    const bool ok = beginUnlocked();
    unlockBus();
    return ok;
}

// Stop Wire and mark the chip absent so later reads and writes fail closed.
void Mcp7941x::end()
{
    if (!lockBus())
    {
        return;
    }
    if (started_)
    {
        Wire.end();
        started_ = false;
    }
    present_ = false;
    unlockBus();
}

// Read the seven time registers and return a UTC Unix time.
// Fails if the oscillator is stopped or a register is not valid BCD.
// Caller already holds the mutex.
bool Mcp7941x::readUtcUnlocked(time_t &epochOut)
{
    uint8_t raw[7] = {};
    if (!readRegs(0x00, raw, sizeof(raw)))
    {
        return false;
    }

    // ST (RTCSEC bit 7) and OSCRUN (RTCWKDAY bit 5). A stopped oscillator is not a clock.
    if ((raw[0] & 0x80) == 0 || (raw[3] & 0x20) == 0)
    {
        return false;
    }

    uint8_t second = 0;
    uint8_t minute = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint8_t year = 0;
    if (!bcdToBin(static_cast<uint8_t>(raw[0] & 0x7F), second) || second > 59)
    {
        return false;
    }
    if (!bcdToBin(static_cast<uint8_t>(raw[1] & 0x7F), minute) || minute > 59)
    {
        return false;
    }
    if (!bcdToBin(static_cast<uint8_t>(raw[4] & 0x3F), day) || day < 1 || day > 31)
    {
        return false;
    }
    if (!bcdToBin(static_cast<uint8_t>(raw[5] & 0x1F), month) || month < 1 || month > 12)
    {
        return false;
    }
    if (!bcdToBin(raw[6], year))
    {
        return false;
    }

    uint8_t hour = 0;
    if ((raw[2] & 0x40) != 0)
    {
        uint8_t hour12 = 0;
        if (!bcdToBin(static_cast<uint8_t>(raw[2] & 0x1F), hour12) || hour12 < 1 || hour12 > 12)
        {
            return false;
        }
        if (hour12 == 12)
        {
            hour12 = 0;
        }
        hour = ((raw[2] & 0x20) != 0) ? static_cast<uint8_t>(hour12 + 12) : hour12;
    }
    else if (!bcdToBin(static_cast<uint8_t>(raw[2] & 0x3F), hour) || hour > 23)
    {
        return false;
    }

    epochOut = utcEpochFromParts(2000 + static_cast<int>(year),
                                 static_cast<int>(month),
                                 static_cast<int>(day),
                                 static_cast<int>(hour),
                                 static_cast<int>(minute),
                                 static_cast<int>(second));
    return true;
}

// Write a UTC Unix time in 24-hour mode and leave the oscillator and battery backup on.
// Rejects years outside 2000-2099. Caller already holds the mutex.
bool Mcp7941x::writeUtcUnlocked(time_t epoch)
{
    struct tm utc = {};
    if (gmtime_r(&epoch, &utc) == nullptr)
    {
        return false;
    }

    const int fullYear = utc.tm_year + 1900;
    if (fullYear < 2000 || fullYear > 2099)
    {
        return false;
    }

    const uint8_t years = static_cast<uint8_t>(fullYear - 2000);
    const uint8_t month = static_cast<uint8_t>(utc.tm_mon + 1);
    const uint8_t day = static_cast<uint8_t>(utc.tm_mday);
    const uint8_t hour = static_cast<uint8_t>(utc.tm_hour);
    const uint8_t minute = static_cast<uint8_t>(utc.tm_min);
    const uint8_t second = static_cast<uint8_t>(utc.tm_sec);
    const uint8_t weekday = static_cast<uint8_t>(utc.tm_wday + 1);

    // Stop the oscillator so the seven time bytes cannot roll over mid-write.
    uint8_t secReg = 0;
    if (!readRegs(0x00, &secReg, 1))
    {
        return false;
    }
    secReg = static_cast<uint8_t>(secReg & ~0x80);
    if (!writeRegs(0x00, &secReg, 1))
    {
        return false;
    }

    uint8_t block[7];
    block[0] = static_cast<uint8_t>(binToBcd(second) | 0x80); // ST = 1
    block[1] = binToBcd(minute);
    block[2] = binToBcd(hour); // 24-hour mode
    block[3] = static_cast<uint8_t>((weekday & 0x07) | 0x08); // weekday + VBATEN
    block[4] = binToBcd(day);
    uint8_t monthReg = binToBcd(month);
    if ((years % 4U) == 0)
    {
        monthReg = static_cast<uint8_t>(monthReg | 0x20); // LPYR
    }
    block[5] = monthReg;
    block[6] = binToBcd(years);
    if (writeRegs(0x00, block, sizeof(block)))
    {
        return true;
    }

    // The burst failed after ST was cleared. Start the oscillator again on the previous second.
    secReg = static_cast<uint8_t>(secReg | 0x80);
    (void)writeRegs(0x00, &secReg, 1);
    return false;
}

// Locked read of the chip time. Fails if begin() has not found the chip.
bool Mcp7941x::readUtc(time_t &epochOut)
{
    if (!present_ || !lockBus())
    {
        return false;
    }
    const bool ok = readUtcUnlocked(epochOut);
    unlockBus();
    return ok;
}

// Locked write of a UTC Unix time. Fails if begin() has not found the chip.
bool Mcp7941x::writeUtc(time_t epoch)
{
    if (!present_ || !lockBus())
    {
        return false;
    }
    const bool ok = writeUtcUnlocked(epoch);
    unlockBus();
    return ok;
}

#endif // defined(TANGO)
