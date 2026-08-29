#if defined(ECHO)

#pragma once
#include <Arduino.h>

#define CLOUD_HOST "http://api.boondock.cloud:7001"
#define SERVER_API "/api/v2/getfile"
#define OUTPUT_FMT "wav"
#define FULL_PATH "http://api.boondock.cloud:7001/api/v2/getfile"

void mqtt_task_begin();

// Cleared by RecordTask when a long-running MQTT command (e.g. play_cloud / play_transmit) finishes.
void mqtt_clearCommandInProgress();

bool mqtt_isCommandInProgress();

// Returns local SD path on success, empty string on failure. Runs on RecordTask.
String downloadFile(const String& fileName, const String& macAddress);

#endif
