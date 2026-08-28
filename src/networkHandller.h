#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern TaskHandle_t networkTaskHandle;
void networkTask(void *pvParameters);
bool networkHandler_isUploading();

void networkHandler_init();
void networkHandler_requestShutdown();
void networkHandler_setUploadPaused(bool paused);
bool networkHandler_isUploadPaused();