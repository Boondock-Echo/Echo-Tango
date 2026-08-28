#include "websocket_handler.h"
#include "boondock_server.h"
#include "logger.h"
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <cstring>

static String base64Encode(const uint8_t* data, size_t length)
{
    const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String result;
    size_t i = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    
    while (length--)
    {
        char_array_3[i++] = *(data++);
        if (i == 3)
        {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (i = 0; i < 4; i++)
                result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    
    if (i)
    {
        for (size_t j = i; j < 3; j++)
            char_array_3[j] = '\0';
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        
        for (size_t j = 0; j < i + 1; j++)
            result += base64_chars[char_array_4[j]];
        
        while (i++ < 3)
            result += '=';
    }
    
    return result;
}

static String websocketComputeAcceptKey(const String& key)
{
    String magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    String combined = key + magic;
    
    // Compute SHA1 hash
    unsigned char hash[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const unsigned char*)combined.c_str(), combined.length());
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
    
    // Base64 encode
    size_t olen = 0;
    unsigned char encoded[32];
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &olen, hash, 20) != 0)
    {
        return String(); // Error
    }
    
    return String((char*)encoded, olen);
}

bool WebSocketHandler::handleUpgrade(WiFiClient& client, WebServer* server)
{
    if (!server || !client.connected())
    {
        Serial.printf("[WebSocket] handleUpgrade: server=%p, client.connected()=%d\n", server, client.connected() ? 1 : 0);
        return false;
    }
    
    // Get WebSocket headers
    String upgradeHeader = server->header("Upgrade");
    String connectionHeader = server->header("Connection");
    String wsKey = server->header("Sec-WebSocket-Key");
    
    Serial.printf("[WebSocket] Headers: Upgrade='%s', Connection='%s', Key='%s'\n", 
                  upgradeHeader.c_str(), connectionHeader.c_str(), wsKey.length() > 0 ? "present" : "missing");
    
    upgradeHeader.toLowerCase();
    connectionHeader.toLowerCase();
    
    if (upgradeHeader != "websocket" || connectionHeader.indexOf("upgrade") < 0 || wsKey.length() == 0)
    {
        Serial.printf("[WebSocket] Header validation failed\n");
        return false;
    }
    
    // Compute accept key
    String accept = websocketComputeAcceptKey(wsKey);
    if (accept.length() == 0)
    {
        Serial.printf("[WebSocket] Failed to compute accept key\n");
        return false;
    }
    
    // Send upgrade response
    String response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n";
    response += "\r\n";
    
    size_t written = client.print(response);
    client.flush();
    
    Serial.printf("[WebSocket] Upgrade response sent (%d bytes), client.connected()=%d\n", 
                  written, client.connected() ? 1 : 0);
    
    return true;
}

bool WebSocketHandler::sendTextFrame(WiFiClient& client, const String& message)
{
    if (!client.connected())
        return false;
    
    size_t payloadLen = message.length();
    uint8_t frame[4];
    size_t frameLen = 0;
    
    // FIN=1, Opcode=1 (text), MASK=0
    frame[0] = 0x81;
    frameLen = 1;
    
    if (payloadLen < 126)
    {
        frame[1] = payloadLen;
        frameLen = 2;
    }
    else if (payloadLen < 65536)
    {
        frame[1] = 126;
        frame[2] = (payloadLen >> 8) & 0xFF;
        frame[3] = payloadLen & 0xFF;
        frameLen = 4;
    }
    else
    {
        return false; // Too large
    }
    
    client.write(frame, frameLen);
    client.print(message);
    
    return true;
}

bool WebSocketHandler::sendBinaryFrame(WiFiClient& client, const uint8_t* data, size_t length)
{
    if (!client.connected() || data == nullptr || length == 0)
        return false;
    
    uint8_t frame[4];
    size_t frameLen = 0;
    
    // FIN=1, Opcode=2 (binary), MASK=0
    frame[0] = 0x82;
    frameLen = 1;
    
    if (length < 126)
    {
        frame[1] = length;
        frameLen = 2;
    }
    else if (length < 65536)
    {
        frame[1] = 126;
        frame[2] = (length >> 8) & 0xFF;
        frame[3] = length & 0xFF;
        frameLen = 4;
    }
    else
    {
        return false; // Too large
    }
    
    client.write(frame, frameLen);
    client.write(data, length);
    
    return true;
}

bool WebSocketHandler::readFrame(WiFiClient& client, String& outMessage, uint8_t* outBinary, size_t maxBinaryLen, size_t& outBinaryLen)
{
    if (!client.connected() || client.available() < 2)
        return false;
    
    uint8_t byte1 = client.read();
    uint8_t byte2 = client.read();
    
    bool fin = (byte1 & 0x80) != 0;
    uint8_t opcode = byte1 & 0x0F;
    bool masked = (byte2 & 0x80) != 0;
    uint64_t payloadLen = byte2 & 0x7F;
    
    if (payloadLen == 126)
    {
        if (client.available() < 2)
            return false;
        payloadLen = (client.read() << 8) | client.read();
    }
    else if (payloadLen == 127)
    {
        if (client.available() < 8)
            return false;
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
        {
            payloadLen = (payloadLen << 8) | client.read();
        }
    }
    
    uint8_t maskingKey[4] = {0};
    if (masked)
    {
        if (client.available() < 4)
            return false;
        for (int i = 0; i < 4; i++)
        {
            maskingKey[i] = client.read();
        }
    }
    
    if (client.available() < payloadLen)
        return false;
    
    if (opcode == 0x1) // Text frame
    {
        outMessage = "";
        for (uint64_t i = 0; i < payloadLen; i++)
        {
            uint8_t byte = client.read();
            if (masked)
                byte ^= maskingKey[i % 4];
            outMessage += (char)byte;
        }
        outBinaryLen = 0;
        return true;
    }
    else if (opcode == 0x2) // Binary frame
    {
        if (outBinary == nullptr || maxBinaryLen < payloadLen)
            return false;
        
        outBinaryLen = 0;
        for (uint64_t i = 0; i < payloadLen; i++)
        {
            uint8_t byte = client.read();
            if (masked)
                byte ^= maskingKey[i % 4];
            outBinary[outBinaryLen++] = byte;
        }
        outMessage = "";
        return true;
    }
    else if (opcode == 0x8) // Close frame
    {
        return false; // Connection closing
    }
    else if (opcode == 0x9) // Ping
    {
        // Respond with pong
        uint8_t pongFrame[2] = {0x8A, 0x00}; // FIN=1, Opcode=10 (pong)
        client.write(pongFrame, 2);
        return readFrame(client, outMessage, outBinary, maxBinaryLen, outBinaryLen); // Read next frame
    }
    
    return false;
}

void WebSocketHandler::sendMessage(WiFiClient& client, WebSocketMessageType type, const JsonObject& data)
{
    if (!client.connected())
        return;
    
    DynamicJsonDocument doc(2048);
    doc["type"] = static_cast<int>(type);
    doc["data"] = data;
    
    String json;
    serializeJson(doc, json);
    
    sendTextFrame(client, json);
}

