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

// WebSocket functions
void boondock_server_pushHomeData(); // Push home data to all WebSocket clients
void boondock_server_pushAudioStats(); // Push audio stats to all WebSocket clients
void boondock_server_pushNetworkConfig(); // Push network config to all WebSocket clients
void boondock_server_pushLiveAudio(); // Push live audio data to all WebSocket clients

