#pragma once

#if defined(ECHO)

#include <Arduino.h>

// Legacy LED compatibility settings
// ledStyle mirrors LEGACY-ECHO-2024 boondock_config.led_style behavior:
// 0 = flashing, 1 = solid (others reserved)
enum class EchoLedStyle : uint8_t
{
    Flashing = 0,
    Solid = 1,
};

// Subset of BOOONDOCK_MODE_* used by legacy LED mapping.
enum class EchoStartupMode : uint8_t
{
    Setup = 0,
    Online = 1,
    OnlineLimited = 2,
    Offline = 7,
    Simplex = 9,
    Duplex = 10,
};

#endif // defined(ECHO)

