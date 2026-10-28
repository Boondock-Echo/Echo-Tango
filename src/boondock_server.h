#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

// Forward declarations
void boondock_server_startAPMode();
void boondock_server_startMainMode(); // Start main web server when WiFi is connected
void boondock_server_loop(); // Call this from network_loop() to handle web server requests
bool boondock_server_isAPModeActive();
bool boondock_server_hasClient(); // Check if any client is currently connected
unsigned long boondock_server_getLastActivityTime(); // Get last client activity timestamp

// WebSocket is reserved for audio graph and live audio streaming.
void boondock_server_pushAudioStats();
void boondock_server_pushLiveAudio();
