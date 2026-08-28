#pragma once

#if defined(ECHO)

#include <Arduino.h>

void echoKeypadTask(void* pvParameters);

// Minimal hooks for other tasks (optional usage)
bool echoKeypad_consumeVolPressed();
bool echoKeypad_consumeNextPressed();
bool echoKeypad_consumePrevPressed();
bool echoKeypad_consumePttPressed();

#endif // defined(ECHO)

