#include "wifi_enterprise.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wpa2.h>
#include <mbedtls/x509_crt.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>

#include "nvs.h"
#include "timekeeper.h"

namespace
{
    // The serial factory reset clears the settings namespace.
    constexpr char kNamespace[] = "settings";
    constexpr char kKey[] = "wifiEnterprise";
    constexpr size_t kMaxCaPemLength = 4096;
    constexpr size_t kMaxProfileBytes = 6144;

    enum class ProfileState { Unknown, Absent, Present };
    std::atomic<ProfileState> profileState{ProfileState::Unknown};

    // ESP-IDF retains a pointer to the PEM for as long as Enterprise is active.
    String activeCaPem;
    bool enterpriseActive = false;

    void writeProfile(JsonObject json, const EnterpriseWiFiProfile &profile)
    {
        json["ssid"] = profile.ssid;
        json["identity"] = profile.identity;
        json["username"] = profile.username;
        json["password"] = profile.password;
        json["caPem"] = profile.caPem;
    }

    bool parseText(JsonObjectConst json, const char *key, String &out,
                   size_t maxLength, bool required = true)
    {
        JsonVariantConst field = json[key];
        if (!field.isNull() && !field.is<const char *>())
            return false;

        JsonString value = field.as<JsonString>();
        const size_t length = value.size();
        if ((required && length == 0) || length > maxLength ||
            (length && strlen(value.c_str()) != length))
            return false;

        out = length ? value.c_str() : "";
        return out.length() == length;
    }

    const unsigned char *bytes(const String &value)
    {
        return reinterpret_cast<const unsigned char *>(value.c_str());
    }
}

const char *enterpriseWiFiParse(JsonObjectConst json, EnterpriseWiFiProfile &profile)
{
    if (!parseText(json, "ssid", profile.ssid, 32))
        return "Enter an SSID of 1 to 32 bytes.";
    if (!parseText(json, "username", profile.username, 128))
        return "Enter an Enterprise username of 1 to 128 bytes.";
    if (!parseText(json, "identity", profile.identity, 128, false))
        return "Enter an outer identity of up to 128 bytes.";
    if (!parseText(json, "password", profile.password, 128))
        return "Enter an Enterprise password of 1 to 128 bytes.";
    if (!parseText(json, "caPem", profile.caPem, kMaxCaPemLength))
        return "Enter a trusted CA certificate in PEM format (up to 4096 bytes).";
    mbedtls_x509_crt certificates;
    mbedtls_x509_crt_init(&certificates);
    const int result = mbedtls_x509_crt_parse(&certificates, bytes(profile.caPem),
                                            profile.caPem.length() + 1);
    mbedtls_x509_crt_free(&certificates);
    return result == 0 ? nullptr : "The trusted CA certificate could not be parsed.";
}

bool enterpriseWiFiConfigured()
{
    ProfileState state = profileState.load();
    if (state != ProfileState::Unknown)
        return state == ProfileState::Present;

    Preferences prefs;
    if (!nvs_openNamespace(kNamespace, true, prefs))
        return false;
    const ProfileState stored = prefs.isKey(kKey) ? ProfileState::Present : ProfileState::Absent;
    nvs_closeNamespace(prefs);
    return profileState.compare_exchange_strong(state, stored) ?
           stored == ProfileState::Present : state == ProfileState::Present;
}

bool enterpriseWiFiLoad(EnterpriseWiFiProfile &profile)
{
    if (!enterpriseWiFiConfigured())
        return false;

    Preferences prefs;
    if (!nvs_openNamespace(kNamespace, true, prefs))
        return false;

    const size_t length = prefs.isKey(kKey) ? prefs.getBytesLength(kKey) : 0;
    if (length == 0 || length > kMaxProfileBytes)
    {
        nvs_closeNamespace(prefs);
        return false;
    }

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[length]);
    size_t bytesRead = length;
    const bool loaded = data &&
        nvs_readBytes(prefs, kKey, data.get(), bytesRead) == NvsResult::SUCCESS &&
        bytesRead == length;
    nvs_closeNamespace(prefs);
    if (!loaded)
        return false;

    DynamicJsonDocument json(kEnterpriseWiFiJsonCapacity);
    return !deserializeJson(json, reinterpret_cast<const char *>(data.get()), length) &&
           enterpriseWiFiParse(json.as<JsonObjectConst>(), profile) == nullptr;
}

