#pragma once

#if defined(ECHO)

#include <Arduino.h>
#include <driver/gpio.h>

// ECHO hardware pinout (ported from LEGACY-ECHO-2024)
// NeoPixel
constexpr gpio_num_t GPIO_NEOPIXEL = GPIO_NUM_22;

// Keypad (used by echo_keypad task)
constexpr gpio_num_t GPIO_KEY_PTT  = GPIO_NUM_36;
constexpr gpio_num_t GPIO_KEY_PREV = GPIO_NUM_19;
constexpr gpio_num_t GPIO_KEY_NEXT = GPIO_NUM_23;
constexpr gpio_num_t GPIO_KEY_VOL  = GPIO_NUM_5;
constexpr gpio_num_t GPIO_KEY_MODE = GPIO_NUM_0;

// External PTT output (legacy)
constexpr gpio_num_t GPIO_PTT_OUT = GPIO_NUM_18;

// NeoPixel layout: 3 pixels (ported from legacy)
constexpr uint8_t NUMPIXELS = 3;
constexpr uint8_t LED_NETWORK = 0;
constexpr uint8_t LED_RADIO = 1;
constexpr uint8_t LED_APP = 2;

// Default NeoPixel brightness (0-255)
constexpr uint8_t DEFAULT_LED_BRIGHTNESS = 150;

// LED color hex values (0xRRGGBB) (ported from legacy)
constexpr uint32_t LED_OFF_COLOR = 0x000000;
constexpr uint32_t GREEN  = 0x00FF00;
constexpr uint32_t RED    = 0xFF0000;
constexpr uint32_t BLUE   = 0x0000FF;
constexpr uint32_t WHITE  = 0xFFFFFF;
constexpr uint32_t ORANGE = 0xFFA500;
constexpr uint32_t PURPLE = 0xDDA0DD;
constexpr uint32_t YELLOW = 0xFFFF00;
constexpr uint32_t GRAY   = 0x808080;

#endif // defined(ECHO)

