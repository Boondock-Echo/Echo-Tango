
`TANGO` is the unified Tango/Edge firmware variant. 

On Echo devices, enter the MQTT key in the Advanced page.

# useful commands

## Local web address

After the device joins a Wi-Fi network, it advertises its web interface using
mDNS (the ESP32 equivalent of a Linux `.local` hostname). The hostname is a
device setting, defaults to `boondock`, and can be changed under **Advanced →
WiFi Settings**. Open `http://<hostname>.local`, for example
`http://boondock.local`. `.local`
resolution requires the client to be on the same local network and the network
to permit multicast traffic.

PlatformIO automatically regenerates the gzip-compressed embedded SPA fallback
from the plain JavaScript source at `src/app_spa.js` before each build. To
regenerate it without compiling firmware, run
`python3 scripts/generate_spa_gzip.py`.


1. Erasing the EEPROM completely

pio run -e TANGO --target erase
