#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct EnterpriseWiFiProfile
{
    String ssid;
    String identity;
    String username;
    String password;
    String caPem;
};

constexpr size_t kEnterpriseWiFiJsonCapacity = 8192;

bool enterpriseWiFiConfigured();
bool enterpriseWiFiLoad(EnterpriseWiFiProfile &profile);
bool enterpriseWiFiSave(const EnterpriseWiFiProfile &profile);
bool enterpriseWiFiClear();

// Returns a message suitable for the setup UI on validation failure.
const char *enterpriseWiFiParse(JsonObjectConst json, EnterpriseWiFiProfile &profile);
const char *enterpriseWiFiStart(const EnterpriseWiFiProfile &profile);
bool enterpriseWiFiStop();
