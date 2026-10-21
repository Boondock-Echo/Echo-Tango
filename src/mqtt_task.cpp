#if defined(ECHO)

#include "mqtt_task.h"
#include "config.h"
#include "logger.h"
#include "common.h"
#include "settings.h"
#include "recorder.h"
#include "network.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include "sd_bus.h"
#include <ctime>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

char device_id[13] = {0};

volatile bool g_commandInProgress = false;
AsyncMqttClient mqttClient;

namespace
{
    constexpr uint32_t kMqttReconnectMs = 5000;

    void mqttRefreshDeviceIdFromMac()
    {
        const String &id = getDeviceId();
        strncpy(device_id, id.c_str(), sizeof(device_id) - 1);
        device_id[sizeof(device_id) - 1] = '\0';
    }

    bool applySettingParam(const char *paramKey, const String &value, bool save = false)
    {
        String v = value;
        v.trim();
        if (v.length() == 0)
            return false;
        if (!settings_setParam(paramKey, v))
            return false;
        settings_clearDebounce();
        if (save)
            settings_save();
        logWarnf("[MQTT] applied %s=%s", paramKey, settings_getParam(paramKey).c_str());
        return true;
    }

    bool applyBoolSetting(const char *paramKey, const String &value, const char *cmdLabel)
    {
        String v = value;
        v.trim();
        v.toLowerCase();
        if (v.length() == 0)
            return false;

        const char *enabled = nullptr;
        if (v == "1")
            enabled = "true";
        else if (v == "0")
            enabled = "false";
        else
        {
            logWarnf("[MQTT] ignored /set/%s invalid value='%s'", cmdLabel, v.c_str());
            return false;
        }

        if (!settings_setParam(paramKey, enabled))
            return false;
        settings_clearDebounce();
        logWarnf("[MQTT] applied /set/%s -> %s=%s", cmdLabel, paramKey, settings_getParam(paramKey).c_str());
        return true;
    }

    // Returns true when the command slot must stay busy (play_cloud / play_transmit until RecordTask finishes).
    bool handleEvents(const String &cmd, const String &value)
    {
        if (cmd == "volume" || cmd == "spkr_vol" || cmd == "speaker_volume")
        {
            (void)applySettingParam("audio.speakervolume", value, true);
            return false;
        }

        if (cmd == "gain" || cmd == "line_in_gain")
        {
            (void)applySettingParam("audio.codecgain", value);
            return false;
        }

        if (cmd == "max" || cmd == "max_rec_sec")
        {
            (void)applySettingParam("audio.maxrecordingms", value);
            return false;
        }

        if (cmd == "silence" || cmd == "audio_stop_silence")
        {
            (void)applySettingParam("audio.silencethresholdms", value);
            return false;
        }

        if (cmd == "spkr_on")
        {
            (void)applyBoolSetting("audio.speakerenabled", value, "spkr_on");
            return false;
        }

        if (cmd == "tx_vol")
        {
            (void)applySettingParam("audio.transmitvolume", value);
            return false;
        }

        if (cmd == "line_min_db")
        {
            (void)applySettingParam("audio.audiothreshold", value);
            return false;
        }

        if (cmd == "tx_on")
        {
            (void)applyBoolSetting("audio.transmitenabled", value, "tx_on");
            return false;
        }

        if (cmd == "record_line_in")
        {
            String v = value;
            v.trim();
            v.toLowerCase();
            if (v == "0")
            {
                recorder_setLineInRecordingEnabled(false);
                logWarnf("[MQTT] record_line_in=0: recording stopped until value=1");
                return false;
            }
            if (v == "1")
            {
                recorder_setLineInRecordingEnabled(true);
                logWarnf("[MQTT] record_line_in=1: recording enabled");
                return false;
            }
            logWarnf("[MQTT] ignored record_line_in invalid value='%s'", v.c_str());
            return false;
        }

        if (cmd == "play_cloud")
        {
            if (!sd_bus::cardType())
            {
                logWarnf("[MQTT] play_cloud rejected: SD card not present");
                return false;
            }

            String fileName = value;
            fileName.trim();
            if (fileName.length() == 0)
                return false;

            logWarnf("[MQTT] play_cloud queued -> %s", fileName.c_str());
            recorder_requestPlayCloud(fileName);
            return true;
        }

        if (cmd == "play_transmit" || cmd == "play_transmit_mp3")
        {
            if (!sd_bus::cardType())
            {
                logWarnf("[MQTT] %s rejected: SD card not present", cmd.c_str());
                return false;
            }

            String fileName = value;
            fileName.trim();
            if (fileName.length() == 0)
                return false;

            logWarnf("[MQTT] %s queued -> %s", cmd.c_str(), fileName.c_str());
            recorder_requestPlayTransmit(fileName);
            return true;
        }

        if (cmd == "reboot")
        {
            logWarnf("[MQTT] reboot requested via /set/reboot");
            system_requestGracefulReboot("mqtt_reboot");
        }

        if (cmd == "factory_reset")
        {
            logWarnf("[MQTT] factory reset requested via /set/factory_reset");
            if (!settings_factoryReset())
            {
                logWarnf("[MQTT] factory_reset failed: cannot open NVS settings namespace");
                return false;
            }
            return true;
        }

        logWarnf("[MQTT] unknown command: %s", cmd.c_str());
        return false;
    }
}

