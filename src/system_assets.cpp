#include "system_assets.h"

#include <HTTPClient.h>
#include <SD_MMC.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "logger.h"
#include "common.h"

namespace
{
    constexpr const char* kSystemRoot = "/system";
    constexpr const char* kSpaDir = "/system/spa";
    constexpr const char* kSoundsDir = "/system/sounds";

    bool ensureDirRecursive(const char* path)
    {
        if (path == nullptr || path[0] == '\0')
        {
            return false;
        }
        if (!isStorageModeSdCard())
        {
            return false;
        }

        String p(path);
        if (!p.startsWith("/"))
        {
            p = "/" + p;
        }
        if (SD_MMC.exists(p))
        {
            return true;
        }

        // Build up /a, /a/b, /a/b/c...
        String accum;
        int start = 1; // skip leading '/'
        while (start < static_cast<int>(p.length()))
        {
            const int slash = p.indexOf('/', start);
            String part;
            if (slash < 0)
            {
                part = p.substring(start);
                start = p.length();
            }
            else
            {
                part = p.substring(start, slash);
                start = slash + 1;
            }

            if (part.length() == 0)
            {
                continue;
            }
            accum += "/" + part;
            if (!SD_MMC.exists(accum))
            {
                if (!SD_MMC.mkdir(accum))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool downloadToFileBestEffort(const String& url, const char* outPath, const char* contentTypeHint)
    {
        if (!isStorageModeSdCard() || outPath == nullptr || outPath[0] == '\0')
        {
            return false;
        }
        if (!ensureDirRecursive(kSystemRoot) || !ensureDirRecursive(kSpaDir) || !ensureDirRecursive(kSoundsDir))
        {
            return false;
        }

        HTTPClient http;
        http.setTimeout(8000);
        http.setUserAgent(String("Boondock/") + String(FIRMWARE));

        esp_task_wdt_reset();
        if (!http.begin(url))
        {
            return false;
        }

        if (contentTypeHint != nullptr && contentTypeHint[0] != '\0')
        {
            http.addHeader("Accept", contentTypeHint);
        }

        const int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            http.end();
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        if (stream == nullptr)
        {
            http.end();
            return false;
        }

        // Write to temp then rename (atomic-ish).
        String tmp = String(outPath) + ".tmp";
        if (SD_MMC.exists(tmp))
        {
            SD_MMC.remove(tmp);
        }

        File f = SD_MMC.open(tmp.c_str(), FILE_WRITE);
        if (!f)
        {
            http.end();
            return false;
        }

        uint8_t buf[1024];
        unsigned long lastWdtMs = millis();
        size_t total = 0;

        while (http.connected())
        {
            const int avail = stream->available();
            if (avail <= 0)
            {
                // If server finished, break; otherwise yield briefly.
                if (!stream->connected())
                {
                    break;
                }
                delay(2);
                continue;
            }

            const size_t toRead = static_cast<size_t>(std::min(avail, static_cast<int>(sizeof(buf))));
            const int n = stream->readBytes(reinterpret_cast<char*>(buf), toRead);
            if (n <= 0)
            {
                break;
            }
            const size_t written = f.write(buf, static_cast<size_t>(n));
            if (written != static_cast<size_t>(n))
            {
                f.close();
                http.end();
                SD_MMC.remove(tmp);
                return false;
            }
            total += written;

            const unsigned long now = millis();
            if ((now - lastWdtMs) > 250)
            {
                esp_task_wdt_reset();
                lastWdtMs = now;
            }
        }

        f.flush();
        f.close();
        http.end();

        if (total == 0)
        {
            SD_MMC.remove(tmp);
            return false;
        }

        if (SD_MMC.exists(outPath))
        {
            SD_MMC.remove(outPath);
        }
        const bool ok = SD_MMC.rename(tmp.c_str(), outPath);
        if (!ok)
        {
            SD_MMC.remove(tmp);
        }
        return ok;
    }

    String joinUrl(const char* host, uint16_t port, const char* basePath, const char* leaf)
    {
        String url = "http://";
        url += host;
        if (port != 80)
        {
            url += ":";
            url += String(port);
        }
        if (basePath != nullptr && basePath[0] != '\0')
        {
            if (basePath[0] != '/')
            {
                url += "/";
            }
            url += basePath;
        }
        if (!url.endsWith("/"))
        {
            url += "/";
        }
        url += leaf;
        return url;
    }
}

String system_assets_localSpaIndexPath()
{
    return isStorageModeSdCard() ? String("/system/spa/index.html") : String("");
}

String system_assets_localSpaCssPath()
{
    return isStorageModeSdCard() ? String("/system/spa/app.css") : String("");
}

String system_assets_localSpaJsPath()
{
    return isStorageModeSdCard() ? String("/system/spa/app.js") : String("");
}

String system_assets_localBootWavPath()
{
    return isStorageModeSdCard() ? String("/system/sounds/boot.wav") : String("");
}

void system_assets_syncFromCdnBestEffort()
{
    if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
    {
        return;
    }
    if (!isStorageModeSdCard())
    {
        return;
    }

    // Disabled unless configured at build time.
    if (String(SYSTEM_ASSETS_CDN_HOST).length() == 0)
    {
        return;
    }

    static unsigned long s_lastSyncMs = 0;
    const unsigned long now = millis();
    if (s_lastSyncMs != 0 && (now - s_lastSyncMs) < (5UL * 60UL * 1000UL))
    {
        return; // avoid spamming on reconnect flaps
    }
    s_lastSyncMs = now;

    logInfof("[SystemAssets] Sync from CDN host=%s base=%s", SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_BASE_PATH);

    const String urlIndex = joinUrl(SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_PORT, SYSTEM_ASSETS_CDN_BASE_PATH, "index.html");
    const String urlCss   = joinUrl(SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_PORT, SYSTEM_ASSETS_CDN_BASE_PATH, "app.css");
    const String urlJs    = joinUrl(SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_PORT, SYSTEM_ASSETS_CDN_BASE_PATH, "app.js");
    const String urlBoot  = joinUrl(SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_PORT, SYSTEM_ASSETS_CDN_BASE_PATH, "boot.wav");

    const String outIndex = system_assets_localSpaIndexPath();
    const String outCss   = system_assets_localSpaCssPath();
    const String outJs    = system_assets_localSpaJsPath();
    const String outBoot  = system_assets_localBootWavPath();

    (void)downloadToFileBestEffort(urlIndex, outIndex.c_str(), "text/html");
    (void)downloadToFileBestEffort(urlCss,   outCss.c_str(),   "text/css");
    (void)downloadToFileBestEffort(urlJs,    outJs.c_str(),    "application/javascript");
    (void)downloadToFileBestEffort(urlBoot,  outBoot.c_str(),  "audio/wav");
}

bool system_assets_updateSoundsNow()
{
    if (!WiFi.isConnected() || WiFi.status() != WL_CONNECTED)
    {
        return false;
    }
    if (!isStorageModeSdCard())
    {
        return false;
    }
    if (String(SYSTEM_ASSETS_CDN_HOST).length() == 0)
    {
        return false;
    }

    logInfof("[SystemAssets] Force sound sync from CDN host=%s base=%s", SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_BASE_PATH);

    const String urlBoot = joinUrl(SYSTEM_ASSETS_CDN_HOST, SYSTEM_ASSETS_CDN_PORT, SYSTEM_ASSETS_CDN_BASE_PATH, "boot.wav");
    const String outBoot = system_assets_localBootWavPath();
    return downloadToFileBestEffort(urlBoot, outBoot.c_str(), "audio/wav");
}

