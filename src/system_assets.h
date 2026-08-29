#pragma once

#include <Arduino.h>

// System assets live on SD card so they can be updated without reflashing.
// CDN sync is optional and is disabled unless SYSTEM_ASSETS_CDN_HOST is set.

// Called once after WiFi is connected (best effort; non-blocking-ish).
void system_assets_syncFromCdnBestEffort();

// Force an immediate CDN sync of sound assets only (best effort).
// This ignores the reconnect cooldown used by the general sync.
bool system_assets_updateSoundsNow();

// Resolve local paths for common system assets.
// Returns empty string if storage isn't available / not mounted.
String system_assets_localSpaIndexPath(); // "/system/spa/index.html"
String system_assets_localSpaCssPath();   // "/system/spa/app.css"
String system_assets_localSpaJsPath();    // "/system/spa/app.js"
String system_assets_localBootWavPath();  // "/system/sounds/boot.wav"

