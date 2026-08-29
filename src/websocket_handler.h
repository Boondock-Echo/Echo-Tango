#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// WebSocket message types
enum class WebSocketMessageType {
    AUDIO_STATS,
    HOME_SUMMARY,
    AUDIO_SETTINGS,
    NETWORK_CONFIG,
    LIVE_AUDIO_DATA,
    DEVICE_INFO,
    FIRMWARE_CHECK,
    SD_CARD_SETTINGS,
    SETTINGS_UPDATE,
    ERROR_RESPONSE
};

// WebSocket frame parsing and sending
class WebSocketHandler {
public:
    static bool handleUpgrade(WiFiClient& client, WebServer* server);
    static bool sendTextFrame(WiFiClient& client, const String& message);
    static bool sendBinaryFrame(WiFiClient& client, const uint8_t* data, size_t length);
    static bool readFrame(WiFiClient& client, String& outMessage, uint8_t* outBinary, size_t maxBinaryLen, size_t& outBinaryLen);
    static void sendMessage(WiFiClient& client, WebSocketMessageType type, const JsonObject& data);
};