bool enterpriseWiFiSave(const EnterpriseWiFiProfile &profile)
{
    DynamicJsonDocument json(kEnterpriseWiFiJsonCapacity);
    writeProfile(json.to<JsonObject>(), profile);
    String data;
    if (json.overflowed() || serializeJson(json, data) != measureJson(json) ||
        data.length() > kMaxProfileBytes)
        return false;

    Preferences prefs;
    if (!nvs_openNamespace(kNamespace, false, prefs))
        return false;
    const bool saved = nvs_writeBytes(prefs, kKey, bytes(data), data.length()) ==
                       NvsResult::SUCCESS;
    nvs_closeNamespace(prefs);
    if (saved)
        profileState.store(ProfileState::Present);
    return saved;
}

bool enterpriseWiFiClear()
{
    Preferences prefs;
    if (!nvs_openNamespace(kNamespace, false, prefs))
        return false;
    const bool cleared = !prefs.isKey(kKey) ||
                         nvs_removeKey(prefs, kKey) == NvsResult::SUCCESS;
    nvs_closeNamespace(prefs);
    if (cleared)
        profileState.store(ProfileState::Absent);
    return cleared;
}

bool enterpriseWiFiStop()
{
    if (!enterpriseActive)
        return true;

    // Disable the supplicant before freeing the CA buffer or changing station profiles.
    if (esp_wifi_sta_wpa2_ent_disable() != ESP_OK)
        return false;

    esp_wifi_sta_wpa2_ent_clear_ca_cert();
    esp_wifi_sta_wpa2_ent_clear_identity();
    esp_wifi_sta_wpa2_ent_clear_username();
    esp_wifi_sta_wpa2_ent_clear_password();
    activeCaPem = "";
    enterpriseActive = false;
    return true;
}

const char *enterpriseWiFiStart(const EnterpriseWiFiProfile &profile)
{
#if defined(CONFIG_MBEDTLS_HAVE_TIME_DATE)
    constexpr bool checkCertificateDates = true;
    if (!timeKeeper().timeIsValid())
        return "Set the device clock before joining a network that checks certificate dates.";
#else
    constexpr bool checkCertificateDates = false;
#endif

    if (!enterpriseWiFiStop())
        return "Could not reset Enterprise WiFi authentication.";

    if (!WiFi.enableSTA(true))
        return "Could not enable the Enterprise WiFi station.";

    // The default station interface uses DHCP. Leave the PSK field empty and
    // filter out weaker networks advertising the same SSID.
    wifi_config_t station = {};
    memcpy(station.sta.ssid, profile.ssid.c_str(), profile.ssid.length());
    station.sta.scan_method = WIFI_FAST_SCAN;
    station.sta.threshold.rssi = -127;
    station.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    station.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK)
        return "Could not set the Enterprise WiFi station configuration.";

    activeCaPem = profile.caPem;
    if (activeCaPem.length() != profile.caPem.length())
        return "Could not allocate memory for the trusted CA certificate.";

    // The RADIUS server selects PEAP. This SDK does not expose an EAP allowlist.
    const String &identity = profile.identity.length() ? profile.identity : profile.username;
    enterpriseActive = true;
    if (esp_wifi_sta_wpa2_ent_set_ca_cert(bytes(activeCaPem), activeCaPem.length() + 1) != ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_identity(bytes(identity), identity.length()) != ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_username(bytes(profile.username), profile.username.length()) != ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_password(bytes(profile.password), profile.password.length()) != ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_disable_time_check(!checkCertificateDates) != ESP_OK ||
        esp_wifi_sta_wpa2_ent_enable() != ESP_OK ||
        esp_wifi_connect() != ESP_OK)
    {
        enterpriseWiFiStop();
        return "Could not start Enterprise WiFi authentication.";
    }

    return nullptr;
}
