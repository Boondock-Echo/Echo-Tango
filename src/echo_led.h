#pragma once

#if defined(ECHO)

#include <Arduino.h>

void echoLedTask(void* pvParameters);

// Optional helper: can be called by other code to briefly force a startup-ready transition.
void echoLed_notifyDeviceReady();

#endif // defined(ECHO)