// Cloud play filename: 2026-08-17T08-35-14Z.wav -> inbox basename 2026-08-17-08-35-14.wav
static bool toInboxBasename(const String &fileName, String &outBasename)
{
    if (!fileName.endsWith(".wav") || fileName.length() < 24)
        return false;

    const String stem = fileName.substring(0, fileName.length() - 4);
    if (stem.length() != 20 || stem.charAt(4) != '-' || stem.charAt(7) != '-' ||
        stem.charAt(10) != 'T' || stem.charAt(19) != 'Z')
    {
        return false;
    }

    const String timePart = stem.substring(11, 19);
    if (timePart.charAt(2) != '-' || timePart.charAt(5) != '-')
        return false;

    outBasename = stem.substring(0, 10) + "-" + timePart + ".wav";
    return true;
}

// Outbox / cloud download filename: 2026-08-17T09-57-42Z.wav
static bool toOutboxBasename(const String &fileName, String &outBasename)
{
    if (!fileName.endsWith(".wav") || fileName.length() < 24)
        return false;

    const String stem = fileName.substring(0, fileName.length() - 4);
    if (stem.length() != 20 || stem.charAt(10) != 'T' || stem.charAt(19) != 'Z')
        return false;

    outBasename = fileName;
    return true;
}

static String findInboxFilePath(const String &fileName)
{
    String inboxBasename;
    if (!toInboxBasename(fileName, inboxBasename))
        return "";

    const String inboxPath = "/inbox/" + inboxBasename.substring(0, 4) + "/" +
                             inboxBasename.substring(5, 7) + "/" +
                             inboxBasename.substring(8, 10) + "/" + inboxBasename;
    if (sd_bus::exists(inboxPath))
        return inboxPath;
    return "";
}

static String buildOutboxPath(const String &fileName)
{
    if (fileName.length() == 0)
        return "";

    String year;
    String month;
    String day;
    if (fileName.length() >= 10 && fileName.charAt(4) == '-' && fileName.charAt(7) == '-')
    {
        year = fileName.substring(0, 4);
        month = fileName.substring(5, 7);
        day = fileName.substring(8, 10);
    }
    else
    {
        time_t now = 0;
        time(&now);
        if (isEpochValid(now))
        {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            char datePath[16];
            strftime(datePath, sizeof(datePath), "%Y/%m/%d", &timeinfo);
            return String("/outbox/") + datePath + "/" + fileName;
        }
        return String("/outbox/1970/01/01/") + fileName;
    }

    return "/outbox/" + year + "/" + month + "/" + day + "/" + fileName;
}

void mqttTask(void *pvParameters);
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttSubscribe(uint16_t packetId, uint8_t qos);
void onMqttUnsubscribe(uint16_t packetId);
void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
void onMqttPublish(uint16_t packetId);
void connectToMqtt();

void mqtt_task_begin()
{
    const BaseType_t result = xTaskCreatePinnedToCore(
        mqttTask,
        "MqttTask",
        8192,
        nullptr,
        1,
        nullptr,
        0);

    if (result != pdPASS)
        logErrorf("[Startup] Failed to create MqttTask");
}

void mqtt_clearCommandInProgress()
{
    g_commandInProgress = false;
}

bool mqtt_isCommandInProgress()
{
    return g_commandInProgress;
}

void mqttTask(void *pvParameters)
{
    (void)pvParameters;

    const WiFiMode_t mode = WiFi.getMode();
    if (mode == WIFI_STA || mode == WIFI_AP_STA || mode == WIFI_AP)
        WiFi.setSleep(false);
    mqttRefreshDeviceIdFromMac();

    mqttClient.setClientId(device_id);
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onSubscribe(onMqttSubscribe);
    mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);
    mqttClient.setServer(ECHO_MQTT_BROKER_HOST, ECHO_MQTT_BROKER_PORT);
    mqttClient.setCredentials(ECHO_MQTT_USERNAME, appSettings.mqttKey);

    unsigned long lastMqttAttemptMs = 0;

    while (true)
    {
        if (WiFi.isConnected())
        {
            if (!mqttClient.connected())
            {
                const unsigned long now = millis();
                if ((unsigned long)(now - lastMqttAttemptMs) >= kMqttReconnectMs)
                {
                    lastMqttAttemptMs = now;
                    connectToMqtt();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void onMqttConnect(bool sessionPresent)
{
    (void)sessionPresent;
    Serial.println("[MQTT] CONNECTED");

    const String subMac = String("boondock/") + device_id + "/set/#";
    mqttClient.subscribe(subMac.c_str(), MQTT_EVENT_QOS);

    String lowerDeviceId = String(device_id);
    lowerDeviceId.toLowerCase();

    const String subMacLower = String("boondock/") + lowerDeviceId + "/set/#";
    mqttClient.subscribe(subMacLower.c_str(), MQTT_EVENT_QOS);

}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
    logWarnf("[MQTT] Disconnected (reason=%d)", static_cast<int>(reason));
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
    (void)packetId;
    (void)qos;
}

void onMqttUnsubscribe(uint16_t packetId)
{
    (void)packetId;
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
    (void)properties;
    (void)index;
    (void)total;

    String value;
    String cleanedTopic;

    /* Cleaning the topic and the payload */
    for (size_t i = 0; i < len; i++)
    {
        if (isPrintable(payload[i]))
            value += (char)payload[i];
    }

    const size_t topicLen = strlen(topic);
    for (size_t i = 0; i < topicLen; i++)
    {
        if (isPrintable(topic[i]))
            cleanedTopic += (char)topic[i];
    }

    const int setIdx = cleanedTopic.indexOf("/set/");
    if (setIdx < 0)
    {
        logWarnf("[MQTT] ignored topic without /set/: %s", cleanedTopic.c_str());
        return;
    }

    String cmd = cleanedTopic.substring(setIdx + 5);
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0)
    {
        logWarnf("[MQTT] ignored empty command topic: %s", cleanedTopic.c_str());
        return;
    }

    logWarnf("[MQTT] RX topic=%s cmd=%s value=%s", cleanedTopic.c_str(), cmd.c_str(), value.c_str());

    if (g_commandInProgress)
    {
        logWarnf("[MQTT] Following command is discarded: cmd=%s value=%s", cmd.c_str(), value.c_str());
        return;
    }

    g_commandInProgress = true;
    if (!handleEvents(cmd, value))
        g_commandInProgress = false;
}

void onMqttPublish(uint16_t packetId)
{
    (void)packetId;
}

void connectToMqtt()
{
    if (ECHO_MQTT_BROKER_HOST[0] == '\0' || appSettings.mqttKey[0] == '\0' || !WiFi.isConnected())
        return;

    mqttClient.setKeepAlive(MQTT_WILL_TIMEOUT);
    mqttClient.connect();
}

String resolvePlayFilePath(const String &fileName, const String &macAddress)
{
    if (fileName.length() == 0 || macAddress.length() == 0)
        return "";

    const String inboxPath = findInboxFilePath(fileName);
    if (inboxPath.length() > 0)
    {
        logWarnf("[Playback] Found in inbox: %s", inboxPath.c_str());
        return inboxPath;
    }

    String outboxBasename;
    if (!toOutboxBasename(fileName, outboxBasename))
    {
        logWarnf("[Playback] Unrecognized play filename: %s", fileName.c_str());
        return "";
    }

    logWarnf("[Playback] Inbox miss; checking outbox/download: %s", outboxBasename.c_str());
    return downloadFile(outboxBasename, macAddress);
}

String downloadFile(const String &fileName, const String &macAddress)
{
    if (fileName.length() == 0 || macAddress.length() == 0)
        return "";

    const String outPath = buildOutboxPath(fileName);
    if (outPath.length() == 0)
        return "";

    if (sd_bus::exists(outPath))
        return outPath;

    const int lastSlash = outPath.lastIndexOf('/');
    if (lastSlash <= 0)
        return "";
    if (!storage_ensureDirectoryPath(outPath.substring(0, lastSlash).c_str()))
        return "";

    constexpr size_t kBufSize = 8192;
    static uint8_t buffer[kBufSize];

    WiFi.setSleep(false);

    HTTPClient http;
    const String url = String(FULL_PATH) + "?mac_address=" + macAddress + "&filename=" + fileName + "&output=wav";
    http.setTimeout(15000);
    http.setReuse(false);
    esp_task_wdt_reset();
    if (!http.begin(url))
        return "";

    esp_task_wdt_reset();
    const int code = http.GET();
    esp_task_wdt_reset();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        return "";
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream)
    {
        http.end();
        return "";
    }

    sd_bus::SdFile file = sd_bus::open(outPath, FILE_WRITE);
    if (!file)
    {
        http.end();
        return "";
    }

    int remaining = http.getSize();
    size_t bytesWritten = 0;
    unsigned long lastWdtMs = millis();
    bool writeOk = true;

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        const unsigned long now = millis();
        if ((now - lastWdtMs) >= 250)
        {
            esp_task_wdt_reset();
            lastWdtMs = now;
        }

        const size_t avail = stream->available();
        if (avail == 0)
        {
            if (!stream->connected())
                break;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t toRead = avail;
        if (toRead > kBufSize)
            toRead = kBufSize;
        if (remaining > 0 && toRead > static_cast<size_t>(remaining))
            toRead = static_cast<size_t>(remaining);

        const int c = stream->read(buffer, toRead);
        if (c <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (file.write(buffer, static_cast<size_t>(c)) != static_cast<size_t>(c))
        {
            writeOk = false;
            break;
        }

        bytesWritten += static_cast<size_t>(c);
        if (remaining > 0)
            remaining -= c;
    }

    esp_task_wdt_reset();
    file.flush();
    file.close();
    http.end();

    if (!writeOk || bytesWritten == 0 || remaining > 0)
    {
        sd_bus::remove(outPath);
        return "";
    }

    sd_bus::SdFile verify = sd_bus::open(outPath, FILE_READ);
    if (!verify || verify.size() == 0)
    {
        if (verify)
            verify.close();
        sd_bus::remove(outPath);
        return "";
    }
    verify.close();

    network_recordCloudPathConnectResult(true);
    return outPath;
}

#endif
