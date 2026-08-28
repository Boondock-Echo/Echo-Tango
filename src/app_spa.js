const API_BASE = '';
let deviceMac = '';
let audioGraphData = []; // Array of {db, clipping, timestamp} — timestamp = wall clock when sample was shown (for X axis + pruning)
let audioGraphMaxPoints = 100; // 10 seconds at 100ms intervals (100 * 100ms = 10s)
let audioGraphBuffer = []; // Buffer for incoming samples (1 second delay)
let audioGraphBufferInterval = null; // Interval to add samples from buffer to graph
const AUDIO_GRAPH_DELAY_MS = 1000; // 1 second delay before showing data
const AUDIO_SAMPLE_ADD_INTERVAL = 100; // Add one sample every 100ms
let globalModalResolve = null;
let currentAudioThreshold = 30; // Default threshold value
let currentRoute = 'home';
let deviceProductTitle = 'Boondock';
let homeDataInterval = null;
let audioDataInterval = null;
let audioGraphInterval = null;
let networkDataInterval = null;
let recordingsClockInterval = null;
let connectionLost = false;
let isLoading = false;
let firmwareAutoCheckDone = false;
let isTabVisible = true; // Track tab visibility state
let pollingPaused = false; // Track if polling was paused due to tab visibility
let RECORD_INPUT_CHANNEL_SUPPORTED = false;
let echoDevice = false;
let maxMaxRecordingTime = 30;

function applyProductBranding(productTitle) {
    if (!productTitle) return;
    deviceProductTitle = productTitle;
    const h1 = document.querySelector('.sidebar-header h1');
    if (h1) h1.textContent = productTitle;
}

const UI_THEME_STORAGE_KEY = 'boondock.uiThemeState.v2';
const UI_THEME_DEFAULT = 'night-ops';
const UI_THEME_PRESETS = {
    'night-ops': {
        label: 'Night Ops',
        description: 'Charcoal command panels with orange ember highlights.'
    },
    'ember-command': {
        label: 'Ember Command',
        description: 'Warm light command surfaces with restrained ember accents.'
    }
};
let uiThemeState = (() => {
    try {
        const saved = JSON.parse(localStorage.getItem(UI_THEME_STORAGE_KEY));
        return { selected: UI_THEME_PRESETS[saved && saved.selected] ? saved.selected : UI_THEME_DEFAULT };
    } catch (_) {
        return { selected: UI_THEME_DEFAULT };
    }
})();

function applyUiTheme(id, persist = true) {
    if (!UI_THEME_PRESETS[id]) id = UI_THEME_DEFAULT;
    uiThemeState.selected = id;
    document.documentElement.dataset.uiTheme = id;
    document.body.dataset.uiTheme = id;
    if (persist) {
        localStorage.setItem(UI_THEME_STORAGE_KEY, JSON.stringify({ selected: id }));
    }
    renderThemeControls();
}

function renderThemeControls() {
    const selector = document.getElementById('theme-selector');
    if (!selector) return;
    const selected = UI_THEME_PRESETS[uiThemeState.selected] ? uiThemeState.selected : UI_THEME_DEFAULT;
    selector.innerHTML = Object.entries(UI_THEME_PRESETS).map(([id, theme]) => `<button type="button" class="theme-choice-btn${id === selected ? ' active' : ''}" data-theme-id="${id}"><strong>${theme.label}</strong><small>${theme.description}</small></button>`).join('');
    selector.querySelectorAll('button').forEach(button => button.onclick = () => applyUiTheme(button.dataset.themeId));
}

applyUiTheme(uiThemeState.selected || UI_THEME_DEFAULT, false);

function applyDeviceCapabilities(deviceInfo) {
    if (!deviceInfo) return;

    if (typeof deviceInfo.recordInputChannelSupported === 'boolean') {
        RECORD_INPUT_CHANNEL_SUPPORTED = deviceInfo.recordInputChannelSupported;
    }
    if (deviceInfo.recordInputChannelSupported === false) {
        document.querySelectorAll('.audio-graph-input-channel-bar').forEach(el => el.remove());
    }
    if (deviceInfo.sdCardMounted && deviceInfo.recordToSdCard) {
        maxMaxRecordingTime = 180;
    }
    if (deviceInfo.firmware.startsWith('ECHO')) {
       echoDevice = true;
    }
}

/** Most handlers use apiOk() -> { "status": "ok" }; some also set success: true. */
function jsonApiOk(data) {
    return data && (data.status === 'ok' || data.success === true);
}

// WebSocket support
let ws = null;
let wsReconnectTimer = null;
let useWebSocket = true; // Try WebSocket first, fallback to polling
let wsConnected = false;
let currentSubscriptions = new Set(); // Track which pages we're subscribed to

// Recordings page (inbox browser)
let recordingsTreeExpanded = {};
let recordingsChildren = {};
let recordingsSelectedDayPath = '';
let recordingsPageNum = 1;
const RECORDINGS_PAGE_SIZE = 5;
let recordingsTreeLoadingPath = '';
let recordingsTableLoadGeneration = 0;
// Per-day path -> normalized items array (client-side; cleared on full page load).
let recordingsDaySummaryCache = {};

window.addEventListener('load', function() {
    recordingsDaySummaryCache = {};
});

// Centralized help text for settings (short, < 100 words each)
const SETTINGS_HELP = {
    // Audio
    'audio.gain': 'Audio Gain applies built-in amplification to the microphone. If recordings are too quiet, increase the gain until speech is clear but the Dynamic Range shows GOOD. Avoid settings that keep it in LOW or CLIPPING. Make small adjustments and watch the live meter.',
    'live.pauseRecording': 'Pauses automatic WAV capture from line-in. The live line-in stream to your browser keeps running. An open recording is closed; resume from this page.',
    'live.pauseUploads': 'Pauses cloud uploads; files stay queued. Reduces WiFi/CPU load while streaming.',
    'audio.inputChannel': 'Record input uses codec channel index 0 or 1. The web labels are swapped vs raw order so Right matches TRS-style wiring; values stored on the device are still 0 and 1. Changing the selection applies to monitoring immediately. On the Recorder page, use the bar under the level graph (or Save Settings below) to persist. (TANGO only.)',
    'audio.threshold': 'Audio threshold controls when automatic recordings start. Keep it just above your normal background noise so everyday hum does not trigger recordings, but voices or important sounds do. Watch the level bars and set threshold so only meaningful peaks cross the threshold line.',
    'audio.minRecordingSeconds': 'Minimum Recording Seconds defines the shortest allowed recording length. Higher values reduce very short clips and group brief sounds into one recording. If you see many tiny recordings, increase this value slightly until each event is captured as a single useful file.',
    'audio.maxRecordingSeconds': 'Maximum Recording Seconds limits how long a single recording can run. If sound continues longer than this value, the device stops the file at this duration and starts a new one. When recording to SD card is enabled, the maximum is 180 seconds (3 minutes). When recording to internal memory (PSRAM), the maximum is limited to 30 seconds due to memory constraints.',
    'audio.silenceThresholdMs': 'Silence Threshold is how many seconds of silence must pass before a recording stops. Short values end recordings quickly between pauses. Longer values keep one recording running through brief gaps in speech or sound.',
    'audio.preRecordMs': 'Pre Recording adds audio from the selected number of seconds before the trigger point. This helps capture the very start of a sound, such as the first word in a sentence.',
    'audio.discardEnabled': 'Discard Small Files removes very short recordings that are usually noise or accidental triggers. Turn this on to keep storage focused on meaningful events. Use the discard duration slider to set the minimum recording length that you consider worth keeping.',
    'audio.discardMillis': 'Discard Duration sets the minimum recording length that will be kept when Discard Small Files is enabled. Recordings shorter than this value are deleted automatically. Start around one second and adjust based on how many tiny files you see.',
    'audio.dynamicRange': 'Dynamic Range shows how well your gain and threshold are balanced. GOOD means levels are healthy. LOW suggests boosting gain or lowering the threshold; CLIPPING means the input is too loud or gain is too high and will cause distortion. Aim to keep it in GOOD most of the time.',

    // Network WiFi
    'net.wifiSsid': 'SSID is the WiFi network name this device will connect to. Enter the exact name as broadcast by your router. You can configure up to three networks; the device will try them in order until it connects. Leave unused entries blank.',
    'net.wifiPassword': 'Password is the WiFi key for the selected SSID. It must match your router’s security settings. For open networks leave this field empty. Keep this value secure; anyone with the password can join your network.',
    'net.staticIpEnabled': 'Use Static IP forces the device to use a fixed IP address instead of DHCP. Enable this if you want the device to always appear at the same address. When enabled, fill in Static IP, Subnet Mask, and Gateway carefully.',
    'net.staticIp': 'Static IP is the fixed address you want this device to use. It must be in the same subnet as your router and not conflict with other devices. Example: 192.168.1.100. Only used when “Use Static IP” is enabled.',
    'net.subnet': 'Subnet Mask defines the network size, typically 255.255.255.0 for home networks. It must match your router’s configuration. Only used when “Use Static IP” is enabled.',
    'net.gateway': 'Gateway is usually your router’s IP address, such as 192.168.1.1. It is required for internet access and cloud uploads when using a static IP. Only used when “Use Static IP” is enabled.',

    // SD card
    'sd.useSdCard': 'Enable SD Card turns on SD card support. When enabled, the device will attempt to mount and use an SD card for storage at boot. Disable this if you do not have a card installed or are troubleshooting SD-related issues.',
    'sd.recordToSdCard': 'Record to SD Card saves audio recordings onto the SD card instead of internal memory. This is recommended when using large or long-term recording setups. Requires SD card support to be enabled and the card to mount successfully.',
    'sd.mode1bit': 'Use 1-bit Mode switches the SD interface to a slower but more compatible mode. Enable this if you see SD mount errors or instability. For best performance leave it off, which uses the default 4-bit mode on supported cards.',
    'sd.frequency': 'SD Card Communication Frequency controls how fast data is clocked to the SD card. Lower values improve compatibility but reduce speed; higher values are faster but may cause issues with some cards or wiring. 10 MHz is the default recommended value.',
    'sd.formatIfMountFailed': 'Format if Mount Failed will automatically format the SD card when mounting fails. This can recover a corrupted card but permanently deletes all data. Use with caution, and only when you have backups or accept data loss.',

    // Firmware & Advanced
    'adv.firmwareUpdate': 'Firmware Update lets you install a new firmware image (.bin file) from your computer. Choose a .bin file, then click Upload & Update. The device will apply the update and reboot. Ensure the file matches your device model. Do not power off during the update.',
    'adv.uploadLocationsTest': 'Use the Test button next to each region or custom host to verify the device can reach that server before saving. This only checks network connectivity (TCP); it does not validate the API.',

    // Settings backup / import
    'adv.exportSettings': 'Export Settings downloads a JSON file with the current device configuration (WiFi credentials are masked). Use this to back up settings or copy them to another device. The file can later be imported to restore configuration.',
    'adv.importSettings': 'Import Settings loads a previously exported JSON settings file and applies it to this device. WiFi passwords and SSIDs marked as hidden are preserved from the current device. The device may reboot to apply all changes.',

    // System actions
    'adv.reboot': 'Reboot Device restarts the unit without changing any settings or recordings. Use this after firmware updates or configuration changes if the device appears stuck or unresponsive. The device will temporarily go offline during reboot.',
    'adv.setDefault': 'Set Default restores most configuration values (audio, upload, RTC, SD card, timezone, logs) to factory defaults while preserving WiFi credentials. Use this if settings become confusing or unstable. The device will reboot after applying defaults.',
    'adv.factoryReset': 'Factory Reset erases all stored settings, including WiFi and SD configuration, returning the device to a fresh state. Use this only as a last resort or before transferring ownership. The device will reboot into AP setup mode.'
};

// Common timezone options (United States first, then others)
const TIMEZONES = [
    // US timezones
    { id: 'US/Pacific', label: 'US Pacific (GMT-8)', offset: -8, isUS: true },
    { id: 'US/Mountain', label: 'US Mountain (GMT-7)', offset: -7, isUS: true },
    { id: 'US/Central', label: 'US Central (GMT-6)', offset: -6, isUS: true },
    { id: 'US/Eastern', label: 'US Eastern (GMT-5)', offset: -5, isUS: true },
    { id: 'US/Alaska', label: 'US Alaska (GMT-9)', offset: -9, isUS: true },
    { id: 'US/Hawaii', label: 'US Hawaii (GMT-10)', offset: -10, isUS: true },

    // Other common timezones
    { id: 'UTC', label: 'UTC (GMT+0)', offset: 0, isUS: false },
    { id: 'Europe/London', label: 'UK London (GMT+0)', offset: 0, isUS: false },
    { id: 'Europe/Berlin', label: 'Europe Central (GMT+1)', offset: 1, isUS: false },
    { id: 'Europe/Helsinki', label: 'Europe Eastern (GMT+2)', offset: 2, isUS: false },
    { id: 'Asia/Dubai', label: 'Asia Dubai (GMT+4)', offset: 4, isUS: false },
    { id: 'Asia/Kolkata', label: 'India (GMT+5)', offset: 5, isUS: false },
    { id: 'Asia/Bangkok', label: 'SE Asia (GMT+7)', offset: 7, isUS: false },
    { id: 'Asia/Shanghai', label: 'China (GMT+8)', offset: 8, isUS: false },
    { id: 'Asia/Tokyo', label: 'Japan (GMT+9)', offset: 9, isUS: false },
    { id: 'Australia/Sydney', label: 'Australia Sydney (GMT+10)', offset: 10, isUS: false }
];

// Helper to format WiFi RSSI with quality label
function formatWifiRssi(rssi) {
    if (rssi === null || rssi === undefined || Number.isNaN(rssi)) {
        return '--';
    }
    const value = Number(rssi);
    let quality = 'Poor';
    if (value >= -50) {
        quality = 'Excellent';
    } else if (value >= -60) {
        quality = 'Great';
    } else if (value >= -70) {
        quality = 'Good';
    } else if (value >= -80) {
        quality = 'Medium';
    } else {
        quality = 'Poor';
    }
    return `${value} dBm (${quality})`;
}

// Page templates
const pageTemplates = {
    home: `
        <div class="home-top-section">
            <div class="summary-grid">
                <div class="summary-card">
                    <h3>📱 Device Information</h3>
                    <div class="info-list">
                        <div class="info-row">
                            <span class="info-label">🆔 Device ID:</span>
                            <span class="info-value" id="device-id-value">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">⚙️ Firmware:</span>
                            <span class="info-value" id="firmware-version">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">🌐 IP Address:</span>
                            <span class="info-value" id="ip-address">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">📶 WiFi SSID:</span>
                            <span class="info-value" id="wifi-ssid">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">📡 WiFi RSSI:</span>
                            <span class="info-value" id="wifi-rssi">--</span>
                        </div>
                    </div>
                </div>
                <div class="summary-card">
                    <h3>🎙️ Recording Status</h3>
                    <div class="info-list">
                        <div class="info-row">
                            <span class="info-label">⚡ Recorder:</span>
                            <span class="info-value" id="recording-status">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">📤 Network:</span>
                            <span class="info-value" id="uploading-status">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">📊 Recorded:</span>
                            <span class="info-value" id="recording-count">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">✅ Uploaded:</span>
                            <span class="info-value" id="uploaded-count">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">⚠️ Errors:</span>
                            <span class="info-value" id="error-count">--</span>
                        </div>
                    </div>
                </div>
                <div class="summary-card">
                    <h3>💿 Storage</h3>
                    <div class="info-list">
                        <div class="info-row">
                            <span class="info-label">🔧 Recording Mode:</span>
                            <span class="info-value" id="storage-mode">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">💾 SD Card Enabled:</span>
                            <span class="info-value" id="sd-enabled">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">🎛 SD Card Mode:</span>
                            <span class="info-value" id="sd-mode-speed">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">💾 Total Size:</span>
                            <span class="info-value" id="storage-total">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">📦 Usage:</span>
                            <span class="info-value" id="storage-usage">--</span>
                        </div>
                    </div>
                </div>
                <div class="summary-card">
                    <h3>💚 System Health</h3>
                    <div class="info-list">
                        <div class="info-row">
                            <span class="info-label">🧠 Memory:</span>
                            <span class="info-value" id="heap-free">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">🧩 PSRAM Usage:</span>
                            <span class="info-value" id="psram-usage">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">🔁 Boot Reason:</span>
                            <span class="info-value" id="boot-reason">--</span>
                        </div>
                      <div class="info-row">
                          <span class="info-label">🛠 Daily Maintenance:</span>
                          <span class="info-value" id="maintenance-time">--</span>
                      </div>
                        <div class="info-row">
                            <span class="info-label">⏱️ Uptime:</span>
                            <span class="info-value" id="uptime">--</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        <div class="repeater-section" id="repeater-card">
            <div class="repeater-header">
                <h3>📻 Repeater Mode</h3>
                <div class="repeater-subtitle">Receive audio and retransmit it (Simplex or Duplex). TX must be enabled.</div>
            </div>
            <div class="repeater-controls">
                <div class="repeater-control">
                    <div class="repeater-label">Enable repeater</div>
                    <div class="repeater-value">
                        <label class="switch">
                            <input type="checkbox" id="repeater-enabled">
                            <span class="slider"></span>
                        </label>
                        <span class="repeater-status" id="repeater-status">--</span>
                    </div>
                </div>
                <div class="repeater-control">
                    <div class="repeater-label">Mode</div>
                    <div class="repeater-value repeater-radios">
                        <label class="radio-inline">
                            <input type="radio" name="repeater-mode" id="repeater-mode-simplex" value="1" disabled>
                            Simplex (record then transmit)
                        </label>
                        <label class="radio-inline">
                            <input type="radio" name="repeater-mode" id="repeater-mode-duplex" value="2" disabled>
                            Duplex (transmit live)
                        </label>
                    </div>
                </div>
            </div>
        </div>
        <div class="firmware-update-section" style="margin-top: 20px; padding: 20px; background: var(--ui-border); border-radius: 8px;">
            <h3 style="margin-top: 0;">Firmware Update</h3>
            <div id="firmware-check-status" style="margin-bottom: 10px;">
                <button id="check-firmware-btn" class="btn-primary">\uD83D\uDD0D Check for Updates</button>
            </div>
            <div id="firmware-update-result" style="display: none;">
                <div id="firmware-update-message" style="margin-bottom: 10px; padding: 10px; background: white; border-radius: 4px;"></div>
                <div id="firmware-update-actions" style="display: none;">
                    <a id="firmware-download-link" href="#" target="_blank" class="btn-secondary" style="display: inline-block; margin-right: 10px; text-decoration: none;">\uD83D\uDCE5 Download Firmware</a>
                    <button id="apply-firmware-btn" class="btn-primary">\u2705 Apply Update</button>
                </div>
                <div style="margin-top: 15px; padding: 10px; background: rgba(255,255,255,0.7); border-radius: 4px; font-size: 12px; color: var(--ui-muted);">
                    <strong>Alternative:</strong> You can also go to the <strong>Advanced</strong> tab and upload the firmware file manually.
                </div>
            </div>
        </div>
    `,
    audio: `
        <div class="status-buttons">
            <button id="recording-btn" class="status-btn recording">📄 RECORDING</button>
            <button id="idle-btn" class="status-btn idle">🌐 IDLE</button>
            
        </div>
        <div class="audio-graph-container">
            <canvas id="audio-graph" width="800" height="300"></canvas>
            <div id="audio-graph-input-channel-bar" class="audio-graph-input-channel-bar">
                <div class="audio-graph-input-channel-inner">
                    <span class="audio-graph-input-channel-label">
                        Record input channel
                        <span class="help-icon" data-help-key="audio.inputChannel">🛈</span>
                    </span>
                    <fieldset class="radio-fieldset audio-graph-channel-fieldset">
                        <label class="audio-graph-channel-option">
                            <input type="radio" name="record-input-channel" id="record-input-left" value="1" checked>
                            Left ( Default )
                        </label>
                        <label class="audio-graph-channel-option">
                            <input type="radio" name="record-input-channel" id="record-input-right" value="0">
                            Right
                        </label>
                    </fieldset>
                    <button type="button" id="audio-graph-save-input-channel" class="btn-secondary">\uD83D\uDCBE Save input channel</button>
                </div>
            </div>
        </div>
        <div class="audio-settings-card">
        <h3>Recording Settings</h3>
        <div class="audio-controls">
            <div class="control-group">
                <label for="audio-gain" id="audio-gain-label">
                    Audio Gain: 0 dB
                    <span class="help-icon" data-help-key="audio.gain">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="audio-gain" class="slider" min="0" max="9" value="1" step="1">
                    <span class="slider-value" id="audio-gain-value">0 dB</span>
                </div>
            </div>
            <div class="control-group">
                <label for="audio-threshold" id="audio-threshold-label">
                    Audio Threshold ( 0 = Low, 60 = High )
                    <span class="help-icon" data-help-key="audio.threshold">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="audio-threshold" class="slider" min="0" max="60" value="30" step="1">
                    <span class="slider-value" id="audio-threshold-value">30</span>
                </div>
            </div>
            <div class="control-group">
                <label for="min-recording-seconds">
                    Minimum Recording Seconds
                    <span class="help-icon" data-help-key="audio.minRecordingSeconds">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="min-recording-seconds" class="slider" min="1" max="60" value="5" step="0.5">
                    <span class="slider-value" id="min-recording-seconds-value">5</span>
                </div>
            </div>
            <div class="control-group">
                <label for="max-recording-seconds">
                    Maximum Recording Seconds
                    <span class="help-icon" data-help-key="audio.maxRecordingSeconds">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="max-recording-seconds" class="slider" min="1" max="30" value="30" step="1">
                    <span class="slider-value" id="max-recording-seconds-value">30</span>
                </div>
            </div>
            <div class="control-group">
                <label for="silence-threshold">
                    Silence Threshold Seconds
                    <span class="help-icon" data-help-key="audio.silenceThresholdMs">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="silence-threshold" class="slider" min="0.5" max="10" value="2" step="0.5">
                    <span class="slider-value" id="silence-threshold-value">2</span>
                </div>
            </div>
            <div class="control-group">
                <label for="pre-record-ms">
                    Pre Recording Seconds
                    <span class="help-icon" data-help-key="audio.preRecordMs">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="pre-record-ms" class="slider" min="0" max="5" value="3" step="0.5">
                    <span class="slider-value" id="pre-record-ms-value">3</span>
                </div>
            </div>
            <div class="control-group">
                <label for="discard-enabled">
                    <input type="checkbox" id="discard-enabled">
                    Discard Small Files
                    <span class="help-icon" data-help-key="audio.discardEnabled">🛈</span>
                </label>
                <div class="slider-container" id="discard-slider-container" style="display:none;">
                    <input type="range" id="discard-millis" class="slider" min="0.5" max="5" value="1" step="0.5">
                    <span class="slider-value" id="discard-millis-value">1</span>
                    <span class="help-icon-inline" data-help-key="audio.discardMillis">🛈</span>
                </div>
            </div>
        </div>
        <div class="audio-actions action-buttons">
            <button id="audio-set-defaults-btn" class="btn-secondary">\uD83D\uDD27 Set Defaults</button>
            <button id="audio-save-settings-btn" class="btn-primary">\uD83D\uDCBE Save Settings</button>
        </div>
        </div>
    `,
    'live-audio': `
        <div class="live-audio-container">
            <div class="live-audio-status">
                <div class="live-audio-controls-row">
                    <div class="status-indicator" id="live-audio-status">
                        <span class="status-dot" id="status-dot"></span>
                        <span id="status-text">Connecting...</span>
                    </div>
                    <div class="audio-controls-live">
                        <button id="start-live-audio-btn" class="btn-primary">\uD83D\uDD0A Start Live Audio</button>
                        <button id="stop-live-audio-btn" class="btn-secondary" style="display:none;">\u23F9\uFE0F Stop Live Audio</button>
                    </div>
                </div>
                <div class="live-audio-visualizer">
                    <canvas id="live-audio-visualizer-canvas" width="800" height="200"></canvas>
                </div>
            </div>
            <div class="info-card" style="margin-bottom:16px;">
                <h3 style="margin-top:0;">Live streaming session</h3>
                <p class="form-help live-audio-session-hint">Pause line-in WAV capture and/or uploads while streaming here. Browser audio still follows live line-in. Resets when you leave this page or stop live audio.</p>
                <div class="control-group" style="margin-top:8px;">
                    <label style="display:flex;align-items:center;gap:8px;cursor:pointer;">
                        <input type="checkbox" id="live-audio-pause-recording">
                        <span>Pause line-in recording</span>
                        <span class="help-icon" data-help-key="live.pauseRecording">🛈</span>
                    </label>
                </div>
                <div class="control-group">
                    <label style="display:flex;align-items:center;gap:8px;cursor:pointer;">
                        <input type="checkbox" id="live-audio-pause-uploads">
                        <span>Pause uploads</span>
                        <span class="help-icon" data-help-key="live.pauseUploads">🛈</span>
                    </label>
                </div>
            </div>
            <div class="live-audio-info">
                <div class="info-card">
                    <h3>Stream Information</h3>
                    <div class="info-list">
                        <div class="info-row">
                            <span class="info-label">Sample Rate:</span>
                            <span class="info-value" id="live-audio-sample-rate">8000 Hz</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">Buffer Status:</span>
                            <span class="info-value" id="live-audio-buffer-status">--</span>
                        </div>
                        <div class="info-row">
                            <span class="info-label">Stream Duration:</span>
                            <span class="info-value" id="live-audio-latency">--</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    `,
    'player-tx': `
        <div class="audio-page">
            <div class="audio-controls" style="margin-top:0;">
                <div class="control-group">
                    <label for="echo-speaker-enabled">
                        <input type="checkbox" id="echo-speaker-enabled">
                        Enable Speaker
                    </label>
                    <p class="form-help">Controls the onboard speaker output (mute/unmute). Browser playback is unaffected.</p>
                </div>
                <div class="control-group" id="echo-speaker-volume-group">
                    <label for="echo-speaker-volume">Speaker Volume</label>
                    <div class="slider-container">
                        <input type="range" id="echo-speaker-volume" class="slider" min="0" max="100" value="50" step="1">
                        <span class="slider-value" id="echo-speaker-volume-value">50</span>
                    </div>
                </div>
                <hr style="margin:16px 0; border:0; border-top:1px solid var(--ui-border, #ddd);">
                <div class="control-group">
                    <label for="echo-tx-enabled">
                        <input type="checkbox" id="echo-tx-enabled">
                        Enable Transmit (TX)
                    </label>
                    <p class="form-help">TX is not implemented in this firmware yet. This setting is stored for future legacy parity.</p>
                </div>
                <div class="control-group" id="echo-tx-volume-group">
                    <label for="echo-tx-volume">Transmit Volume</label>
                    <div class="slider-container">
                        <input type="range" id="echo-tx-volume" class="slider" min="0" max="100" value="50" step="1">
                        <span class="slider-value" id="echo-tx-volume-value">50</span>
                    </div>
                </div>
            </div>
        </div>
    `,

    cw: `
        <div class="cw-page">
            <div class="cw-layout">
                <section class="cw-panel cw-panel-compose">
                    <div class="cw-panel-header">
                    </div>

                    <div class="cw-message">
                        <label for="cw-text">Message</label>
                        <textarea id="cw-text" rows="4" class="form-control cw-textarea" placeholder="Example: CQ CQ DE BOONDOCK"></textarea>
                        <div class="cw-actions">
                            <button id="cw-send" type="button" class="btn-primary">▶ Send</button>
                            <button id="cw-stop" type="button" class="btn-secondary btn-warning">⏹ Stop</button>
                            <button id="cw-clear" type="button" class="btn-secondary">🧹 Clear</button>
                        </div>
                        <div id="cw-status" class="form-help cw-status" aria-live="polite"></div>
                    </div>
                    <hr style="margin:16px 0; border:0; border-top:1px solid var(--ui-border, #ddd);">
                    <h3 style="margin:0 0 8px 0;">History (this browser)</h3>
                    <div class="cw-history" id="cw-history"></div>
                    <div class="cw-history-actions">
                        <button id="cw-history-clear" type="button" class="btn-secondary btn-warning">🗑️ Clear history</button>
                    </div>

                    <hr style="margin:16px 0; border:0; border-top:1px solid var(--ui-border, #ddd);">
                    <h3 style="margin:0 0 8px 0;">Settings</h3>
                    <div class="cw-settings-grid">
                        <div class="cw-field">
                            <label for="cw-wpm">Speed (WPM)</label>
                            <div class="slider-container">
                                <input type="range" id="cw-wpm" class="slider" min="5" max="40" value="18" step="1">
                                <span class="slider-value" id="cw-wpm-value">18</span>
                            </div>
                        </div>
                        <div class="cw-field">
                            <label for="cw-tone">Tone (Hz)</label>
                            <div class="slider-container">
                                <input type="range" id="cw-tone" class="slider" min="500" max="800" value="600" step="10">
                                <span class="slider-value" id="cw-tone-value">600</span>
                            </div>
                        </div>
                        <div class="cw-field">
                            <label for="cw-volume">Volume</label>
                            <div class="slider-container">
                                <input type="range" id="cw-volume" class="slider" min="0" max="100" value="60" step="1">
                                <span class="slider-value" id="cw-volume-value">60</span>
                            </div>
                        </div>
                        <div class="cw-field">
                            <label for="cw-repeat">Repeat</label>
                            <div class="slider-container">
                                <input type="range" id="cw-repeat" class="slider" min="1" max="5" value="1" step="1">
                                <span class="slider-value" id="cw-repeat-value">1</span>
                            </div>
                        </div>
                    </div>
                    <div style="margin-top:12px; display:flex; justify-content:flex-end; gap:8px; flex-wrap:wrap;">
                        <button id="cw-save-settings" type="button" class="btn-primary">💾 Save CW settings</button>
                    </div>
                </section>
            </div>
        </div>
    `,
    recordings: `
        <div class="recordings-page">
            <p class="form-help" id="recordings-storage-hint" style="margin-bottom:12px;color:#b45309;"></p>
            <div class="recordings-layout">
                <div class="recordings-pane recordings-pane-tree">
                    <h3 class="recordings-pane-title">Recordings folders</h3>
                    <div id="recordings-tree-root" class="recordings-tree-root"></div>
                    <div id="recordings-tree-error" class="recordings-error" style="display:none;"></div>
                </div>
                <div class="recordings-pane recordings-pane-list">
                    <h3 class="recordings-pane-title">Day recordings</h3>
                    <p id="recordings-day-label" class="recordings-day-label">Select a day folder (YYYY / MON / DD) on the left.</p>
                    <div id="recordings-table-loading" class="recordings-table-loading" style="display:none;" role="status" aria-live="polite">
                        <span class="recordings-spinner" aria-hidden="true"></span>
                        <span class="recordings-table-loading-text">Loading recordings…</span>
                    </div>
                    <div class="recordings-table-wrap">
                        <table class="recordings-table">
                            <thead>
                                <tr>
                                    <th>🔊 Filename</th>
                                    <th>Recorded</th>
                                    <th>Size</th>
                                    <th>Play</th>
                                    <th>Download</th>
                                </tr>
                            </thead>
                            <tbody id="recordings-table-body"></tbody>
                        </table>
                    </div>
                    <div class="recordings-pagination-bar" id="recordings-pagination-bar" style="display:none;">
                        <div class="recordings-pagination" id="recordings-pagination" style="display:none;">
                            <button type="button" id="recordings-prev-page" class="btn-secondary">\u2B05\uFE0F Previous</button>
                            <span id="recordings-page-indicator"></span>
                            <button type="button" id="recordings-next-page" class="btn-secondary">Next \u27A1\uFE0F</button>
                        </div>
                        <button type="button" id="recordings-refresh-day" class="btn-secondary btn-warning recordings-refresh-btn">🔄 Refresh</button>
                    </div>
                    <audio id="recordings-player" controls preload="none" class="recordings-player"></audio>
                    <div id="recordings-list-error" class="recordings-error" style="display:none;"></div>
                </div>
            </div>
        </div>
    `,
    network: `
        <div id="wifi-configs">
            <!-- WiFi configurations will be dynamically loaded -->
        </div>
        <div class="network-page-actions" style="margin-top:1.5rem; padding-top:1rem; border-top:1px solid var(--ui-border, #ddd); display:flex; gap:10px; align-items:center;">
            <button id="network-save-btn" class="btn-primary" disabled>\uD83D\uDCBE Save</button>
            <button id="network-cancel-btn" class="btn-secondary">\u274C Cancel</button>
        </div>
    `,
    advanced: `
        <div class="advanced-section">
            <h3>Appearance Theme</h3>
            <p class="form-help">Choose Night Ops dark mode or Ember Command light mode. Changes apply immediately and are saved in this browser.</p>
            <div id="theme-selector" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px"></div>
        </div>
        <div class="advanced-section">
            <h3>SD Card Settings</h3>
            <div class="form-group">
                <label>
                    <input type="checkbox" id="sd-card-enabled"> Enable SD Card
                    <span class="help-icon" data-help-key="sd.useSdCard">🛈</span>
                </label>
            </div>
            <div id="sd-card-extra-settings">
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="record-to-sd-card"> Record to SD Card
                        <span class="help-icon" data-help-key="sd.recordToSdCard">🛈</span>
                    </label>
                    <p class="form-help">When enabled, recordings will be saved to the SD card.</p>
                </div>
                <div class="form-group">
                    <label>
                        SD Card Mode
                        <span class="help-icon" data-help-key="sd.frequency">🛈</span>
                    </label>
                    <select id="sd-card-mode-select" class="form-control">
                        <option value="sloth">Sloth Mode (1-bit, 5 MHz)</option>
                        <option value="slow">Slow (1-bit, 10 MHz)</option>
                        <option value="medium">Medium (4-bit, 5 MHz)</option>
                        <option value="fast">Fast (4-bit, 10 MHz)</option>
                        <option value="insane">Insane (4-bit, 20 MHz)</option>
                    </select>
                    <p class="form-help">Choose a preset that sets both SD card mode (1-bit / 4-bit) and clock speed.</p>
                </div>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="sd-card-format-if-mount-failed"> Format if Mount Failed
                        <span class="help-icon" data-help-key="sd.formatIfMountFailed">🛈</span>
                    </label>
                    <p class="form-help">⚠️ WARNING: When enabled, the SD card will be automatically formatted if mount fails. This will delete all data!</p>
                </div>
            </div>
            <div class="action-buttons" style="margin-top:10px;">
                <button id="test-sd-card-btn" class="btn-secondary">\uD83E\uDDEA Test SD Card</button>
            </div>

            <h3 style="margin-top:24px;">Timezone</h3>
            <div class="form-group">
                <label>Device Timezone</label>
                <select id="timezone-select" class="form-control"></select>
                <p class="form-help">Select the local timezone for this device. This is used for scheduling and timestamps.</p>
            </div>

            <h3 style="margin-top:24px;">WiFi Settings</h3>
            <div class="form-group">
                <label>Local Hostname</label>
                <input type="text" id="local-hostname" class="form-control" maxlength="63" placeholder="boondock">
                <p class="form-help">Address used for mDNS, for example boondock.local. Use letters, numbers, and interior hyphens. A reboot is required after changing it.</p>
            </div>
            <div class="form-group">
                <label>
                    WiFi Tx Power
                    <span class="help-icon" data-help-key="wifi.txPower">🛈</span>
                </label>
                <div class="slider-container">
                    <input type="range" id="wifi-tx-power" class="slider" min="1" max="10" value="10" step="1">
                    <span class="slider-value" id="wifi-tx-power-value">10</span>
                </div>
                <p class="form-help">WiFi transmission power level (1-10). Higher values increase range but may consume more power. Default is 10.</p>
            </div>
            <div class="form-group">
                <label>MQTT Key</label>
                <input type="password" id="mqtt-key" class="form-control" maxlength="127" autocomplete="new-password" placeholder="Enter a new MQTT key">
                <label><input type="checkbox" id="mqtt-key-clear"> Clear saved MQTT key</label>
                <p class="form-help" id="mqtt-key-status">The saved key is never displayed. Leave blank to keep it unchanged; enter a new value to replace it. A reboot is required.</p>
            </div>
        </div>
        <div class="advanced-section">
            <h3>Firmware Update</h3>
            <p class="form-help">Upload a firmware (.bin) file to update the device. Choose a file that matches your device model, then click Upload & Update. The device will install the update and reboot; do not power off during the process.</p>
            <div class="form-group">
                <label>
                    Select Firmware File (.bin)
                    <span class="help-icon" data-help-key="adv.firmwareUpdate">🛈</span>
                </label>
                <input type="file" id="firmware-file-input" accept=".bin" class="form-control">
                <div id="firmware-progress" style="display:none; margin-top:10px;">
                    <div style="background:#f0f0f0; border-radius:4px; height:20px; overflow:hidden;">
                        <div id="firmware-progress-bar" style="background:var(--ui-accent); height:100%; width:0%; transition:width 0.3s;"></div>
                    </div>
                    <div id="firmware-progress-text" style="margin-top:5px; font-size:12px; color:var(--ui-muted);">0%</div>
                </div>
                <button id="update-firmware-btn" class="btn-primary" style="margin-top:10px;">\uD83D\uDCE4 Upload & Update Firmware</button>
            </div>
        </div>
        <div class="advanced-section">
            <h3>Export & Import Settings</h3>
            <p class="form-help">Export saves your current configuration to a JSON file (for backup or transfer). Import applies a previously exported file to this device. WiFi credentials from the current device are preserved when importing.</p>
            <div class="action-buttons">
                <button id="export-settings-btn" class="btn-secondary btn-info">📤 Export Settings</button>
                <button id="import-settings-btn" class="btn-secondary btn-warning">📥 Import Settings</button>
                <input type="file" id="settings-file-input" accept=".json" style="display:none;">
            </div>
        </div>
        <div class="advanced-section">
            <h3>System Actions</h3>
            <p class="form-help">Reboot restarts the device without changing settings. Set Default restores most options (audio, upload regions, timezone, etc.) to factory defaults but keeps WiFi; the device will reboot. Factory Reset erases all settings and WiFi; use only as a last resort—the device will reboot into setup mode.</p>
            <div class="action-buttons">
                <button id="reboot-btn" class="btn-secondary btn-warning">\uD83D\uDD04 Reboot Device</button>
                <button id="set-default-btn" class="btn-secondary">\uD83D\uDD27 Set Default</button>
                <button id="factory-reset-btn" class="btn-secondary btn-danger">\u26A0\uFE0F Factory Reset</button>
            </div>
        </div>
        <div class="advanced-section">
            <h3>Upload Locations</h3>
            <p class="form-help">Choose which regions to upload to (Ohio, Oregon, Virginia) or enable a custom host. Use the Test button next to each option to verify connectivity before saving. When Custom Host is enabled, only the custom host is used; the region checkboxes are ignored.</p>
            <div class="form-group theme-warning" style="border-radius: 6px; padding: 10px 12px; margin-bottom: 12px;">
                <strong>Warning:</strong> Changing upload server settings can have unexpected effects: recordings and events may stop reaching the cloud, device health may not report correctly, and firmware/configuration sync may fail until the device can reach a valid server. Use custom hosts only if you know the endpoint is compatible. Revert to default regions (Ohio, Oregon, Virginia) if uploads fail.
            </div>
            <div class="form-group upload-regions-group">
                <span class="upload-region-row"><label style="margin:0;"><input type="checkbox" id="upload-region-ohio"> 🌐 Boondock - Ohio</label><button id="test-upload-region-0" class="btn-secondary" type="button" style="padding:4px 10px;font-size:12px;">\uD83E\uDDEA Test</button></span>
                <span class="upload-region-row"><label style="margin:0;"><input type="checkbox" id="upload-region-oregon"> 🌐 Boondock - Oregon</label><button id="test-upload-region-1" class="btn-secondary" type="button" style="padding:4px 10px;font-size:12px;">\uD83E\uDDEA Test</button></span>
                <span class="upload-region-row"><label style="margin:0;"><input type="checkbox" id="upload-region-virginia"> 🌐 Boondock - Virginia</label><button id="test-upload-region-2" class="btn-secondary" type="button" style="padding:4px 10px;font-size:12px;">\uD83E\uDDEA Test</button></span>
            </div>
            <div class="form-group upload-custom-row">
                <label style="margin:0; display:flex; align-items:center;"><input type="checkbox" id="upload-use-custom-host"> Use custom host</label>
                <span id="upload-custom-host-fields" class="upload-custom-fields" style="display:none;">
                    <input type="text" id="upload-custom-host" placeholder="hostname or IP" class="form-control" maxlength="63" style="min-width:160px;">
                    <input type="number" id="upload-custom-port" placeholder="7001" min="1" max="65535" class="form-control" style="width:88px;">
                    <button id="test-upload-region-3" class="btn-secondary" type="button">\uD83E\uDDEA Test</button>
                </span>
            </div>
            <div class="advanced-page-actions" style="margin-top:1.5rem; padding-top:1rem; border-top:1px solid var(--ui-border, #ddd); display:flex; gap:10px; align-items:center;">
                <button id="advanced-save-btn" class="btn-primary" disabled>\uD83D\uDCBE Save</button>
                <button id="advanced-cancel-btn" class="btn-secondary">\u274C Cancel</button>
            </div>
        </div>
    `
};

// Page titles and icons
const pageInfo = {
    home: { icon: '📊', title: 'Device Summary' },
    audio: { icon: '⏺️', title: 'Recorder' },
    'live-audio': { icon: '🔊', title: 'Live Audio' },
    'player-tx': { icon: '🎚️', title: 'Player & TX' },
    cw: { icon: '📡', title: 'CW ( Morse )' },
    recordings: { icon: '📁', title: 'Recordings' },
    network: { icon: '🌐', title: 'Network Configuration' },
    advanced: { icon: '⚙️', title: 'Advanced Settings' }
};

// Routing functions
function getRoute() {
    const hash = window.location.hash.slice(1);
    if (hash) return hash;
    const path = (window.location.pathname || '/').replace(/^\/+/, '');
    return path || 'home';
}


function navigateTo(route) {
    currentRoute = route;
    window.location.hash = route;
    renderPage(route);
}

function showLoadingPopup() {
    isLoading = true;
    const popup = document.getElementById('loading-popup');
    if (popup) popup.style.display = 'flex';
}

function hideLoadingPopup() {
    isLoading = false;
    const popup = document.getElementById('loading-popup');
    if (popup) popup.style.display = 'none';
}

function showConnectionError() {
    if (connectionLost) return; // Already showing
    
    // Only show connection error if we've had multiple consecutive failures
    // This prevents false positives from transient network issues
    if (!window.connectionErrorCount) {
        window.connectionErrorCount = 0;
    }
    window.connectionErrorCount++;
    
    // Require at least 2 consecutive failures before showing error
    if (window.connectionErrorCount < 2) {
        return;
    }
    
    connectionLost = true;
    stopAllPolling();
    const popup = document.getElementById('connection-error-popup');
    if (popup) popup.style.display = 'flex';
}

function hideConnectionError() {
    connectionLost = false;
    window.connectionErrorCount = 0; // Reset error count on successful connection
    const popup = document.getElementById('connection-error-popup');
    if (popup) popup.style.display = 'none';
}

function showRebootCountdown() {
    const popup = document.getElementById('reboot-countdown-popup');
    if (!popup) return;
    
    popup.style.display = 'flex';
    let countdown = 30;
    const countdownNumber = document.getElementById('countdown-number');
    
    const updateCountdown = () => {
        if (countdownNumber) {
            countdownNumber.textContent = countdown;
        }
        
        if (countdown <= 0) {
            window.location.reload();
            return;
        }
        
        countdown--;
        setTimeout(updateCountdown, 1000);
    };
    
    updateCountdown();
    
    // Refresh now button
    const refreshNowBtn = document.getElementById('refresh-now-btn');
    if (refreshNowBtn) {
        refreshNowBtn.addEventListener('click', function() {
            window.location.reload();
        });
    }
}

function stopAllPolling() {
    if (homeDataInterval) {
        clearInterval(homeDataInterval);
        homeDataInterval = null;
    }
    if (audioDataInterval) {
        clearInterval(audioDataInterval);
        audioDataInterval = null;
    }
    if (audioGraphInterval) {
        clearInterval(audioGraphInterval);
        audioGraphInterval = null;
    }
    if (audioGraphBufferInterval) {
        clearInterval(audioGraphBufferInterval);
        audioGraphBufferInterval = null;
    }
    if (networkDataInterval) {
        clearInterval(networkDataInterval);
        networkDataInterval = null;
    }
    if (recordingsClockInterval) {
        clearInterval(recordingsClockInterval);
        recordingsClockInterval = null;
    }
}

// Pause all polling when tab becomes inactive
function pausePolling() {
    if (pollingPaused) return; // Already paused
    
    pollingPaused = true;
    console.log('Tab inactive - pausing polling');
    
    // Stop all intervals
    if (homeDataInterval) {
        clearInterval(homeDataInterval);
        homeDataInterval = null;
    }
    if (audioDataInterval) {
        clearInterval(audioDataInterval);
        audioDataInterval = null;
    }
    // Stop buffer drain; graph draw interval keeps running but drawGraph skips when !isTabVisible
    if (audioGraphBufferInterval) {
        clearInterval(audioGraphBufferInterval);
        audioGraphBufferInterval = null;
    }
    audioGraphBuffer.length = 0;
    audioGraphData.length = 0;
    if (networkDataInterval) {
        clearInterval(networkDataInterval);
        networkDataInterval = null;
    }
    if (recordingsClockInterval) {
        clearInterval(recordingsClockInterval);
        recordingsClockInterval = null;
    }
    
    // Close WebSocket to save resources when tab is inactive
    if (wsConnected && ws) {
        ws.close();
    }
}

// Resume polling when tab becomes active
function resumePolling() {
    if (!pollingPaused) return; // Not paused
    
    pollingPaused = false;
    console.log('Tab active - resuming polling');
    
    // Reset connection state and try to reconnect
    connectionLost = false;
    hideConnectionError();
    
    // Reconnect WebSocket if enabled
    if (useWebSocket && (!ws || ws.readyState !== WebSocket.OPEN)) {
        connectWebSocket();
    }
    
    // Resume polling based on current route
    if (currentRoute === 'home') {
        // Immediately try to load data and then resume polling
        loadHomeData().then(() => {
            if (!connectionLost && !homeDataInterval && isTabVisible && !useWebSocket) {
                homeDataInterval = setInterval(loadHomeData, 1000);
            }
        });
        // Subscribe to WebSocket if connected
        if (wsConnected && useWebSocket) {
            subscribeToPage('home');
        }
    } else if (currentRoute === 'audio') {
        // Resume graph drawing first (always needed for audio page)
        if (!audioGraphInterval && isTabVisible) {
            initAudioGraph();
        }
        // Resume data updates
        if (wsConnected && useWebSocket) {
            // WebSocket mode - just subscribe, data comes via WebSocket
            subscribeToPage('audio');
            loadAudioData(); // Load initial data
        } else {
            // Polling mode - resume polling interval
            loadAudioData().then(() => {
                if (!connectionLost && !audioDataInterval && isTabVisible && !useWebSocket) {
                    audioDataInterval = setInterval(loadAudioData, 500);
                }
            });
        }
    } else if (currentRoute === 'network') {
        // Immediately try to load data and then resume polling
        loadNetworkConfig().then(() => {
            if (!connectionLost && !networkDataInterval && isTabVisible && !useWebSocket) {
                networkDataInterval = setInterval(loadNetworkConfig, 1000);
            }
        });
        // Subscribe to WebSocket if connected
        if (wsConnected && useWebSocket) {
            subscribeToPage('network');
        }
    } else if (currentRoute === 'recordings') {
        syncDeviceClockFromSummary();
        if (!recordingsClockInterval && isTabVisible) {
            recordingsClockInterval = setInterval(syncDeviceClockFromSummary, 5000);
        }
    }
}

function renderPage(route) {
    const contentBody = document.getElementById('content-body');
    const pageTitle = document.getElementById('page-title');
    const titleIcon = document.getElementById('title-icon');
    const titleText = document.getElementById('title-text');
    
    if (!contentBody) return;
    
    // Show loading popup
    showLoadingPopup();
    
    // Update active menu item
    document.querySelectorAll('.nav-item').forEach(item => {
        item.classList.remove('active');
        if (item.getAttribute('data-route') === route) {
            item.classList.add('active');
        }
    });
    
    // Update page title
    if (pageInfo[route]) {
        if (titleIcon) titleIcon.textContent = pageInfo[route].icon;
        if (titleText) titleText.textContent = pageInfo[route].title;
        document.title = deviceProductTitle + ' - ' + pageInfo[route].title;
    }
    const titleExtra = document.getElementById('page-title-extra');
    if (titleExtra && route === 'recordings') {
        titleExtra.style.display = 'block';
        titleExtra.innerHTML = '<strong>UTC:</strong> Folder tree and filenames use UTC (your local calendar day may differ).<span class="page-title-extra-tz" id="page-title-extra-tz"></span>';
    }
    
    // Clear intervals from previous page and unsubscribe from WebSocket
    // Always unsubscribe from pages that are not the current route (regardless of interval existence)
    if (route !== 'home') {
        if (homeDataInterval) {
            clearInterval(homeDataInterval);
            homeDataInterval = null;
        }
        if (wsConnected) {
            unsubscribeFromPage('home');
            console.log('Unsubscribed from home page (navigating to', route, ')');
        }
    }
    if (route !== 'audio') {
        if (audioDataInterval) {
            clearInterval(audioDataInterval);
            audioDataInterval = null;
        }
        if (audioGraphInterval) {
            clearInterval(audioGraphInterval);
            audioGraphInterval = null;
        }
        if (wsConnected) {
            unsubscribeFromPage('audio');
            console.log('Unsubscribed from audio page (navigating to', route, ')');
        }
    }
    if (route !== 'network') {
        if (networkDataInterval) {
            clearInterval(networkDataInterval);
            networkDataInterval = null;
        }
        if (wsConnected) {
            unsubscribeFromPage('network');
            console.log('Unsubscribed from network page (navigating to', route, ')');
        }
    }
    if (route !== 'live-audio') {
        resumeLiveAudioSessionOverrides();
        if (wsConnected) {
            unsubscribeFromPage('live-audio');
            console.log('Unsubscribed from live-audio page (navigating to', route, ')');
        }
        // Stop live audio if playing
        if (typeof stopLiveAudio === 'function') {
            stopLiveAudio();
        }
    }
    if (route !== 'recordings') {
        if (typeof stopRecordingsPlayback === 'function') {
            stopRecordingsPlayback();
        }
        // Invalidate in-flight list loads so stale responses cannot clear the table after navigating away
        recordingsTableLoadGeneration++;
        if (recordingsClockInterval) {
            clearInterval(recordingsClockInterval);
            recordingsClockInterval = null;
        }
        const titleExtraEl = document.getElementById('page-title-extra');
        if (titleExtraEl) {
            titleExtraEl.style.display = 'none';
            titleExtraEl.innerHTML = '';
        }
    }
    // Render page content
    if (pageTemplates[route]) {
        contentBody.innerHTML = pageTemplates[route];
        
        // Initialize page-specific functionality
        if (route === 'home') {
            initHomePage();
        } else if (route === 'audio') {
            initAudioPage();
        } else if (route === 'live-audio') {
            initLiveAudioPage();
        } else if (route === 'player-tx') {
            initPlayerTxPage();
        } else if (route === 'cw') {
            initCwPage();
        } else if (route === 'recordings') {
            initRecordingsPage();
        } else if (route === 'network') {
            initNetworkPage();
        } else if (route === 'advanced') {
            initAdvancedPage();
        }

        // Attach help handlers for any help icons on this page
        initHelpIcons();
    } else {
        contentBody.innerHTML = '<div class="info-message">Page not found</div>';
        hideLoadingPopup();
    }
}

// Initialize on page load
// WebSocket connection and message handling
function connectWebSocket() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        return; // Already connected or connecting
    }
    
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.hostname}:81/ws`;
    
    try {
        ws = new WebSocket(wsUrl);
        ws.binaryType = 'arraybuffer';

        ws.onopen = function() {
            console.log('WebSocket connected');
            wsConnected = true;
            clearTimeout(wsReconnectTimer);
            hideConnectionError();
            // Clear any stale subscriptions first
            currentSubscriptions.clear();
            // Subscribe to current page if we're on a page that uses WebSocket
            if (currentRoute === 'home' || currentRoute === 'audio' || currentRoute === 'network' || currentRoute === 'live-audio') {
                subscribeToPage(currentRoute);
                console.log('Subscribed to', currentRoute, 'after WebSocket connection');
            }
            if (currentRoute === 'audio') {
                loadAudioData();
            }
        };
        
        ws.onmessage = function(event) {
            // Binary frames are exclusively used for live-audio right now.
            if (event.data instanceof ArrayBuffer) {
                handleLiveAudioBinaryFrame(event.data);
                return;
            }
            try {
                const data = JSON.parse(event.data);
                console.log('WebSocket message received, type:', data.type);
                
                // Route messages by type
                if (data.type === 'home') {
                    updateHomeDataFromWebSocket(data);
                } else if (data.type === 'audio') {
                    let _logDb = data.currentDb;
                    if (_logDb === undefined && data.samples && data.samples.length) {
                        _logDb = data.samples[data.samples.length - 1].currentDb;
                    }
                    console.log('Audio data received:', _logDb, 'dB');
                    updateAudioDataFromWebSocket(data);
                } else if (data.type === 'network') {
                    updateNetworkDataFromWebSocket(data);
                } else {
                    console.log('[WebSocket] Unknown message type:', data.type);
                }
            } catch (e) {
                console.error('Error parsing WebSocket message:', e, 'Raw:', event.data.substring(0, 100));
            }
        };
        
        ws.onerror = function(error) {
            console.error('WebSocket error:', error);
            wsConnected = false;
            showConnectionError();
        };
        
        ws.onclose = function() {
            console.log('WebSocket disconnected, attempting reconnect...');
            wsConnected = false;
            showConnectionError();
            currentSubscriptions.clear();
            clearTimeout(wsReconnectTimer);
            // Reconnect after 2 seconds
            wsReconnectTimer = setTimeout(connectWebSocket, 2000);
        };
    } catch (error) {
        console.error('Error creating WebSocket:', error);
        wsConnected = false;
        useWebSocket = false; // Fallback to polling
    }
}

function subscribeToPage(page) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        return;
    }
    
    if (!currentSubscriptions.has(page)) {
        currentSubscriptions.add(page);
        const message = JSON.stringify({ action: 'subscribe', page: page });
        ws.send(message);
        console.log('Subscribed to:', page);
    }
}

function unsubscribeFromPage(page) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        return;
    }
    
    if (currentSubscriptions.has(page)) {
        currentSubscriptions.delete(page);
        const message = JSON.stringify({ action: 'unsubscribe', page: page });
        ws.send(message);
        console.log('Unsubscribed from:', page);
    }
}

document.addEventListener('DOMContentLoaded', function() {
    isTabVisible = !document.hidden;
    loadDeviceInfo();
    // Keep device info (firmware/product/device-id) refreshed across all tabs.
    setInterval(loadDeviceInfo, 15000);
    updateTime();
    setInterval(updateTime, 1000);
    // Keep header clock synced even when not on Home/Recordings.
    setInterval(syncDeviceClockFromSummary, 5000);
    initMobileSidebar();
    initRouting();
    setupGlobalModal();
    
    // Setup Page Visibility API to pause/resume polling
    // Handle visibility change
    document.addEventListener('visibilitychange', function() {
        if (document.hidden) {
            // Tab is now hidden
            isTabVisible = false;
            pausePolling();
        } else {
            // Tab is now visible
            isTabVisible = true;
            resumePolling();
        }
    });
    
    // Also handle window focus/blur as fallback
    window.addEventListener('blur', function() {
        if (!document.hidden) return; // Only if visibility API didn't catch it
        isTabVisible = false;
        pausePolling();
    });
    
    window.addEventListener('focus', function() {
        if (document.hidden) return; // Only if visibility API didn't catch it
        isTabVisible = true;
        resumePolling();
    });
    
    // Setup refresh button
    const refreshBtn = document.getElementById('refresh-page-btn');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', function() {
            window.location.reload();
        });
    }
    
    // Handle initial route
    const route = getRoute();
    navigateTo(route);
    
    // Attach help handlers for initial content (route will reattach on each render)
    initHelpIcons();
    
    // Connect WebSocket if enabled
    if (useWebSocket) {
        connectWebSocket();
    }
});

// Global modal helpers
function setupGlobalModal() {
    const overlay = document.getElementById('global-modal-overlay');
    const confirmBtn = document.getElementById('global-modal-confirm');
    const cancelBtn = document.getElementById('global-modal-cancel');

    if (!overlay || !confirmBtn || !cancelBtn) {
        return;
    }

    const cleanup = (result) => {
        overlay.style.display = 'none';
        if (globalModalResolve) {
            const resolveFn = globalModalResolve;
            globalModalResolve = null;
            resolveFn(result);
        }
    };

    confirmBtn.addEventListener('click', () => cleanup(true));
    cancelBtn.addEventListener('click', () => cleanup(false));

    overlay.addEventListener('click', (e) => {
        if (e.target === overlay) {
            cleanup(false);
        }
    });
}

function initHelpIcons() {
    const icons = document.querySelectorAll('.help-icon, .help-icon-inline');
    if (!icons || icons.length === 0) return;

    icons.forEach(icon => {
        icon.addEventListener('click', async function(e) {
            e.preventDefault();
            e.stopPropagation();
            const key = this.getAttribute('data-help-key');
            const message = key && SETTINGS_HELP[key] ? SETTINGS_HELP[key] : 'No help is available for this setting.';
            let title = 'Help';
            if (key && key.startsWith('audio.')) {
                title = 'Audio Help';
            }
            await showHelpModal(message, title);
        });
    });
}

async function loadTimezoneFromServer() {
    const select = document.getElementById('timezone-select');
    if (!select) return;
    try {
        const response = await fetchWithRetry('/api/home/summary', {}, 8000, 2);
        if (response.ok) {
            const data = await response.json();
            if (data && typeof data.timezoneOffsetHours === 'number') {
                const offsetStr = String(data.timezoneOffsetHours);
                Array.from(select.options).forEach(opt => {
                    if (!opt.disabled && opt.value === offsetStr) {
                        select.value = offsetStr;
                    }
                });
            }
        }
    } catch (e) {
        console.error('Error loading timezone from summary:', e);
    }
}

async function initTimezoneControls() {
    const select = document.getElementById('timezone-select');
    if (!select) return;

    // Populate options: US first, then separator, then others
    select.innerHTML = '';
    const usZones = TIMEZONES.filter(tz => tz.isUS);
    const otherZones = TIMEZONES.filter(tz => !tz.isUS);

    usZones.forEach(tz => {
        const opt = document.createElement('option');
        opt.value = String(tz.offset);
        opt.textContent = tz.label;
        select.appendChild(opt);
    });

    if (otherZones.length > 0) {
        const sep = document.createElement('option');
        sep.disabled = true;
        sep.textContent = '──────────';
        select.appendChild(sep);
    }

    otherZones.forEach(tz => {
        const opt = document.createElement('option');
        opt.value = String(tz.offset);
        opt.textContent = tz.label;
        select.appendChild(opt);
    });

    await loadTimezoneFromServer();
}

async function saveTimezoneSettings(skipConfirm) {
    const select = document.getElementById('timezone-select');
    if (!select) return true;
    const offset = parseInt(select.value, 10);
    if (Number.isNaN(offset)) {
        if (!skipConfirm) await showInfoModal('Please select a valid timezone.', 'Timezone');
        return false;
    }
    if (!skipConfirm && !await showConfirmModal('Save timezone offset ' + offset + ' hours relative to UTC?', 'Timezone')) {
        return false;
    }
    try {
        const response = await fetchWithRetry('/api/audio/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({param: 'timezone.offsetHours', value: offset})
        }, 8000, 2);
        const data = await response.json();
        if (response.ok && data.success) {
            if (!skipConfirm) await showInfoModal('Timezone updated. Device will use the new offset for scheduling and timestamps.', 'Timezone');
            return true;
        }
        if (!skipConfirm) await showInfoModal('Failed to save timezone: ' + (data.message || 'Unknown error'), 'Timezone');
        return false;
    } catch (error) {
        console.error('Error saving timezone:', error);
        if (!skipConfirm) await showInfoModal('Error saving timezone setting.', 'Timezone');
        return false;
    }
}

function showGlobalModal(options) {
    const {
        title = 'Notice',
        message = '',
        showCancel = false,
        confirmText = 'OK',
        cancelText = 'Cancel',
        type = 'info'
    } = options || {};

    return new Promise((resolve) => {
        const overlay = document.getElementById('global-modal-overlay');
        const titleEl = document.getElementById('global-modal-title');
        const messageEl = document.getElementById('global-modal-message');
        const confirmBtn = document.getElementById('global-modal-confirm');
        const cancelBtn = document.getElementById('global-modal-cancel');
        const iconEl = document.getElementById('global-modal-icon');
        const modalEl = overlay ? overlay.querySelector('.global-modal') : null;

        if (!overlay || !titleEl || !messageEl || !confirmBtn || !cancelBtn) {
            // Fallback to native alert/confirm only if modal is unavailable
            if (showCancel) {
                const result = window.confirm(message || title);
                resolve(result);
            } else {
                window.alert(message || title);
                resolve(true);
            }
            return;
        }

        const iconMap = {
            info: 'ℹ️',
            help: '❓',
            confirm: '❗',
            warning: '⚠️'
        };

        titleEl.textContent = title;
        messageEl.textContent = message;
        confirmBtn.textContent = confirmText;
        cancelBtn.textContent = cancelText;
        cancelBtn.style.display = showCancel ? 'inline-block' : 'none';

        if (iconEl) {
            const iconKey = iconMap[type] ? type : 'info';
            iconEl.textContent = iconMap[iconKey];
        }

        if (modalEl) {
            modalEl.classList.remove('global-modal--info', 'global-modal--help', 'global-modal--confirm', 'global-modal--warning');
            const cls = ['info', 'help', 'confirm', 'warning'].includes(type) ? type : 'info';
            modalEl.classList.add('global-modal--' + cls);
        }

        globalModalResolve = resolve;
        overlay.style.display = 'flex';
    });
}

function showInfoModal(message, title = 'Notice') {
    let type = 'info';
    const t = (title || '').toLowerCase();
    if (t.includes('error') || t.includes('failed') || t.includes('warning')) {
        type = 'warning';
    }
    return showGlobalModal({
        title,
        message,
        showCancel: false,
        confirmText: 'OK',
        type
    });
}

function showConfirmModal(message, title = 'Confirm') {
    return showGlobalModal({
        title,
        message,
        showCancel: true,
        confirmText: 'Confirm',
        cancelText: 'Cancel',
        type: 'confirm'
    });
}

function showHelpModal(message, title = 'Help') {
    return showGlobalModal({
        title,
        message,
        showCancel: false,
        confirmText: 'Close',
        type: 'help'
    });
}

function initRouting() {
    // Handle navigation clicks
    document.querySelectorAll('.nav-item').forEach(item => {
        item.addEventListener('click', function(e) {
            e.preventDefault();
            const route = this.getAttribute('data-route');
            if (route) {
                navigateTo(route);
            }
        });
    });
    
    // Handle browser back/forward
    window.addEventListener('hashchange', function() {
        const route = getRoute();
        if (route !== currentRoute) {
            currentRoute = route;
            renderPage(route);
        }
    });
}

// Mobile sidebar toggle
function initMobileSidebar() {
    const sidebar = document.querySelector('.sidebar');
    const sidebarHeader = document.querySelector('.sidebar-header');
    
    if (sidebar && sidebarHeader && window.innerWidth <= 768) {
        sidebarHeader.addEventListener('click', function() {
            sidebar.classList.toggle('expanded');
        });
        
        // Close sidebar when clicking outside on mobile
        document.addEventListener('click', function(e) {
            if (window.innerWidth <= 768 && 
                sidebar.classList.contains('expanded') &&
                !sidebar.contains(e.target)) {
                sidebar.classList.remove('expanded');
            }
        });
        
        // Close sidebar when navigating on mobile
        document.querySelectorAll('.nav-item').forEach(item => {
            item.addEventListener('click', function() {
                if (window.innerWidth <= 768) {
                    setTimeout(() => {
                        sidebar.classList.remove('expanded');
                    }, 300);
                }
            });
        });
    }
    
    // Handle window resize
    let resizeTimer;
    window.addEventListener('resize', function() {
        clearTimeout(resizeTimer);
        resizeTimer = setTimeout(function() {
            if (window.innerWidth > 768) {
                const sidebar = document.querySelector('.sidebar');
                if (sidebar) sidebar.classList.remove('expanded');
            }
        }, 250);
    });
}

// Helper function to create fetch with timeout
// Request queue to prevent too many concurrent requests
let activeRequests = 0;
const maxConcurrentRequests = 2;
const requestQueue = [];

function processRequestQueue() {
    while (requestQueue.length > 0 && activeRequests < maxConcurrentRequests) {
        const { url, options, timeoutMs, resolve, reject } = requestQueue.shift();
        activeRequests++;
        
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), timeoutMs);
        
        fetch(url, {
            ...options,
            signal: controller.signal,
            cache: 'no-cache',
            keepalive: false
        })
        .then(resolve)
        .catch(reject)
        .finally(() => {
            clearTimeout(timeoutId);
            activeRequests--;
            processRequestQueue();
        });
    }
}

function fetchWithTimeout(url, options = {}, timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
        if (activeRequests >= maxConcurrentRequests) {
            requestQueue.push({ url, options, timeoutMs, resolve, reject });
            processRequestQueue();
        } else {
            activeRequests++;
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), timeoutMs);
            
            fetch(url, {
                ...options,
                signal: controller.signal,
                cache: 'no-cache',
                keepalive: false
            })
            .then(resolve)
            .catch(reject)
            .finally(() => {
                clearTimeout(timeoutId);
                activeRequests--;
                processRequestQueue();
            });
        }
    });
}

// Retry wrapper with exponential backoff
async function fetchWithRetry(url, options = {}, timeoutMs = 10000, maxRetries = 2) {
    let lastError;
    for (let attempt = 0; attempt < maxRetries; attempt++) {
        try {
            const response = await fetchWithTimeout(url, options, timeoutMs);
            if (response.ok) {
                return response;
            }
            // For non-OK responses, retry if it's a server error (5xx)
            if (response.status >= 500 && attempt < maxRetries - 1) {
                const delay = Math.min(500 * Math.pow(2, attempt), 2000);
                await new Promise(resolve => setTimeout(resolve, delay));
                continue;
            }
            return response; // Return non-OK response for client errors (4xx)
        } catch (error) {
            lastError = error;
            // AbortError means timeout - retry if we have attempts left
            if (error.name === 'AbortError') {
                if (attempt < maxRetries - 1) {
                    const delay = Math.min(500 * Math.pow(2, attempt), 2000);
                    await new Promise(resolve => setTimeout(resolve, delay));
                    continue;
                }
                // All retries exhausted, throw the error
                throw error;
            }
            // For other errors, only retry if we have attempts left
            if (attempt === maxRetries - 1) {
                throw error;
            }
            const delay = Math.min(500 * Math.pow(2, attempt), 2000);
            await new Promise(resolve => setTimeout(resolve, delay));
        }
    }
    throw lastError;
}

// Common functions
async function loadDeviceInfo() {
    try {
        const response = await fetchWithRetry('/api/device-info', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        deviceMac = data.mac || 'Unknown';
        const deviceIdEl = document.getElementById('device-id');
        if (deviceIdEl) {
            deviceIdEl.textContent = deviceMac;
        }
        if (document.getElementById('sidebar-firmware')) {
            document.getElementById('sidebar-firmware').textContent = 'Firmware : ' + (data.firmware || '--');
        }
        applyProductBranding(data.product);
        applyDeviceCapabilities(data);
        hideConnectionError(); // Connection successful
    } catch (error) {
        console.error('Error loading device info:', error);
        if (error.name === 'AbortError' || error.message.includes('Failed to fetch')) {
            showConnectionError();
        }
    }
}

// Device clock: Unix UTC epoch from API; header shows browser-local time + UTC subline (ignore device timezone offset for display)
let lastDeviceUtcSyncMs = 0;
let wallMsAtUtcSync = 0;

function applyHeaderClockFromUtcMs(utcMs) {
    const d = new Date(utcMs);
    const timeEl = document.getElementById('current-time');
    const subEl = document.getElementById('header-timezone');
    if (timeEl) {
        timeEl.textContent = d.toLocaleTimeString(undefined, { hour12: true, hour: 'numeric', minute: '2-digit', second: '2-digit' });
    }
    if (subEl) {
        const utcStr = d.toLocaleTimeString('en-US', { timeZone: 'UTC', hour12: true, hour: 'numeric', minute: '2-digit', second: '2-digit' });
        subEl.textContent = 'UTC (' + utcStr + ')';
    }
}

function updateTime() {
    if (lastDeviceUtcSyncMs <= 0 || wallMsAtUtcSync <= 0) {
        const timeEl = document.getElementById('current-time');
        const subEl = document.getElementById('header-timezone');
        if (timeEl) {
            timeEl.textContent = '--:--:-- --';
        }
        if (subEl) {
            subEl.textContent = '';
        }
        return;
    }
    const utcMs = lastDeviceUtcSyncMs + (Date.now() - wallMsAtUtcSync);
    applyHeaderClockFromUtcMs(utcMs);
}

function updateDeviceTimeFromApi(data) {
    if (data.deviceUtcEpoch === undefined || data.deviceUtcEpoch === null) {
        return;
    }
    const sec = Number(data.deviceUtcEpoch);
    if (Number.isNaN(sec) || sec <= 0) {
        return;
    }
    lastDeviceUtcSyncMs = sec * 1000;
    wallMsAtUtcSync = Date.now();
    applyHeaderClockFromUtcMs(lastDeviceUtcSyncMs);
}

async function syncDeviceClockFromSummary() {
    if (!isTabVisible || pollingPaused || connectionLost) {
        return;
    }
    try {
        const response = await fetchWithRetry('/api/home/summary', {}, 8000, 2);
        if (!response.ok) {
            return;
        }
        const data = await response.json();
        updateDeviceTimeFromApi(data);
    } catch (e) {
        /* ignore */
    }
}

// Home Page
function initHomePage() {
    if (connectionLost) {
        hideLoadingPopup();
        return;
    }
    
    // Subscribe to WebSocket if connected, otherwise use polling
    if (wsConnected && useWebSocket) {
        subscribeToPage('home');
        hideLoadingPopup();
        // Load initial data once
        loadHomeData();
    } else {
        loadHomeData().then(() => {
            hideLoadingPopup();
            if (!connectionLost && !homeDataInterval && isTabVisible && !useWebSocket) {
                homeDataInterval = setInterval(loadHomeData, 1000); // Poll every second
            }
        });
    }
    
    // Initialize firmware update check button
    const checkFirmwareBtn = document.getElementById('check-firmware-btn');
    if (checkFirmwareBtn) {
        checkFirmwareBtn.addEventListener('click', checkFirmwareUpdate);
    }

    // Auto-check firmware once per app load, 10s after summary is visible
    if (!firmwareAutoCheckDone && checkFirmwareBtn) {
        firmwareAutoCheckDone = true;
        setTimeout(() => {
            // Only call if still on home and button exists
            if (document.getElementById('check-firmware-btn') && currentRoute === 'home' && !connectionLost) {
                checkFirmwareUpdate();
            }
        }, 10000);
    }
    
    // Initialize apply firmware button
    const applyFirmwareBtn = document.getElementById('apply-firmware-btn');
    if (applyFirmwareBtn) {
        applyFirmwareBtn.addEventListener('click', applyFirmwareUpdate);
    }

    // Repeater controls (ECHO only; fields absent on other builds)
    const repEnabled = document.getElementById('repeater-enabled');
    const repSimplex = document.getElementById('repeater-mode-simplex');
    const repDuplex = document.getElementById('repeater-mode-duplex');
    if (repEnabled) {
        repEnabled.addEventListener('change', function() {
            if (this.disabled) {
                this.checked = false;
                return;
            }
            const enabledNow = !!this.checked;
            if (repSimplex) repSimplex.disabled = !enabledNow;
            if (repDuplex) repDuplex.disabled = !enabledNow;
            savePlayerTxParam('repeater.enabled', enabledNow ? 'true' : 'false');
            // If enabling and no mode selected yet, default to simplex.
            if (enabledNow && repSimplex && repDuplex && !repSimplex.checked && !repDuplex.checked) {
                repSimplex.checked = true;
                savePlayerTxParam('repeater.mode', '1');
            }
        });
    }
    if (repSimplex) {
        repSimplex.addEventListener('change', function() {
            if (this.checked) {
                savePlayerTxParam('repeater.mode', '1');
            }
        });
    }
    if (repDuplex) {
        repDuplex.addEventListener('change', function() {
            if (this.checked) {
                savePlayerTxParam('repeater.mode', '2');
            }
        });
    }
}

// WebSocket update handler for home data
function updateHomeDataFromWebSocket(data) {
    hideConnectionError();
    
    // Only update UI if data changed
    if (data.changed === false) {
        // Still update time even if other data hasn't changed
        updateDeviceTimeFromApi(data);
        return; // Nothing changed, skip UI update
    }
    
    updateDeviceTimeFromApi(data);
    
    // Update all UI elements (same as loadHomeData)
    if (document.getElementById('device-id-value')) {
        document.getElementById('device-id-value').textContent = data.deviceId || '--';
    }
    if (document.getElementById('firmware-version')) {
        document.getElementById('firmware-version').textContent = data.firmware || '--';
    }
    if (document.getElementById('sidebar-firmware')) {
        document.getElementById('sidebar-firmware').textContent = 'Firmware : ' + (data.firmware || '--');
    }
    applyProductBranding(data.product);
    applyDeviceCapabilities(data);
    if (document.getElementById('ip-address')) {
        document.getElementById('ip-address').textContent = data.ipAddress || '--';
    }
    if (document.getElementById('wifi-ssid')) {
        document.getElementById('wifi-ssid').textContent = data.wifiSsid || '--';
    }
    if (document.getElementById('wifi-rssi')) {
        document.getElementById('wifi-rssi').textContent = formatWifiRssi(data.wifiRssi);
    }
    if (document.getElementById('recording-status')) {
        const el = document.getElementById('recording-status');
        el.textContent = data.recordingStatus || '--';
        if (data.recordingStatus === 'Recording') {
            el.classList.add('status-flash');
        } else {
            el.classList.remove('status-flash');
        }
    }
    if (document.getElementById('uploading-status')) {
        const el = document.getElementById('uploading-status');
        el.textContent = data.uploadingStatus || '--';
        if (data.uploadingStatus === 'Uploading') {
            el.classList.add('status-flash');
        } else {
            el.classList.remove('status-flash');
        }
    }
    if (document.getElementById('recording-count')) {
        document.getElementById('recording-count').textContent = data.recordingCount || '--';
    }
    if (document.getElementById('uploaded-count')) {
        const uploadedCount = data.uploadedCount !== undefined ? data.uploadedCount : '--';
        const queueCount = data.uploadQueue !== undefined ? data.uploadQueue : 0;
        if (uploadedCount === '--') {
            document.getElementById('uploaded-count').textContent = '--';
        } else {
            document.getElementById('uploaded-count').textContent = `${uploadedCount} (${queueCount})`;
        }
    }
    if (document.getElementById('error-count')) {
        document.getElementById('error-count').textContent = data.errorCount || '--';
    }
    if (document.getElementById('storage-mode')) {
        document.getElementById('storage-mode').textContent = data.storageMode || '--';
    }
    if (document.getElementById('storage-total')) {
        if (data.storageTotalGB && data.storageTotalGB !== 'N/A') {
            document.getElementById('storage-total').textContent = data.storageTotalGB + ' GB';
        } else {
            document.getElementById('storage-total').textContent = '--';
        }
    }
    if (document.getElementById('storage-usage')) {
        if (data.storageUsagePercent && data.storageUsagePercent !== 'N/A') {
            document.getElementById('storage-usage').textContent = data.storageUsagePercent + '%';
        } else {
            document.getElementById('storage-usage').textContent = '--';
        }
    }
    if (document.getElementById('sd-enabled')) {
        if (typeof data.sdCardEnabled === 'boolean') {
            document.getElementById('sd-enabled').textContent = data.sdCardEnabled ? 'Enabled' : 'Disabled';
        } else {
            document.getElementById('sd-enabled').textContent = '--';
        }
    }
    if (document.getElementById('sd-mode-speed')) {
        const el = document.getElementById('sd-mode-speed');
        if (typeof data.sdCardEnabled === 'boolean' && !data.sdCardEnabled) {
            el.textContent = 'N/A';
        } else if (data.sdCardMode && data.sdCardFrequencyHz) {
            const mhz = (data.sdCardFrequencyHz / 1000000).toFixed(1).replace(/\.0$/, '');
            el.textContent = data.sdCardMode + ' @ ' + mhz + ' MHz';
        } else {
            el.textContent = '--';
        }
    }
    if (document.getElementById('heap-free')) {
        if (data.heapUsedPercent && data.heapUsedPercent !== 'N/A') {
            document.getElementById('heap-free').textContent = data.heapUsedPercent + '% used';
        } else if (data.heapFreePercent && data.heapFreePercent !== 'N/A') {
            const usedPct = (100.0 - parseFloat(data.heapFreePercent)).toFixed(1);
            document.getElementById('heap-free').textContent = usedPct + '% used';
        } else if (data.heapFree) {
            document.getElementById('heap-free').textContent = data.heapFree;
        } else {
            document.getElementById('heap-free').textContent = '--';
        }
    }
    if (document.getElementById('psram-usage')) {
        document.getElementById('psram-usage').textContent = data.psramUsagePercent && data.psramUsagePercent !== 'N/A'
            ? data.psramUsagePercent + '% used'
            : 'N/A';
    }
    if (document.getElementById('boot-reason')) {
        document.getElementById('boot-reason').textContent = data.bootReason || '--';
    }
    if (document.getElementById('maintenance-time')) {
        if (typeof data.maintenanceHour === 'number' && typeof data.maintenanceMinute === 'number') {
            const h = data.maintenanceHour;
            const m = data.maintenanceMinute;
            const date = new Date();
            date.setHours(h);
            date.setMinutes(m);
            const timeStr = date.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', hour12: true });
            document.getElementById('maintenance-time').textContent = timeStr;
        } else {
            document.getElementById('maintenance-time').textContent = '--';
        }
    }
    if (document.getElementById('uptime')) {
        document.getElementById('uptime').textContent = data.uptime || '--';
    }

    // Repeater controls (ECHO only; fields absent on other builds)
    const repCard = document.getElementById('repeater-card');
    const repEnabled = document.getElementById('repeater-enabled');
    const repSimplex = document.getElementById('repeater-mode-simplex');
    const repDuplex = document.getElementById('repeater-mode-duplex');
    const repStatus = document.getElementById('repeater-status');
    if (repCard && typeof data.repeaterEnabled !== 'boolean') {
        repCard.style.display = 'none';
    } else if (repCard) {
        repCard.style.display = '';
    }
    if (repEnabled && typeof data.repeaterEnabled === 'boolean') {
        const txOk = (typeof data.transmitEnabled === 'boolean') ? !!data.transmitEnabled : true;
        repEnabled.checked = !!data.repeaterEnabled;
        const enabledNow = !!data.repeaterEnabled && txOk;
        repEnabled.disabled = !txOk;
        if (repSimplex) repSimplex.disabled = !enabledNow;
        if (repDuplex) repDuplex.disabled = !enabledNow;
        const mode = (data.repeaterMode !== undefined) ? Number(data.repeaterMode) : 1;
        if (repSimplex) repSimplex.checked = (mode === 1);
        if (repDuplex) repDuplex.checked = (mode === 2);
        if (repStatus) repStatus.textContent = !txOk ? 'Enable TX to use repeater' : (enabledNow ? ((mode === 2) ? 'Duplex' : 'Simplex') : 'Disabled');
    }
}

async function loadHomeData() {
    // Don't poll if tab is not visible
    if (!isTabVisible || pollingPaused) {
        return;
    }
    
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/home/summary', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError(); // Connection successful
        
        // Only update UI if data changed
        if (data.changed === false) {
            // Still update time even if other data hasn't changed
            updateDeviceTimeFromApi(data);
            return; // Nothing changed, skip UI update
        }
        
        updateDeviceTimeFromApi(data);
        
        if (document.getElementById('device-id-value')) {
            document.getElementById('device-id-value').textContent = data.deviceId || '--';
        }
        if (document.getElementById('firmware-version')) {
            document.getElementById('firmware-version').textContent = data.firmware || '--';
        }
        if (document.getElementById('sidebar-firmware')) {
            document.getElementById('sidebar-firmware').textContent = 'Firmware : ' + (data.firmware || '--');
        }
        applyProductBranding(data.product);
        applyDeviceCapabilities(data);
        if (document.getElementById('ip-address')) {
            document.getElementById('ip-address').textContent = data.ipAddress || '--';
        }
        if (document.getElementById('wifi-ssid')) {
            document.getElementById('wifi-ssid').textContent = data.wifiSsid || '--';
        }
        if (document.getElementById('wifi-rssi')) {
            document.getElementById('wifi-rssi').textContent = formatWifiRssi(data.wifiRssi);
        }
        if (document.getElementById('recording-status')) {
            const el = document.getElementById('recording-status');
            el.textContent = data.recordingStatus || '--';
            if (data.recordingStatus === 'Recording') {
                el.classList.add('status-flash');
            } else {
                el.classList.remove('status-flash');
            }
        }
        if (document.getElementById('uploading-status')) {
            const el = document.getElementById('uploading-status');
            el.textContent = data.uploadingStatus || '--';
            if (data.uploadingStatus === 'Uploading') {
                el.classList.add('status-flash');
            } else {
                el.classList.remove('status-flash');
            }
        }
        if (document.getElementById('recording-count')) {
            document.getElementById('recording-count').textContent = data.recordingCount || '--';
        }
        if (document.getElementById('uploaded-count')) {
            const uploadedCount = data.uploadedCount !== undefined ? data.uploadedCount : '--';
            const queueCount = data.uploadQueue !== undefined ? data.uploadQueue : 0;
            if (uploadedCount === '--') {
                document.getElementById('uploaded-count').textContent = '--';
            } else {
                document.getElementById('uploaded-count').textContent = `${uploadedCount} (${queueCount})`;
            }
        }
        if (document.getElementById('error-count')) {
            document.getElementById('error-count').textContent = data.errorCount || '--';
        }
        if (document.getElementById('storage-mode')) {
            document.getElementById('storage-mode').textContent = data.storageMode || '--';
        }
        if (document.getElementById('storage-total')) {
            if (data.storageTotalGB && data.storageTotalGB !== 'N/A') {
                document.getElementById('storage-total').textContent = data.storageTotalGB + ' GB';
            } else {
                document.getElementById('storage-total').textContent = '--';
            }
        }
        if (document.getElementById('storage-usage')) {
            if (data.storageUsagePercent && data.storageUsagePercent !== 'N/A') {
                document.getElementById('storage-usage').textContent = data.storageUsagePercent + '%';
            } else {
                document.getElementById('storage-usage').textContent = '--';
            }
        }
        if (document.getElementById('sd-enabled')) {
            if (typeof data.sdCardEnabled === 'boolean') {
                document.getElementById('sd-enabled').textContent = data.sdCardEnabled ? 'Enabled' : 'Disabled';
            } else {
                document.getElementById('sd-enabled').textContent = '--';
            }
        }
        if (document.getElementById('sd-mode-speed')) {
            const el = document.getElementById('sd-mode-speed');
            if (typeof data.sdCardEnabled === 'boolean' && !data.sdCardEnabled) {
                el.textContent = 'N/A';
            } else if (data.sdCardMode && data.sdCardFrequencyHz) {
                const mhz = (data.sdCardFrequencyHz / 1000000).toFixed(1).replace(/\.0$/, '');
                el.textContent = data.sdCardMode + ' @ ' + mhz + ' MHz';
            } else {
                el.textContent = '--';
            }
        }
        if (document.getElementById('heap-free')) {
            if (data.heapUsedPercent && data.heapUsedPercent !== 'N/A') {
                document.getElementById('heap-free').textContent = data.heapUsedPercent + '% used';
            } else if (data.heapFreePercent && data.heapFreePercent !== 'N/A') {
                // Fallback: calculate used from free if used not available
                const usedPct = (100.0 - parseFloat(data.heapFreePercent)).toFixed(1);
                document.getElementById('heap-free').textContent = usedPct + '% used';
            } else if (data.heapFree) {
                document.getElementById('heap-free').textContent = data.heapFree;
            } else {
                document.getElementById('heap-free').textContent = '--';
            }
        }
        if (document.getElementById('psram-usage')) {
            document.getElementById('psram-usage').textContent = data.psramUsagePercent && data.psramUsagePercent !== 'N/A'
                ? data.psramUsagePercent + '% used'
                : 'N/A';
        }
        if (document.getElementById('boot-reason')) {
            document.getElementById('boot-reason').textContent = data.bootReason || '--';
      }
      if (document.getElementById('maintenance-time')) {
          if (typeof data.maintenanceHour === 'number' && typeof data.maintenanceMinute === 'number') {
              const h = data.maintenanceHour;
              const m = data.maintenanceMinute;
              const date = new Date();
              date.setHours(h);
              date.setMinutes(m);
              const timeStr = date.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', hour12: true });
              document.getElementById('maintenance-time').textContent = timeStr;
          } else {
              document.getElementById('maintenance-time').textContent = '--';
          }
        }
        if (document.getElementById('uptime')) {
            document.getElementById('uptime').textContent = data.uptime || '--';
        }

        // Repeater controls (ECHO only; fields absent on other builds)
        const repCard = document.getElementById('repeater-card');
        const repEnabled = document.getElementById('repeater-enabled');
        const repSimplex = document.getElementById('repeater-mode-simplex');
        const repDuplex = document.getElementById('repeater-mode-duplex');
        const repStatus = document.getElementById('repeater-status');
        if (repCard && typeof data.repeaterEnabled !== 'boolean') {
            repCard.style.display = 'none';
        } else if (repCard) {
            repCard.style.display = '';
        }
        if (repEnabled && typeof data.repeaterEnabled === 'boolean') {
            const txOk = (typeof data.transmitEnabled === 'boolean') ? !!data.transmitEnabled : true;
            repEnabled.checked = !!data.repeaterEnabled;
            const enabledNow = !!data.repeaterEnabled && txOk;
            repEnabled.disabled = !txOk;
            if (repSimplex) repSimplex.disabled = !enabledNow;
            if (repDuplex) repDuplex.disabled = !enabledNow;
            const mode = (data.repeaterMode !== undefined) ? Number(data.repeaterMode) : 1;
            if (repSimplex) repSimplex.checked = (mode === 1);
            if (repDuplex) repDuplex.checked = (mode === 2);
            if (repStatus) repStatus.textContent = !txOk ? 'Enable TX to use repeater' : (enabledNow ? ((mode === 2) ? 'Duplex' : 'Simplex') : 'Disabled');
        }
    } catch (error) {
        console.error('Error loading home data:', error);
        // Don't show connection error for AbortError (timeout) - these are expected
        // Only show connection error for actual network failures
        if (error.name === 'AbortError') {
            // Timeout occurred, but don't treat as connection lost
            // The retry logic should handle this
            hideLoadingPopup();
            return;
        }
        if (error.message && error.message.includes('Failed to fetch')) {
            // Only show connection error after retries have been exhausted
            showConnectionError();
        }
        hideLoadingPopup();
    }
}

async function checkFirmwareUpdate() {
    const checkBtn = document.getElementById('check-firmware-btn');
    const resultDiv = document.getElementById('firmware-update-result');
    const messageDiv = document.getElementById('firmware-update-message');
    const actionsDiv = document.getElementById('firmware-update-actions');
    const downloadLink = document.getElementById('firmware-download-link');
    
    if (!checkBtn || !resultDiv || !messageDiv || !actionsDiv) return;
    
    checkBtn.disabled = true;
    checkBtn.textContent = '\u23F3 Checking...';
    resultDiv.style.display = 'none';
    
    try {
        const response = await fetchWithRetry('/api/firmware/check', {}, 15000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        resultDiv.style.display = 'block';
        
        if (data.success === false) {
            messageDiv.innerHTML = '<strong>Error:</strong> ' + (data.message || 'Failed to check for updates');
            messageDiv.style.color = '#d32f2f';
            actionsDiv.style.display = 'none';
        } else if (data.upgrade_available === true && data.download_link) {
            messageDiv.innerHTML = '<strong>Update Available!</strong><br>' + (data.message || 'A new firmware version is available.');
            messageDiv.style.color = '#2e7d32';
            actionsDiv.style.display = 'block';
            downloadLink.href = data.download_link;
            downloadLink.textContent = '\uD83D\uDCE5 Download Firmware';
            downloadLink.setAttribute('data-download-link', data.download_link);
        } else {
            messageDiv.innerHTML = '<strong>Up to Date</strong><br>' + (data.message || 'Your device is running the latest firmware version.');
            messageDiv.style.color = '#1976d2';
            actionsDiv.style.display = 'none';
        }
    } catch (error) {
        console.error('Error checking firmware update:', error);
        resultDiv.style.display = 'block';
        messageDiv.innerHTML = '<strong>Error:</strong> Failed to check for updates. Please try again.';
        messageDiv.style.color = '#d32f2f';
        actionsDiv.style.display = 'none';
    } finally {
        checkBtn.disabled = false;
        checkBtn.textContent = '\uD83D\uDD0D Check for Updates';
    }
}

async function applyFirmwareUpdate() {
    const downloadLink = document.getElementById('firmware-download-link');
    if (!downloadLink) return;
    
    const link = downloadLink.getAttribute('data-download-link');
    if (!link) {
        await showInfoModal('No download link available', 'Firmware Update');
        return;
    }
    
    if (!await showConfirmModal('Apply firmware update now? The device will download and install the firmware, then reboot. This may take a few minutes.', 'Apply Firmware Update')) {
        return;
    }
    
    const applyBtn = document.getElementById('apply-firmware-btn');
    if (applyBtn) {
        applyBtn.disabled = true;
        applyBtn.textContent = '\u23F3 Applying...';
    }
    
    try {
        const formData = new URLSearchParams();
        formData.append('download_link', link);
        
        const response = await fetchWithRetry('/api/firmware/apply', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded'
            },
            body: formData.toString()
        }, 30000); // 30 second timeout for apply
        
        const data = await response.json();
        
        if (jsonApiOk(data)) {
            await showInfoModal('Firmware update started. Device will reboot shortly.', 'Firmware Update');
            showRebootCountdown();
        } else {
            await showInfoModal('Failed to apply update: ' + (data.message || 'Unknown error'), 'Firmware Update Failed');
            if (applyBtn) {
                applyBtn.disabled = false;
                applyBtn.textContent = '\u2705 Apply Update';
            }
        }
    } catch (error) {
        console.error('Error applying firmware update:', error);
        await showInfoModal('Error applying firmware update. Please try again.', 'Firmware Update Error');
        if (applyBtn) {
            applyBtn.disabled = false;
            applyBtn.textContent = '\u2705 Apply Update';
        }
    }
}

// Audio Page
function initAudioPage() {
    if (connectionLost) {
        hideLoadingPopup();
        return;
    }
    
    // Always initialize graph first (it draws every 100ms regardless of data source)
    if (isTabVisible) {
        initAudioGraph();
    }
    
    // Subscribe to WebSocket if connected, otherwise use polling
    if (wsConnected && useWebSocket) {
        subscribeToPage('audio');
        hideLoadingPopup();
        // Load initial data once
        loadAudioData();
    } else {
        loadAudioData().then(() => {
            hideLoadingPopup();
            if (!connectionLost && !audioDataInterval && isTabVisible && !useWebSocket) {
                audioDataInterval = setInterval(loadAudioData, 500);
            }
        });
    }
    loadAudioSettings();
    initAudioControls();
}

async function applyRecordInputChannelFromSettingsData(data) {
    if (!RECORD_INPUT_CHANNEL_SUPPORTED || !data || data.recordInputChannel === undefined) return;
    const ch = parseInt(data.recordInputChannel, 10);
    const left = document.getElementById('record-input-left');
    const right = document.getElementById('record-input-right');
    if (left && right) {
        if (ch === 0) {
            right.checked = true;
            left.checked = false;
        } else {
            right.checked = false;
            left.checked = true;
        }
    }
    await applyMonitorInputChannel(ch);
}

// Live Audio page initialization
function initLiveAudioPage() {
    if (connectionLost) {
        hideLoadingPopup();
        return;
    }
    
    hideLoadingPopup();
    
    console.log('[LiveAudio] Initializing live audio page...');
    
    // Initialize live audio controls
    const startBtn = document.getElementById('start-live-audio-btn');
    const stopBtn = document.getElementById('stop-live-audio-btn');
    
    console.log('[LiveAudio] Buttons found:', { startBtn: !!startBtn, stopBtn: !!stopBtn });
    
    if (startBtn) {
        // Remove any existing listeners first
        const newStartBtn = startBtn.cloneNode(true);
        startBtn.parentNode.replaceChild(newStartBtn, startBtn);
        newStartBtn.addEventListener('click', function(e) {
            console.log('[LiveAudio] Start button clicked!');
            e.preventDefault();
            startLiveAudio();
        });
        console.log('[LiveAudio] Start button handler attached');
    } else {
        console.error('[LiveAudio] Start button not found!');
    }
    
    if (stopBtn) {
        // Remove any existing listeners first
        const newStopBtn = stopBtn.cloneNode(true);
        stopBtn.parentNode.replaceChild(newStopBtn, stopBtn);
        newStopBtn.addEventListener('click', function(e) {
            console.log('[LiveAudio] Stop button clicked!');
            e.preventDefault();
            stopLiveAudio();
        });
        console.log('[LiveAudio] Stop button handler attached');
    } else {
        console.error('[LiveAudio] Stop button not found!');
    }
    
    // Subscribe to WebSocket for live audio
    if (wsConnected && useWebSocket) {
        subscribeToPage('live-audio');
        console.log('[LiveAudio] Subscribed to WebSocket');
    } else {
        console.warn('[LiveAudio] WebSocket not connected, will subscribe when connected');
    }
    
    // Initialize visualizer
    initLiveAudioVisualizer();

    const pauseRec = document.getElementById('live-audio-pause-recording');
    if (pauseRec) {
        pauseRec.addEventListener('change', function() {
            postLiveAudioSessionUpdate({ pauseRecording: !!this.checked });
        });
    }
    const pauseUp = document.getElementById('live-audio-pause-uploads');
    if (pauseUp) {
        pauseUp.addEventListener('change', function() {
            postLiveAudioSessionUpdate({ pauseUploads: !!this.checked });
        });
    }

    loadLiveAudioSessionState();

    console.log('[LiveAudio] Page initialization complete');
}

function recordingsInboxDepth(path) {
    const m = path.match(/^\/recordings(?:\/(.*))?$/);
    if (!m || !m[1]) return 0;
    return m[1].split('/').filter(Boolean).length;
}

var RECORDINGS_MONTH_SHORT = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];

function recordingsMonthSegmentToShortName(segment) {
    if (!segment || typeof segment !== 'string') return segment;
    const s = segment.trim();
    if (!/^(0[1-9]|1[0-2])$/.test(s)) return segment;
    return RECORDINGS_MONTH_SHORT[parseInt(s, 10) - 1];
}

// Last path segment shown in the tree: numeric month folders (under YYYY) become JAN, FEB, ...
function recordingsTreeDisplayName(path) {
    if (path === '/recordings') return 'Recordings';
    const parts = path.split('/').filter(Boolean);
    const seg = parts[parts.length - 1];
    if (parts.length === 3 && parts[0] === 'recordings') {
        return recordingsMonthSegmentToShortName(seg);
    }
    return seg;
}

// e.g. /recordings/2026/03/23 -> /recordings/2026/MAR/23 (display only; API paths stay numeric)
function recordingsFormatPathForDisplay(fullPath) {
    if (!fullPath || typeof fullPath !== 'string') return fullPath;
    const parts = fullPath.split('/').filter(Boolean);
    if (parts.length < 3 || parts[0] !== 'recordings') return fullPath;
    const out = parts.slice();
    out[2] = recordingsMonthSegmentToShortName(out[2]);
    return '/' + out.join('/');
}

function stopRecordingsPlayback() {
    const a = document.getElementById('recordings-player');
    if (!a) return;
    a.pause();
    a.removeAttribute('src');
    try { a.load(); } catch (e) {}
}

async function loadRecordingsFoldersForPath(path) {
    const url = '/api/recordings/folders?path=' + encodeURIComponent(path);
    const response = await fetchWithRetry(url, {}, 12000, 2);
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
        throw new Error(data.message || 'Could not list folders');
    }
    if (data.status !== 'ok') {
        throw new Error(data.message || 'Could not list folders');
    }
    recordingsChildren[path] = data.folders || [];
}

function recordingsResolveFolderSegment(folderNames, want) {
    if (!folderNames || want === undefined || want === null) {
        return null;
    }
    const w = String(want).trim();
    for (let i = 0; i < folderNames.length; i++) {
        const a = String(folderNames[i]).trim();
        if (a === w) {
            return a;
        }
    }
    for (let i = 0; i < folderNames.length; i++) {
        const a = String(folderNames[i]).trim();
        if (/^\d+$/.test(a) && /^\d+$/.test(w) && parseInt(a, 10) === parseInt(w, 10)) {
            return a;
        }
    }
    return null;
}

async function focusRecordingsTodayUtc() {
    const y = String(new Date().getUTCFullYear());
    const mo = String(new Date().getUTCMonth() + 1).padStart(2, '0');
    const da = String(new Date().getUTCDate()).padStart(2, '0');
    try {
        if (!recordingsChildren['/recordings']) {
            await loadRecordingsFoldersForPath('/recordings');
        }
        const ySeg = recordingsResolveFolderSegment(recordingsChildren['/recordings'], y);
        if (!ySeg) {
            return;
        }
        const yearPath = '/recordings/' + ySeg;
        recordingsTreeExpanded[yearPath] = true;
        if (!recordingsChildren[yearPath]) {
            await loadRecordingsFoldersForPath(yearPath);
        }
        const moSeg = recordingsResolveFolderSegment(recordingsChildren[yearPath], mo);
        if (!moSeg) {
            renderRecordingsTree();
            return;
        }
        const monthPath = yearPath + '/' + moSeg;
        recordingsTreeExpanded[monthPath] = true;
        if (!recordingsChildren[monthPath]) {
            await loadRecordingsFoldersForPath(monthPath);
        }
        const daSeg = recordingsResolveFolderSegment(recordingsChildren[monthPath], da);
        if (!daSeg) {
            renderRecordingsTree();
            return;
        }
        const dayPath = monthPath + '/' + daSeg;
        recordingsSelectedDayPath = dayPath;
        recordingsPageNum = 1;
        renderRecordingsTree();
        requestAnimationFrame(function() {
            const el = document.querySelector('.recordings-tree-label.active');
            if (el && el.scrollIntoView) {
                el.scrollIntoView({ block: 'nearest', behavior: 'auto' });
            }
        });
        await loadRecordingsTable();
    } catch (e) {
        console.warn('[Recordings] focus today:', e);
    }
}

function renderRecordingsTree() {
    const root = document.getElementById('recordings-tree-root');
    if (!root) return;
    root.innerHTML = '';
    const folderEmoji = '\uD83D\uDCC1 '; // 📁
    function walk(path, depth) {
        const row = document.createElement('div');
        row.className = 'recordings-tree-row';
        row.style.paddingLeft = (depth * 14) + 'px';
        const d = recordingsInboxDepth(path);
        const shortName = recordingsTreeDisplayName(path);
        const labelText = folderEmoji + shortName;
        if (d < 3) {
            const toggle = document.createElement('button');
            toggle.type = 'button';
            toggle.className = 'recordings-tree-toggle';
            toggle.textContent = recordingsTreeExpanded[path] ? '\u2212' : '+';
            toggle.onclick = function(e) {
                e.preventDefault();
                toggleRecordingsExpand(path);
            };
            row.appendChild(toggle);
        } else {
            const sp = document.createElement('span');
            sp.className = 'recordings-tree-spacer';
            sp.textContent = '\u00a0';
            row.appendChild(sp);
        }
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'recordings-tree-label' + (recordingsSelectedDayPath === path ? ' active' : '');
        btn.textContent = labelText;
        btn.onclick = function() { onRecordingsTreeSelect(path); };
        row.appendChild(btn);
        if (recordingsTreeLoadingPath === path) {
            const spin = document.createElement('span');
            spin.className = 'recordings-spinner recordings-tree-row-spinner';
            spin.title = 'Loading folders…';
            row.appendChild(spin);
        }
        root.appendChild(row);
        if (d < 3 && recordingsTreeExpanded[path] && recordingsChildren[path]) {
            recordingsChildren[path].forEach(function(child) {
                const seg = String(child).trim();
                if (!seg || seg === '.' || seg === '..') {
                    return;
                }
                const childPath = path === '/recordings' ? '/recordings/' + seg : path + '/' + seg;
                walk(childPath, depth + 1);
            });
        }
    }
    const top = recordingsChildren['/recordings'];
    if (!top || top.length === 0) {
        if (recordingsTreeLoadingPath === '/recordings') {
            const row = document.createElement('div');
            row.className = 'recordings-tree-row';
            row.style.paddingLeft = '0';
            const sp = document.createElement('span');
            sp.className = 'recordings-tree-muted';
            sp.textContent = 'Loading folders…';
            row.appendChild(sp);
            const spin = document.createElement('span');
            spin.className = 'recordings-spinner recordings-tree-row-spinner';
            spin.title = 'Loading';
            row.appendChild(spin);
            root.appendChild(row);
        }
        return;
    }
    top.forEach(function(seg) {
        const s = String(seg).trim();
        if (!s || s === '.' || s === '..') {
            return;
        }
        walk('/recordings/' + s, 0);
    });
}

async function toggleRecordingsExpand(path) {
    const wasExpanded = !!recordingsTreeExpanded[path];
    if (wasExpanded) {
        recordingsTreeExpanded[path] = false;
        if (recordingsTreeLoadingPath === path) {
            recordingsTreeLoadingPath = '';
        }
        renderRecordingsTree();
        return;
    }
    if (recordingsChildren[path]) {
        recordingsTreeExpanded[path] = true;
        renderRecordingsTree();
        return;
    }
    recordingsTreeLoadingPath = path;
    renderRecordingsTree();
    try {
        await loadRecordingsFoldersForPath(path);
        recordingsTreeExpanded[path] = true;
        const err = document.getElementById('recordings-tree-error');
        if (err) { err.style.display = 'none'; err.textContent = ''; }
    } catch (e) {
        recordingsTreeExpanded[path] = false;
        delete recordingsChildren[path];
        const err = document.getElementById('recordings-tree-error');
        if (err) {
            err.style.display = 'block';
            err.textContent = e.message || String(e);
        }
    } finally {
        recordingsTreeLoadingPath = '';
    }
    renderRecordingsTree();
}

async function onRecordingsTreeSelect(path) {
    const d = recordingsInboxDepth(path);
    if (d < 3) {
        await toggleRecordingsExpand(path);
        return;
    }
    recordingsSelectedDayPath = path;
    recordingsPageNum = 1;
    renderRecordingsTree();
    await loadRecordingsTable();
}

function formatRecordingsBytes(n) {
    if (n === undefined || n === null || Number.isNaN(n)) return '--';
    const v = Number(n);
    if (v < 1024) return v + ' B';
    if (v < 1024 * 1024) return (v / 1024).toFixed(1) + ' KB';
    return (v / (1024 * 1024)).toFixed(2) + ' MB';
}

// Build UTC ISO from catalog filename YYYY-MM-DD-HH-mm-ss.wav (same convention as firmware).
function parseUtcIsoFromWavFilename(name) {
    if (!name || typeof name !== 'string') return '';
    const m = name.match(/^(\d{4}-\d{2}-\d{2})-(\d{2})-(\d{2})-(\d{2})\.wav$/i);
    if (!m) return '';
    return m[1] + 'T' + m[2] + ':' + m[3] + ':' + m[4] + 'Z';
}

// Parse API recordedAtUtc (ISO 8601 UTC) or fallback filename; display in the browser's local timezone.
function formatRecordingTimeFromUtcIso(isoUtc) {
    if (!isoUtc || typeof isoUtc !== 'string') return '\u2014';
    let s = isoUtc.trim();
    if (s.length === 19 && s.charAt(10) === 'T' && !/Z$/i.test(s)) {
        s += 'Z';
    }
    const ms = Date.parse(s);
    if (Number.isNaN(ms)) return '\u2014';
    try {
        return new Date(ms).toLocaleString(undefined, { dateStyle: 'short', timeStyle: 'medium' });
    } catch (e) {
        return '\u2014';
    }
}

function formatRecordingDisplayTime(item) {
    var iso = '';
    if (item && item.recordedAtUtc != null && item.recordedAtUtc !== '') {
        iso = String(item.recordedAtUtc).trim();
    }
    if (!iso) {
        iso = parseUtcIsoFromWavFilename(item && item.name ? item.name : '');
    }
    return formatRecordingTimeFromUtcIso(iso);
}

function recordingSortKeyMs(item) {
    var iso = item && item.recordedAtUtc;
    if (iso && typeof iso === 'string' && iso.length) {
        var ms = Date.parse(iso);
        if (!Number.isNaN(ms)) {
            return ms;
        }
    }
    return 0;
}

function normalizeSummaryLineObject(obj) {
    var playPath = '';
    if (obj && obj.path != null && obj.path !== '') {
        playPath = String(obj.path);
    } else if (obj && obj.inboxPath != null && obj.inboxPath !== '') {
        playPath = String(obj.inboxPath);
    }
    var slash = playPath.lastIndexOf('/');
    var name = slash >= 0 ? playPath.substring(slash + 1) : playPath;
    var recordedAtUtc = parseUtcIsoFromWavFilename(name);
    return {
        name: name,
        path: playPath,
        sizeBytes: obj && obj.sizeBytes != null ? Number(obj.sizeBytes) : 0,
        durationMs: obj && obj.durationMs != null ? Number(obj.durationMs) : 0,
        endReason: obj && obj.endReason != null ? String(obj.endReason) : '',
        recordedAtUtc: recordedAtUtc || ''
    };
}

function parseSummaryNdjsonText(text) {
    var lines = String(text || '').split(/\r?\n/);
    var out = [];
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i].trim();
        if (!line) {
            continue;
        }
        try {
            var obj = JSON.parse(line);
            out.push(normalizeSummaryLineObject(obj));
        } catch (e) {
        }
    }
    // Newest first (UTC from filename); tie-break by name descending.
    out.sort(function(a, b) {
        var ka = recordingSortKeyMs(a);
        var kb = recordingSortKeyMs(b);
        if (kb !== ka) {
            return kb - ka;
        }
        return (b.name || '').localeCompare(a.name || '');
    });
    return out;
}

function setRecordingsTableLoadingVisible(show) {
    const el = document.getElementById('recordings-table-loading');
    if (el) {
        el.style.display = show ? 'flex' : 'none';
    }
}

function appendRecordingsTableRow(tbody, item) {
    const tr = document.createElement('tr');
    tr.className = 'recordings-table-row-enter';
    const td1 = document.createElement('td');
    td1.className = 'recordings-filename-cell';
    td1.textContent = '\uD83D\uDD0A ' + (item.name || '');
    const td2 = document.createElement('td');
    td2.textContent = formatRecordingDisplayTime(item);
    const td3 = document.createElement('td');
    td3.textContent = formatRecordingsBytes(item.sizeBytes);
    const td4 = document.createElement('td');
    const pb = document.createElement('button');
    pb.type = 'button';
    pb.className = 'btn-primary recordings-action-btn';
    pb.textContent = '\u25B6\uFE0F Play';
    pb.onclick = function() { playRecordingsItem(item.path); };
    td4.appendChild(pb);
    const td5 = document.createElement('td');
    const dl = document.createElement('a');
    dl.href = '/api/recordings/stream?path=' + encodeURIComponent(item.path) + '&download=1';
    dl.className = 'btn-secondary btn-info recordings-action-btn';
    dl.textContent = '\uD83D\uDCE5 Download';
    dl.setAttribute('download', item.name || 'recording.wav');
    td5.appendChild(dl);
    tr.appendChild(td1);
    tr.appendChild(td2);
    tr.appendChild(td3);
    tr.appendChild(td4);
    tr.appendChild(td5);
    tbody.appendChild(tr);
    requestAnimationFrame(function() {
        tr.classList.add('recordings-table-row-visible');
    });
}

async function loadRecordingsTable() {
    const tbody = document.getElementById('recordings-table-body');
    const pag = document.getElementById('recordings-pagination');
    const pagBar = document.getElementById('recordings-pagination-bar');
    const ind = document.getElementById('recordings-page-indicator');
    const dayLabel = document.getElementById('recordings-day-label');
    const err = document.getElementById('recordings-list-error');
    if (!recordingsSelectedDayPath) {
        setRecordingsTableLoadingVisible(false);
        if (tbody) tbody.innerHTML = '';
        if (pag) pag.style.display = 'none';
        if (pagBar) pagBar.style.display = 'none';
        if (dayLabel) {
            dayLabel.textContent = 'Select a day folder (YYYY / MON / DD) on the left.';
        }
        return;
    }
    const loadGen = ++recordingsTableLoadGeneration;
    if (err) { err.style.display = 'none'; err.textContent = ''; }
    setRecordingsTableLoadingVisible(true);
    if (tbody) tbody.innerHTML = '';
    if (pag) pag.style.display = 'none';
    if (pagBar) pagBar.style.display = 'flex';
    const url = '/api/recordings/summary?path=' + encodeURIComponent(recordingsSelectedDayPath);
    try {
        var allItems = recordingsDaySummaryCache[recordingsSelectedDayPath];
        if (!Array.isArray(allItems)) {
            const response = await fetchWithRetry(url, {}, 120000, 2);
            const text = await response.text();
            if (loadGen !== recordingsTableLoadGeneration) {
                return;
            }
            if (!response.ok) {
                throw new Error('Could not load recordings summary (' + response.status + ')');
            }
            allItems = parseSummaryNdjsonText(text);
            recordingsDaySummaryCache[recordingsSelectedDayPath] = allItems;
        }
        if (loadGen !== recordingsTableLoadGeneration) {
            return;
        }
        if (dayLabel) {
            dayLabel.textContent = 'Folder: ' + recordingsFormatPathForDisplay(recordingsSelectedDayPath);
        }
        const total = allItems.length;
        const totalPages = Math.max(1, Math.ceil(total / RECORDINGS_PAGE_SIZE));
        if (recordingsPageNum > totalPages) {
            recordingsPageNum = totalPages;
        }
        if (recordingsPageNum < 1) {
            recordingsPageNum = 1;
        }
        const start = (recordingsPageNum - 1) * RECORDINGS_PAGE_SIZE;
        const items = allItems.slice(start, start + RECORDINGS_PAGE_SIZE);
        setRecordingsTableLoadingVisible(false);
        if (tbody) {
            if (items.length === 0) {
                const tr = document.createElement('tr');
                const td = document.createElement('td');
                td.colSpan = 5;
                td.className = 'recordings-table-empty';
                td.textContent = 'No recordings listed for this day.';
                tr.appendChild(td);
                tbody.appendChild(tr);
            } else {
                for (let i = 0; i < items.length; i++) {
                    if (loadGen !== recordingsTableLoadGeneration) {
                        return;
                    }
                    appendRecordingsTableRow(tbody, items[i]);
                }
            }
        }
        if (pag && ind) {
            if (total > RECORDINGS_PAGE_SIZE) {
                pag.style.display = 'flex';
                ind.textContent = 'Page ' + recordingsPageNum + ' of ' + totalPages + ' (' + total + ' files)';
                document.getElementById('recordings-prev-page').disabled = recordingsPageNum <= 1;
                document.getElementById('recordings-next-page').disabled = recordingsPageNum >= totalPages;
            } else {
                pag.style.display = 'none';
            }
        }
    } catch (e) {
        if (loadGen !== recordingsTableLoadGeneration) {
            return;
        }
        setRecordingsTableLoadingVisible(false);
        if (err) {
            err.style.display = 'block';
            err.textContent = e.message || String(e);
        }
    }
}

function playRecordingsItem(filePath) {
    const a = document.getElementById('recordings-player');
    if (!a) return;
    a.src = '/api/recordings/stream?path=' + encodeURIComponent(filePath) + '&download=0';
    a.play().catch(function() {});
}

function initRecordingsPage() {
    if (connectionLost) {
        hideLoadingPopup();
        return;
    }
    stopRecordingsPlayback();
    recordingsTreeExpanded = {};
    recordingsChildren = {};
    recordingsSelectedDayPath = '';
    recordingsPageNum = 1;
    recordingsTreeLoadingPath = '/recordings';
    const dayLabelInit = document.getElementById('recordings-day-label');
    if (dayLabelInit) {
        dayLabelInit.textContent = 'Select a day folder (YYYY / MON / DD) on the left.';
    }
    setRecordingsTableLoadingVisible(false);
    const recordingsPagBarInit = document.getElementById('recordings-pagination-bar');
    if (recordingsPagBarInit) {
        recordingsPagBarInit.style.display = 'none';
    }

    syncDeviceClockFromSummary();
    if (recordingsClockInterval) {
        clearInterval(recordingsClockInterval);
        recordingsClockInterval = null;
    }
    recordingsClockInterval = setInterval(syncDeviceClockFromSummary, 5000);

    fetch('/api/device-info')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            applyProductBranding(d.product);
            applyDeviceCapabilities(d);
            const tzSlot = document.getElementById('page-title-extra-tz');
            if (tzSlot) {
                var tzName = '';
                try {
                    tzName = Intl.DateTimeFormat().resolvedOptions().timeZone || '';
                } catch (e2) {}
                tzSlot.textContent = ' \u201cRecorded\u201d uses this browser\u2019s local time' +
                    (tzName ? ' (' + tzName + ').' : '.');
            }
            const sh = document.getElementById('recordings-storage-hint');
            if (sh) {
                if (d.sdCardMounted === false) {
                    sh.textContent = 'SD card is not mounted. The recordings list lives on the SD card—insert a card, enable SD storage in settings, and reboot. PSRAM-only recordings do not appear here.';
                } else if (d.recordToSdCard === false) {
                    sh.textContent = '“Record to SD card” is off. Turn it on in Advanced / SD settings to build the recordings catalog; PSRAM-only clips are not listed.';
                } else {
                    sh.textContent = '';
                }
            }
        })
        .catch(function() {});

    const prevBtn = document.getElementById('recordings-prev-page');
    const nextBtn = document.getElementById('recordings-next-page');
    if (prevBtn) {
        prevBtn.onclick = function() {
            if (recordingsPageNum > 1) {
                recordingsPageNum -= 1;
                loadRecordingsTable();
            }
        };
    }
    if (nextBtn) {
        nextBtn.onclick = function() {
            recordingsPageNum += 1;
            loadRecordingsTable();
        };
    }
    const refreshDayBtn = document.getElementById('recordings-refresh-day');
    if (refreshDayBtn) {
        refreshDayBtn.onclick = function() {
            if (!recordingsSelectedDayPath) {
                return;
            }
            delete recordingsDaySummaryCache[recordingsSelectedDayPath];
            recordingsPageNum = 1;
            loadRecordingsTable();
        };
    }

    const recPlayer = document.getElementById('recordings-player');
    if (recPlayer && !recPlayer.dataset.errBound) {
        recPlayer.dataset.errBound = '1';
        recPlayer.addEventListener('error', function() {
            const url = recPlayer.src || '';
            if (url.indexOf('/api/recordings/stream') < 0) {
                return;
            }
            fetch(url, { headers: { 'Range': 'bytes=0-0' } })
                .then(function(r) {
                    if (r.ok) {
                        return;
                    }
                    return r.json().then(function(j) {
                        alert(j.message || 'File not found for playback');
                    });
                })
                .catch(function() {
                    alert('File not found for playback');
                });
        });
    }

    renderRecordingsTree();
    loadRecordingsFoldersForPath('/recordings')
        .then(function() {
            recordingsTreeLoadingPath = '';
            renderRecordingsTree();
            const te = document.getElementById('recordings-tree-error');
            if (te) { te.style.display = 'none'; te.textContent = ''; }
            return focusRecordingsTodayUtc();
        })
        .then(function() {
            hideLoadingPopup();
        })
        .catch(function(e) {
            recordingsTreeLoadingPath = '';
            renderRecordingsTree();
            const te = document.getElementById('recordings-tree-error');
            if (te) {
                te.style.display = 'block';
                te.textContent = e.message || String(e);
            }
            hideLoadingPopup();
        });
}

// Live Audio playback state
let liveAudioContext = null;
let liveAudioBufferQueue = []; // entries are Float32Array (already decoded + scaled)
let liveAudioIsPlaying = false;
let liveAudioSampleRate = 8000;
const LIVE_AUDIO_MIN_BUFFER_SAMPLES = 3; // Minimum chunks before starting playback (smoother startup vs ~150 ms latency)
const LIVE_AUDIO_SCHEDULE_LEAD_SEC = 0.15; // How far ahead of currentTime to (re)anchor the timeline on (re)start / underrun
let liveAudioLatencyStartTime = null;
let liveAudioNextPlayTime = null;
let liveAudioLastSeq = -1;

// G.711 µ-law (uint8) -> int16 PCM lookup. Built once on first use.
let liveAudioUlawToPcm = null;
function ensureUlawLut() {
    if (liveAudioUlawToPcm) return liveAudioUlawToPcm;
    const lut = new Int16Array(256);
    for (let i = 0; i < 256; i++) {
        let u = (~i) & 0xFF;
        const sign = u & 0x80;
        const exponent = (u >> 4) & 0x07;
        const mantissa = u & 0x0F;
        let sample = ((mantissa << 3) + 0x84) << exponent;
        sample -= 0x84;
        lut[i] = sign ? -sample : sample;
    }
    liveAudioUlawToPcm = lut;
    return lut;
}

function handleLiveAudioBinaryFrame(arrayBuffer) {
    if (!liveAudioIsPlaying || !liveAudioContext) {
        return;
    }
    if (arrayBuffer.byteLength < 20) {
        console.warn('[LiveAudio] Binary frame too short:', arrayBuffer.byteLength);
        return;
    }
    const view = new DataView(arrayBuffer);
    const magicOk = view.getUint8(0) === 0x42 && view.getUint8(1) === 0x41 &&
                    view.getUint8(2) === 0x55 && view.getUint8(3) === 0x44;
    if (!magicOk) {
        console.warn('[LiveAudio] Bad magic in binary frame');
        return;
    }
    const codec = view.getUint8(5);
    const sampleRate = view.getUint32(8, true);
    const sampleCount = view.getUint32(12, true);
    const seq = view.getUint32(16, true);

    if (typeof seq === 'number' && seq <= liveAudioLastSeq) {
        return; // duplicate or out-of-order
    }

    const payloadOffset = 20;
    const payloadBytes = arrayBuffer.byteLength - payloadOffset;
    const expectedBytes = (codec === 1) ? sampleCount : sampleCount * 2;
    if (payloadBytes !== expectedBytes) {
        console.error('[LiveAudio] Payload size mismatch: got', payloadBytes, 'expected', expectedBytes,
                      'codec=', codec, 'samples=', sampleCount);
        return;
    }

    const float32 = new Float32Array(sampleCount);
    if (codec === 1) {
        const lut = ensureUlawLut();
        const u8 = new Uint8Array(arrayBuffer, payloadOffset, sampleCount);
        for (let i = 0; i < sampleCount; i++) {
            float32[i] = lut[u8[i]] / 32768.0;
        }
    } else if (codec === 0) {
        const i16 = new Int16Array(arrayBuffer, payloadOffset, sampleCount);
        for (let i = 0; i < sampleCount; i++) {
            float32[i] = i16[i] / 32768.0;
        }
    } else {
        console.warn('[LiveAudio] Unknown codec id', codec);
        return;
    }

    liveAudioLastSeq = seq;
    if (sampleRate && sampleRate !== liveAudioSampleRate) {
        liveAudioSampleRate = sampleRate;
    }

    updateLiveAudioVisualizer(float32);
    liveAudioBufferQueue.push(float32);
    processLiveAudioScheduleQueue();

    const bufferStatus = document.getElementById('live-audio-buffer-status');
    if (bufferStatus) {
        let ahead = 0;
        if (liveAudioNextPlayTime != null) {
            ahead = Math.max(0, liveAudioNextPlayTime - liveAudioContext.currentTime);
        }
        bufferStatus.textContent = liveAudioBufferQueue.length + ' queued, ' + ahead.toFixed(2) + 's scheduled ahead';
    }

    if (liveAudioLatencyStartTime === null) {
        liveAudioLatencyStartTime = Date.now();
    }
    const latencyElement = document.getElementById('live-audio-latency');
    if (latencyElement) {
        const streamDuration = ((Date.now() - liveAudioLatencyStartTime) / 1000).toFixed(0);
        latencyElement.textContent = streamDuration + 's';
    }
}

function startLiveAudio() {
    console.log('[LiveAudio] startLiveAudio() called, liveAudioIsPlaying:', liveAudioIsPlaying);
    
    if (liveAudioIsPlaying) {
        console.log('[LiveAudio] Already playing, ignoring');
        return;
    }
    
    console.log('[LiveAudio] Starting live audio...');
    
    // Request WebSocket subscription if not already subscribed
    if (wsConnected && useWebSocket) {
        subscribeToPage('live-audio');
        console.log('[LiveAudio] Subscribed to live-audio page');
    } else {
        console.warn('[LiveAudio] WebSocket not connected! wsConnected:', wsConnected, 'useWebSocket:', useWebSocket);
    }
    
    // Initialize Web Audio API
    try {
        const AudioContext = window.AudioContext || window.webkitAudioContext;
        if (!AudioContext) {
            throw new Error('Web Audio API not supported in this browser');
        }
        
        console.log('[LiveAudio] Creating AudioContext with sampleRate:', liveAudioSampleRate);
        liveAudioContext = new AudioContext({ sampleRate: liveAudioSampleRate });
        liveAudioIsPlaying = true;
        
        console.log('[LiveAudio] AudioContext created, state:', liveAudioContext.state);
        
        // Update UI
        const startBtn = document.getElementById('start-live-audio-btn');
        const stopBtn = document.getElementById('stop-live-audio-btn');
        const statusText = document.getElementById('status-text');
        const statusDot = document.getElementById('status-dot');
        
        console.log('[LiveAudio] UI elements:', { startBtn: !!startBtn, stopBtn: !!stopBtn, statusText: !!statusText, statusDot: !!statusDot });
        
        if (startBtn) {
            startBtn.style.display = 'none';
            console.log('[LiveAudio] Start button hidden');
        } else {
            console.error('[LiveAudio] Start button not found for UI update!');
        }
        
        if (stopBtn) {
            stopBtn.style.display = 'inline-block';
            console.log('[LiveAudio] Stop button shown');
        } else {
            console.error('[LiveAudio] Stop button not found for UI update!');
        }
        
        if (statusText) {
            statusText.textContent = 'Streaming...';
            console.log('[LiveAudio] Status text updated');
        }
        
        if (statusDot) {
            statusDot.style.backgroundColor = '#4CAF50';
            statusDot.classList.add('pulsing');
            console.log('[LiveAudio] Status dot updated');
        }
        
        // Clear buffer queue and playback timeline
        liveAudioBufferQueue = [];
        liveAudioLatencyStartTime = null;
        liveAudioNextPlayTime = null;
        liveAudioLastSeq = -1;
        
        updateLiveAudioStatus('Buffering...', '#FFA500');
        console.log('[LiveAudio] Initialization complete, waiting for audio data...');
    } catch (error) {
        console.error('[LiveAudio] Error starting live audio:', error);
        updateLiveAudioStatus('Error: ' + error.message, '#D42329');
        liveAudioIsPlaying = false;
    }
}

function stopLiveAudio() {
    if (!liveAudioIsPlaying) return;
    
    liveAudioIsPlaying = false;

    if (liveAudioContext) {
        liveAudioContext.close().catch(() => {});
        liveAudioContext = null;
    }

    liveAudioBufferQueue = [];
    liveAudioLatencyStartTime = null;
    liveAudioNextPlayTime = null;
    liveAudioLastSeq = -1;
    
    // Update UI
    const startBtn = document.getElementById('start-live-audio-btn');
    const stopBtn = document.getElementById('stop-live-audio-btn');
    const statusText = document.getElementById('status-text');
    const statusDot = document.getElementById('status-dot');
    
    if (startBtn) startBtn.style.display = 'inline-block';
    if (stopBtn) stopBtn.style.display = 'none';
    if (statusText) statusText.textContent = 'Stopped';
    if (statusDot) {
        statusDot.style.backgroundColor = '#999';
        statusDot.classList.remove('pulsing');
    }
    
    // Unsubscribe from WebSocket
    if (wsConnected) {
        unsubscribeFromPage('live-audio');
    }
    resumeLiveAudioSessionOverrides();
}

function updateLiveAudioStatus(text, color) {
    const statusText = document.getElementById('status-text');
    const statusDot = document.getElementById('status-dot');
    if (statusText) {
        statusText.textContent = text;
        console.log('[LiveAudio] Status updated:', text);
    } else {
        console.warn('[LiveAudio] Status text element not found!');
    }
    if (statusDot && color) {
        statusDot.style.backgroundColor = color;
    }
}

function processLiveAudioScheduleQueue() {
    if (!liveAudioIsPlaying || !liveAudioContext) {
        return;
    }
    if (liveAudioBufferQueue.length < LIVE_AUDIO_MIN_BUFFER_SAMPLES && liveAudioNextPlayTime === null) {
        return;
    }
    if (liveAudioContext.state === 'suspended') {
        liveAudioContext.resume().catch(function() {});
    }

    const now = liveAudioContext.currentTime;
    let scheduledAny = false;
    while (liveAudioBufferQueue.length > 0) {
        const samples = liveAudioBufferQueue.shift();
        if (!samples) {
            continue;
        }
        let float32Samples;
        if (samples instanceof Float32Array) {
            float32Samples = samples;
        } else {
            float32Samples = new Float32Array(samples.length);
            for (let i = 0; i < samples.length; i++) {
                float32Samples[i] = samples[i] / 32768.0;
            }
        }
        const audioBuffer = liveAudioContext.createBuffer(1, float32Samples.length, liveAudioSampleRate);
        audioBuffer.copyToChannel(float32Samples, 0);
        const src = liveAudioContext.createBufferSource();
        src.buffer = audioBuffer;
        src.connect(liveAudioContext.destination);
        if (liveAudioNextPlayTime === null || liveAudioNextPlayTime < now) {
            liveAudioNextPlayTime = now + LIVE_AUDIO_SCHEDULE_LEAD_SEC;
        }
        try {
            src.start(liveAudioNextPlayTime);
            scheduledAny = true;
        } catch (e) {
            console.error('[LiveAudio] start() failed:', e);
        }
        liveAudioNextPlayTime += audioBuffer.duration;
    }
    if (scheduledAny) {
        updateLiveAudioStatus('Streaming...', '#4CAF50');
    }
}

function initLiveAudioVisualizer() {
    const canvas = document.getElementById('live-audio-visualizer-canvas');
    if (!canvas) return;
    
    const ctx = canvas.getContext('2d');
    // Make canvas responsive but maintain aspect ratio
    const container = canvas.parentElement;
    if (container) {
        canvas.width = container.offsetWidth || 800;
    } else {
        canvas.width = 800;
    }
    canvas.height = 200;
    
    // Store canvas context for visualizer updates
    window.liveAudioCanvasCtx = ctx;
    window.liveAudioCanvas = canvas;
    
    // Draw initial empty state
    drawVisualizerBackground(ctx, canvas);
}

function drawVisualizerBackground(ctx, canvas) {
    const width = canvas.width;
    const height = canvas.height;
    
    // Clear canvas with background
    ctx.fillStyle = '#A4EDFF'; // Light blue background (matching the image)
    ctx.fillRect(0, 0, width, height);
    
    // Draw border
    ctx.strokeStyle = '#0587C7';
    ctx.lineWidth = 2;
    ctx.strokeRect(0, 0, width, height);
    
    // Visualization area margins
    const marginTop = 30;
    const marginBottom = 30;
    const marginLeft = 50;
    const marginRight = 10;
    const vizTop = marginTop;
    const vizBottom = height - marginBottom;
    const vizLeft = marginLeft;
    const vizRight = width - marginRight;
    const vizHeight = vizBottom - vizTop;
    const centerY = (vizTop + vizBottom) / 2;
    
    // Draw amplitude reference lines and labels
    // 16-bit audio range: -32768 to 32767
    // Normalized range: -1.0 to 1.0
    const maxAmplitude = 32767; // Maximum value for 16-bit signed integer
    
    // Draw reference lines at -1.0, -0.5, 0, 0.5, 1.0
    const referenceLevels = [
        { value: -1.0, label: '-1.0', y: vizTop },
        { value: -0.5, label: '-0.5', y: vizTop + vizHeight * 0.25 },
        { value: 0.0, label: '0', y: centerY },
        { value: 0.5, label: '0.5', y: vizTop + vizHeight * 0.75 },
        { value: 1.0, label: '1.0', y: vizBottom }
    ];
    
    ctx.font = '12px monospace';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = '#002942';
    
    for (const level of referenceLevels) {
        // Draw horizontal reference line
        ctx.strokeStyle = level.value === 0 ? '#002942' : '#666';
        ctx.lineWidth = level.value === 0 ? 2 : 1;
        ctx.setLineDash(level.value === 0 ? [] : [4, 4]);
        ctx.beginPath();
        ctx.moveTo(vizLeft, level.y);
        ctx.lineTo(vizRight, level.y);
        ctx.stroke();
        ctx.setLineDash([]);
        
        // Draw label
        ctx.fillText(level.label, vizLeft - 10, level.y);
    }
}

function updateLiveAudioVisualizer(samples) {
    const ctx = window.liveAudioCanvasCtx;
    const canvas = window.liveAudioCanvas;
    if (!ctx || !canvas || !samples || samples.length === 0) return;
    
    const width = canvas.width;
    const height = canvas.height;
    
    // Draw background with reference lines
    drawVisualizerBackground(ctx, canvas);
    
    // Main visualization area (matching background function margins)
    const marginTop = 30;
    const marginBottom = 30;
    const marginLeft = 50;
    const marginRight = 10;
    const vizTop = marginTop;
    const vizBottom = height - marginBottom;
    const vizLeft = marginLeft;
    const vizRight = width - marginRight;
    const vizHeight = vizBottom - vizTop;
    const vizWidth = vizRight - vizLeft;
    const centerY = (vizTop + vizBottom) / 2;
    
    // Determine if samples are Int16Array (16-bit) or Float32Array (already normalized)
    const isInt16 = samples instanceof Int16Array;
    const maxAmplitude = isInt16 ? 32767 : 1.0; // For 16-bit: max is 32767, for Float32: already normalized
    
    // Draw waveform
    ctx.strokeStyle = '#800080'; // Purple color matching the image
    ctx.lineWidth = 1;
    ctx.beginPath();
    
    // Downsample if we have too many samples for smooth rendering
    // Show up to canvas width pixels worth of data
    const maxPoints = vizWidth;
    const step = samples.length > maxPoints ? Math.floor(samples.length / maxPoints) : 1;
    
    let firstPoint = true;
    for (let i = 0; i < samples.length; i += step) {
        // Normalize sample based on type
        let normalizedValue;
        if (isInt16) {
            // Normalize 16-bit samples (-32768 to 32767) to -1.0 to 1.0 range
            normalizedValue = samples[i] / maxAmplitude;
        } else {
            // Float32Array samples are already in -1.0 to 1.0 range
            normalizedValue = samples[i];
        }
        
        // Clamp to -1.0 to 1.0 range
        const clampedValue = Math.max(-1.0, Math.min(1.0, normalizedValue));
        
        // Map normalized value (-1.0 to 1.0) to canvas Y position
        // -1.0 maps to vizBottom, 0.0 maps to centerY, 1.0 maps to vizTop
        const y = centerY - (clampedValue * (vizHeight / 2));
        
        // Calculate X position
        const x = vizLeft + (i / samples.length) * vizWidth;
        
        if (firstPoint) {
            ctx.moveTo(x, y);
            firstPoint = false;
        } else {
            ctx.lineTo(x, y);
        }
    }
    
    ctx.stroke();
    
    // Draw waveform fill (semi-transparent)
    ctx.fillStyle = 'rgba(5, 135, 199, 0.15)';
    ctx.lineTo(vizLeft + vizWidth, centerY);
    ctx.lineTo(vizLeft, centerY);
    ctx.closePath();
    ctx.fill();
}

function resumeLiveAudioSessionOverrides() {
    if (connectionLost) return Promise.resolve();
    return fetchWithRetry('/api/live-audio/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pauseRecording: false, pauseUploads: false })
    }, 8000, 1).catch(function(e) { console.warn('[LiveAudio] resume session', e); });
}

async function loadLiveAudioSessionState() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/live-audio/session', {}, 5000, 1);
        if (!response.ok) return;
        const data = await response.json();
        const pr = document.getElementById('live-audio-pause-recording');
        const pu = document.getElementById('live-audio-pause-uploads');
        if (pr && data.pauseRecording !== undefined) pr.checked = !!data.pauseRecording;
        if (pu && data.pauseUploads !== undefined) pu.checked = !!data.pauseUploads;
    } catch (e) {
        console.warn('[LiveAudio] load session state', e);
    }
}

async function postLiveAudioSessionUpdate(partial) {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/live-audio/session', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(partial)
        }, 8000, 1);
        if (!response.ok) console.warn('[LiveAudio] session POST', response.status);
    } catch (e) {
        console.warn('[LiveAudio] session POST', e);
    }
}

function wireRecordInputChannelRadios() {
    if (!RECORD_INPUT_CHANNEL_SUPPORTED) return;
    const recordLeft = document.getElementById('record-input-left');
    const recordRight = document.getElementById('record-input-right');
    const onRecordChannelChange = function() {
        if (!this.checked) return;
        const v = parseInt(this.value, 10);
        if (v === 0 || v === 1) {
            applyMonitorInputChannel(v);
        }
    };
    if (recordLeft) recordLeft.addEventListener('change', onRecordChannelChange);
    if (recordRight) recordRight.addEventListener('change', onRecordChannelChange);
}

function initAudioControls() {
    wireRecordInputChannelRadios();

    const saveInputChBtn = document.getElementById('audio-graph-save-input-channel');
    if (saveInputChBtn) {
        saveInputChBtn.addEventListener('click', async function() {
            if (!RECORD_INPUT_CHANNEL_SUPPORTED) return;
            saveInputChBtn.disabled = true;
            const prev = saveInputChBtn.textContent;
            saveInputChBtn.textContent = '\u23F3 Saving...';
            try {
                const leftEl = document.getElementById('record-input-left');
                const rightEl = document.getElementById('record-input-right');
                let ch = 1;
                if (rightEl && rightEl.checked) ch = 0;
                else if (leftEl && leftEl.checked) ch = 1;
                await saveAudioSetting('recordInputChannel', ch);
                const response = await fetchWithRetry('/api/audio/save', { method: 'POST' }, 20000, 2);
                const data = await response.json();
                if (response.ok && jsonApiOk(data)) {
                    await showInfoModal(data.message || 'Input channel saved.', 'Recorder');
                } else {
                    await showInfoModal(data.message || 'Failed to save input channel.', 'Recorder');
                }
            } catch (e) {
                console.error(e);
                await showInfoModal('Error saving input channel.', 'Recorder');
            } finally {
                saveInputChBtn.disabled = false;
                saveInputChBtn.textContent = prev;
            }
        });
    }

    // Audio Gain
    const gainSlider = document.getElementById('audio-gain');
    const gainValue = document.getElementById('audio-gain-value');
    const gainLabel = document.getElementById('audio-gain-label');
    const gainValues = [-1, 0, 3, 6, 9, 12, 15, 18, 21, 24];
    if (gainSlider && gainValue && gainLabel) {
        const updateGainDisplay = (index) => {
            const gainDb = gainValues[index] ?? 0;
            gainValue.textContent = gainDb + ' dB';
            gainLabel.textContent = 'Audio Gain: ' + gainDb + ' dB';
        };
        // Initialize display
        updateGainDisplay(parseInt(gainSlider.value, 10) || 0);
        gainSlider.addEventListener('input', function() {
            const idx = parseInt(this.value, 10) || 0;
            updateGainDisplay(idx);
        });
        gainSlider.addEventListener('change', function() {
            const idx = parseInt(this.value, 10) || 0;
            const gainDb = gainValues[idx] ?? 0;
            saveAudioSetting('codecGainDb', gainDb);
        });
    }
    
    // Min Recording Seconds
    const minRecordingSlider = document.getElementById('min-recording-seconds');
    const minRecordingValue = document.getElementById('min-recording-seconds-value');
    if (minRecordingSlider && minRecordingValue) {
        minRecordingSlider.addEventListener('input', function() {
            minRecordingValue.textContent = this.value;
        });
        minRecordingSlider.addEventListener('change', function() {
            saveAudioSetting('minRecordingSeconds', parseFloat(this.value));
        });
    }
    
    // Max Recording Seconds
    const maxRecordingSlider = document.getElementById('max-recording-seconds');
    const maxRecordingValue = document.getElementById('max-recording-seconds-value');
    if (maxRecordingSlider && maxRecordingValue) {
        maxRecordingSlider.addEventListener('input', function() {
            maxRecordingValue.textContent = this.value;
        });
        maxRecordingSlider.addEventListener('change', function() {
            saveAudioSetting('maxRecordingSeconds', parseInt(this.value));
        });
    }
    
    // Silence Threshold Seconds
    const silenceSlider = document.getElementById('silence-threshold');
    const silenceValue = document.getElementById('silence-threshold-value');
    if (silenceSlider && silenceValue) {
        silenceSlider.addEventListener('input', function() {
            silenceValue.textContent = this.value;
        });
        silenceSlider.addEventListener('change', function() {
            saveAudioSetting('silenceThresholdMs', parseFloat(this.value));
        });
    }
    
    // Pre Recording Seconds
    const preRecordSlider = document.getElementById('pre-record-ms');
    const preRecordValue = document.getElementById('pre-record-ms-value');
    if (preRecordSlider && preRecordValue) {
        preRecordSlider.addEventListener('input', function() {
            preRecordValue.textContent = this.value;
        });
        preRecordSlider.addEventListener('change', function() {
            saveAudioSetting('preRecordMs', parseFloat(this.value, 10));
        });
    }
    
     // Audio actions: Set Defaults and Save Settings
    const setDefaultsBtn = document.getElementById('audio-set-defaults-btn');
    if (setDefaultsBtn) {
        setDefaultsBtn.addEventListener('click', async function() {
            if (!await showConfirmModal('Reset audio settings to their default values?', 'Reset Audio Settings')) {
                return;
            }
            try {
                const response = await fetchWithRetry('/api/audio/defaults', {
                    method: 'POST'
                }, 10000, 2);
                const data = await response.json();
                if (response.ok && jsonApiOk(data)) {
                    // Reload sliders from backend defaults
                    await loadAudioSettings();
                    await showInfoModal('Audio settings have been reset to defaults.', 'Audio Settings');
                } else {
                    await showInfoModal('Failed to reset audio settings: ' + (data.message || 'Unknown error'), 'Audio Settings');
                }
            } catch (error) {
                console.error('Error resetting audio defaults:', error);
                await showInfoModal('Error resetting audio settings to defaults.', 'Audio Settings');
            }
        });
    }
    
    const saveSettingsBtn = document.getElementById('audio-save-settings-btn');
    if (saveSettingsBtn) {
        saveSettingsBtn.addEventListener('click', async function() {
            if (!await showConfirmModal('Save audio settings and push them to the server?', 'Save Audio Settings')) {
                return;
            }
            saveSettingsBtn.disabled = true;
            const originalText = saveSettingsBtn.textContent;
            saveSettingsBtn.textContent = '\u23F3 Saving...';
            try {
                if (RECORD_INPUT_CHANNEL_SUPPORTED) {
                    const leftEl = document.getElementById('record-input-left');
                    const rightEl = document.getElementById('record-input-right');
                    let ch = 1;
                    if (rightEl && rightEl.checked) ch = 0;
                    else if (leftEl && leftEl.checked) ch = 1;
                    await saveAudioSetting('recordInputChannel', ch);
                }
                const response = await fetchWithRetry('/api/audio/save', {
                    method: 'POST'
                }, 20000, 2);
                const data = await response.json();
                if (response.ok && jsonApiOk(data)) {
                    await showInfoModal(data.message || 'Audio settings saved and pushed to server.', 'Audio Settings');
                } else {
                    await showInfoModal(data.message || 'Failed to push audio settings to server.', 'Audio Settings');
                }
            } catch (error) {
                console.error('Error saving audio settings:', error);
                await showInfoModal('Error saving audio settings.', 'Audio Settings');
            } finally {
                saveSettingsBtn.disabled = false;
                saveSettingsBtn.textContent = originalText;
            }
        });
    }
    
    // Audio Threshold
    const audioThresholdSlider = document.getElementById('audio-threshold');
    const audioThresholdValue = document.getElementById('audio-threshold-value');
    const audioThresholdLabel = document.getElementById('audio-threshold-label');
    
    // Function to calculate dB from threshold setting (matches C++ logic)
    function calculateThresholdDb(threshold) {
        return threshold - 80;
    }
    
    if (audioThresholdSlider && audioThresholdValue) {        
        audioThresholdSlider.addEventListener('input', function() {
            const value = parseInt(this.value);
            audioThresholdValue.textContent = value;
            currentAudioThreshold = value;
        });
        audioThresholdSlider.addEventListener('change', function() {
            currentAudioThreshold = parseInt(this.value);
            saveAudioSetting('audioThreshold', currentAudioThreshold);
        });
    }
    
    // Discard Enabled
    const discardEnabled = document.getElementById('discard-enabled');
    const discardSliderContainer = document.getElementById('discard-slider-container');
    const discardSlider = document.getElementById('discard-millis');
    const discardValue = document.getElementById('discard-millis-value');
    
    if (discardEnabled && discardSliderContainer) {
        discardEnabled.addEventListener('change', function() {
            discardSliderContainer.style.display = this.checked ? 'block' : 'none';
            saveAudioSetting('discardEnabled', this.checked);
        });
    }
    
    if (discardSlider && discardValue) {
        discardSlider.addEventListener('input', function() {
            discardValue.textContent = this.value;
        });
        discardSlider.addEventListener('change', function() {
            saveAudioSetting('discardMillis', parseFloat(this.value));
        });
    }
}

async function applyMonitorInputChannel(ch) {
    if (!RECORD_INPUT_CHANNEL_SUPPORTED) return;
    if (connectionLost) return;
    const c = (parseInt(ch, 10) === 0) ? 0 : 1;
    try {
        const response = await fetchWithRetry('/api/audio/monitor-input-channel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ channel: c })
        }, 5000, 1);
        if (!response.ok) {
            console.warn('[Audio] monitor-input-channel HTTP', response.status);
        }
    } catch (e) {
        console.warn('[Audio] monitor-input-channel', e);
    }
}

async function loadAudioSettings() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/audio/settings', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError();
        
        // Update sliders
        await applyRecordInputChannelFromSettingsData(data);

        if (data.minRecordingMs !== undefined) {
            const seconds = data.minRecordingMs / 1000;
            const slider = document.getElementById('min-recording-seconds');
            const value = document.getElementById('min-recording-seconds-value');
            if (slider) slider.value = seconds;
            if (value) value.textContent = seconds;
        }
        
        if (data.maxRecordingMs !== undefined) {
            const seconds = Math.round(data.maxRecordingMs / 1000);
            const slider = document.getElementById('max-recording-seconds');
            const value = document.getElementById('max-recording-seconds-value');
            if (slider) {
                slider.value = seconds;
                slider.max = maxMaxRecordingTime;
            }
            if (value) value.textContent = seconds;
        }

        if (data.silenceThresholdMs !== undefined) {
            const seconds = data.silenceThresholdMs / 1000;
            const slider = document.getElementById('silence-threshold');
            const value = document.getElementById('silence-threshold-value');
            if (slider) slider.value = seconds;
            if (value) value.textContent = seconds;
        }
        
        if (data.audioThreshold !== undefined) {
            const slider = document.getElementById('audio-threshold');
            const value = document.getElementById('audio-threshold-value');
            if (slider) slider.value = data.audioThreshold;
            if (value) value.textContent = data.audioThreshold;
            currentAudioThreshold = data.audioThreshold;
        }
        
        if (data.preRecordMs !== undefined) {
            const seconds = data.preRecordMs / 1000;
            const slider = document.getElementById('pre-record-ms');
            const value = document.getElementById('pre-record-ms-value');
            if (slider) slider.value = seconds;
            if (value) value.textContent = seconds;
        }
        
        if (data.codecGainDb !== undefined) {
            const gainSlider = document.getElementById('audio-gain');
            const gainValue = document.getElementById('audio-gain-value');
            const gainLabel = document.getElementById('audio-gain-label');
            if (gainSlider && gainValue && gainLabel) {
                const gainValues = [-1, 0, 3, 6, 9, 12, 15, 18, 21, 24];
                let bestIndex = 0;
                let bestDiff = Math.abs(data.codecGainDb - gainValues[0]);
                for (let i = 1; i < gainValues.length; i++) {
                    const diff = Math.abs(data.codecGainDb - gainValues[i]);
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        bestIndex = i;
                    }
                }
                gainSlider.value = String(bestIndex);
                const gainDb = gainValues[bestIndex];
                gainValue.textContent = gainDb + ' dB';
                gainLabel.textContent = 'Audio Gain: ' + gainDb + ' dB';
            }
        }
        
        if (data.discardEnabled !== undefined) {
            const checkbox = document.getElementById('discard-enabled');
            const container = document.getElementById('discard-slider-container');
            if (checkbox) checkbox.checked = data.discardEnabled;
            if (container) container.style.display = data.discardEnabled ? 'block' : 'none';
        }
        
        if (data.discardMillis !== undefined) {
            const seconds = data.discardMillis / 1000;
            const slider = document.getElementById('discard-millis');
            const value = document.getElementById('discard-millis-value');
            if (slider) slider.value = seconds;
            if (value) value.textContent = seconds;
        }
    } catch (error) {
        console.error('Error loading audio settings:', error);
    }
}

async function saveAudioSetting(setting, value) {
    if (connectionLost) return;
    try {
        let paramName = '';
        let paramValue = value;
        
        if (setting === 'minRecordingSeconds') {
            paramName = 'audio.minrecordingms';
            paramValue = value * 1000; // Convert seconds to milliseconds
        } else if (setting === 'maxRecordingSeconds') {
            paramName = 'audio.maxrecordingms';
            paramValue = value * 1000; // Convert seconds to milliseconds
        } else if (setting === 'silenceThresholdMs') {
            paramName = 'audio.silencethresholdms';
            paramValue = value * 1000; // Convert seconds to milliseconds
        } else if (setting === 'audioThreshold') {
            paramName = 'audio.audiothreshold';
        } else if (setting === 'preRecordMs') {
            paramName = 'audio.prerecordms';
            paramValue = value * 1000; // Convert seconds to milliseconds
        } else if (setting === 'codecGainDb') {
            paramName = 'audio.codecgain';
        } else if (setting === 'recordInputChannel') {
            if (!RECORD_INPUT_CHANNEL_SUPPORTED) return;
            paramName = 'audio.recordinputchannel';
            paramValue = parseInt(value, 10);
        } else if (setting === 'discardEnabled') {
            paramName = 'audio.discardSmallFilesEnabled';
            paramValue = value ? 'true' : 'false';
        } else if (setting === 'discardMillis') {
            paramName = 'audio.discardSmallFilesMinMs';
            paramValue = value * 1000; // Convert seconds to milliseconds
        }
        
        if (!paramName) return;
        
        const response = await fetchWithRetry('/api/audio/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({param: paramName, value: paramValue})
        });
        
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        if (!jsonApiOk(data)) {
            console.error('Failed to save audio setting:', data.message);
        }
    } catch (error) {
        console.error('Error saving audio setting:', error);
    }
}

function initPlayerTxPage() {
    const speakerEnabled = document.getElementById('echo-speaker-enabled');
    const speakerVol = document.getElementById('echo-speaker-volume');
    const speakerVolVal = document.getElementById('echo-speaker-volume-value');
    const speakerVolGroup = document.getElementById('echo-speaker-volume-group');

    const txEnabled = document.getElementById('echo-tx-enabled');
    const txVol = document.getElementById('echo-tx-volume');
    const txVolVal = document.getElementById('echo-tx-volume-value');
    const txVolGroup = document.getElementById('echo-tx-volume-group');

    const updateVisibility = () => {
        if (speakerVolGroup && speakerEnabled) {
            speakerVolGroup.style.display = speakerEnabled.checked ? 'block' : 'none';
        }
        if (txVolGroup && txEnabled) {
            txVolGroup.style.display = txEnabled.checked ? 'block' : 'none';
        }
    };

    if (speakerEnabled) {
        speakerEnabled.addEventListener('change', function() {
            updateVisibility();
            savePlayerTxParam('audio.speakerenabled', this.checked ? 'true' : 'false');
        });
    }
    if (speakerVol && speakerVolVal) {
        speakerVol.addEventListener('input', function() {
            speakerVolVal.textContent = this.value;
        });
        speakerVol.addEventListener('change', function() {
            savePlayerTxParam('audio.speakervolume', parseInt(this.value, 10));
        });
    }

    if (txEnabled) {
        txEnabled.addEventListener('change', function() {
            updateVisibility();
            savePlayerTxParam('audio.transmitenabled', this.checked ? 'true' : 'false');
        });
    }
    if (txVol && txVolVal) {
        txVol.addEventListener('input', function() {
            txVolVal.textContent = this.value;
        });
        txVol.addEventListener('change', function() {
            savePlayerTxParam('audio.transmitvolume', parseInt(this.value, 10));
        });
    }

    updateVisibility();
    loadPlayerTxSettings()
        .catch(() => {})
        .then(() => {
            updateVisibility();
            hideLoadingPopup();
        });
}

async function loadPlayerTxSettings() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/audio/settings', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();

        const speakerEnabled = document.getElementById('echo-speaker-enabled');
        const speakerVol = document.getElementById('echo-speaker-volume');
        const speakerVolVal = document.getElementById('echo-speaker-volume-value');

        const txEnabled = document.getElementById('echo-tx-enabled');
        const txVol = document.getElementById('echo-tx-volume');
        const txVolVal = document.getElementById('echo-tx-volume-value');

        if (speakerEnabled && data.speakerEnabled !== undefined) speakerEnabled.checked = !!data.speakerEnabled;
        if (speakerVol && data.speakerVolume !== undefined) speakerVol.value = String(data.speakerVolume);
        if (speakerVolVal && data.speakerVolume !== undefined) speakerVolVal.textContent = String(data.speakerVolume);

        if (txEnabled && data.transmitEnabled !== undefined) txEnabled.checked = !!data.transmitEnabled;
        if (txVol && data.transmitVolume !== undefined) txVol.value = String(data.transmitVolume);
        if (txVolVal && data.transmitVolume !== undefined) txVolVal.textContent = String(data.transmitVolume);
    } catch (error) {
        console.error('Error loading player/tx settings:', error);
    }
}

async function savePlayerTxParam(paramName, paramValue) {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/audio/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({param: paramName, value: paramValue})
        });
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        if (!data.success) {
            console.error('Failed to save player/tx setting:', data.message);
        }
    } catch (error) {
        console.error('Error saving player/tx setting:', error);
    }
}

// --- CW (Morse) page (client-side presets/history + visualization) ---
let cwSendTimer = null;
let cwSendState = null;

function cwMorseForChar(c) {
    // Uppercase expected
    const map = {
        'A': '.-',    'B': '-...',  'C': '-.-.',  'D': '-..',   'E': '.',
        'F': '..-.',  'G': '--.',   'H': '....',  'I': '..',    'J': '.---',
        'K': '-.-',   'L': '.-..',  'M': '--',    'N': '-.',    'O': '---',
        'P': '.--.',  'Q': '--.-',  'R': '.-.',   'S': '...',   'T': '-',
        'U': '..-',   'V': '...-',  'W': '.--',   'X': '-..-',  'Y': '-.--',
        'Z': '--..',
        '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-',
        '5': '.....', '6': '-....', '7': '--...', '8': '---..', '9': '----.',
        '.': '.-.-.-', ',': '--..--', '?': '..--..', '/': '-..-.', '=': '-...-',
        '+': '.-.-.',  '-': '-....-', '(': '-.--.', ')': '-.--.-', ':': '---...',
        ';': '-.-.-.', '\'': '.----.', '"': '.-..-.', '!': '-.-.--', '@': '.--.-.'
    };
    return map[c] || null;
}

function cwBuildTimeline(text, wpm, repeat) {
    const dotMs = Math.max(20, Math.floor(1200 / Math.max(5, wpm)));
    const dashMs = dotMs * 3;
    const intraMs = dotMs;
    const charGapMs = dotMs * 3;
    const wordGapMs = dotMs * 7;

    const steps = []; // {type:'tone'|'gap', ms, sym?:'.'|'-'}
    const pushGap = (ms) => { if (ms > 0) steps.push({type:'gap', ms}); };
    const pushTone = (sym, ms) => { steps.push({type:'tone', sym, ms}); };

    const clean = (text || '').trim();
    const msg = clean.toUpperCase();
    if (!msg) return {steps: [], dotMs};

    const reps = Math.max(1, Math.min(5, repeat || 1));
    for (let r = 0; r < reps; r++) {
        let firstChar = true;
        for (let i = 0; i < msg.length; i++) {
            const ch = msg[i];
            const isSpace = (ch === ' ' || ch === '\t' || ch === '\n' || ch === '\r');
            if (isSpace) {
                pushGap(wordGapMs);
                firstChar = true;
                continue;
            }

            if (!firstChar) pushGap(charGapMs);
            firstChar = false;

            const code = cwMorseForChar(ch);
            if (!code) {
                pushGap(wordGapMs);
                firstChar = true;
                continue;
            }
            for (let k = 0; k < code.length; k++) {
                const sym = code[k];
                pushTone(sym, sym === '.' ? dotMs : dashMs);
                if (k + 1 < code.length) pushGap(intraMs);
            }
        }
        if (r + 1 < reps) pushGap(wordGapMs);
    }

    return {steps, dotMs};
}

function cwStopSendingUi() {
    if (cwSendTimer) {
        clearTimeout(cwSendTimer);
        cwSendTimer = null;
    }
    cwSendState = null;
    // Clear any selection highlight.
    const text = document.getElementById('cw-text');
    if (text && typeof text.setSelectionRange === 'function') {
        try { text.setSelectionRange(0, 0); } catch {}
    }
}

function cwEstimateCharDurationsMs(text, wpm) {
    // Used only for UI highlighting cadence (client-side). Firmware plays actual audio.
    const dotMs = Math.max(20, Math.floor(1200 / Math.max(5, wpm)));
    const dashMs = dotMs * 3;
    const intraMs = dotMs;
    const charGapMs = dotMs * 3;
    const wordGapMs = dotMs * 7;

    const s = String(text || '').toUpperCase();
    const dur = [];
    for (let i = 0; i < s.length; i++) {
        const ch = s[i];
        if (ch === ' ') {
            dur.push(wordGapMs);
            continue;
        }
        const code = cwMorseForChar(ch);
        if (!code) {
            dur.push(wordGapMs);
            continue;
        }
        let ms = 0;
        for (let k = 0; k < code.length; k++) {
            ms += (code[k] === '.') ? dotMs : dashMs;
            if (k + 1 < code.length) ms += intraMs;
        }
        ms += charGapMs;
        dur.push(ms);
    }
    return dur;
}

function cwStartSendingUi(text, wpm, repeat) {
    cwStopSendingUi();
    const reps = Math.max(1, Math.min(5, repeat || 1));
    const baseText = String(text || '');
    if (!baseText) return;

    // Build sequence of char indices across repeats.
    const seq = [];
    for (let r = 0; r < reps; r++) {
        for (let i = 0; i < baseText.length; i++) seq.push(i);
    }
    const durs = cwEstimateCharDurationsMs(baseText, wpm);
    cwSendState = {baseText, seq, pos: 0, durs};

    const tick = () => {
        if (!cwSendState) return;
        const st = cwSendState;
        const idx = st.seq[st.pos];
        if (idx === undefined) { cwStopSendingUi(); return; }
        // Highlight inside the actual textbox by selecting the character.
        const ta = document.getElementById('cw-text');
        if (ta && typeof ta.setSelectionRange === 'function') {
            try {
                // Selection highlight works with readonly; we avoid disabling the textarea while sending.
                ta.focus({preventScroll: true});
                const start = Math.max(0, Math.min(idx, ta.value.length));
                const end = Math.min(ta.value.length, start + 1);
                ta.setSelectionRange(start, end);
            } catch {}
        }
        const wait = Math.max(60, st.durs[idx] || 120);
        cwSendTimer = setTimeout(() => {
            if (!cwSendState) return;
            cwSendState.pos++;
            tick();
        }, wait);
    };
    tick();
}

function cwLoadJson(key, fallback) {
    try {
        const raw = localStorage.getItem(key);
        if (!raw) return fallback;
        return JSON.parse(raw);
    } catch {
        return fallback;
    }
}

function cwSaveJson(key, value) {
    try { localStorage.setItem(key, JSON.stringify(value)); } catch {}
}

function cwSanitizeMessageText(input) {
    // Firmware supports: A-Z 0-9 and common punctuation (see morseForChar in recorder.cpp).
    // Keep spaces; force uppercase; drop unsupported characters.
    const s = String(input || '').toUpperCase();
    let out = '';
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        const ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c === ' ' ||
            c === '.' || c === ',' || c === '?' || c === '/' ||
            c === '=' || c === '+' || c === '-' ||
            c === '(' || c === ')' ||
            c === ':' || c === ';' ||
            c === '\'' || c === '"' ||
            c === '!' || c === '@';
        if (ok) out += c;
    }
    // Collapse whitespace to single spaces for nicer output.
    out = out.replace(/\s+/g, ' ').trimStart();
    return out;
}

function initCwPage() {
    const wpm = document.getElementById('cw-wpm');
    const wpmVal = document.getElementById('cw-wpm-value');
    const tone = document.getElementById('cw-tone');
    const toneVal = document.getElementById('cw-tone-value');
    const vol = document.getElementById('cw-volume');
    const volVal = document.getElementById('cw-volume-value');
    const repeat = document.getElementById('cw-repeat');
    const repeatVal = document.getElementById('cw-repeat-value');

    const text = document.getElementById('cw-text');
    const sendBtn = document.getElementById('cw-send');
    const stopBtn = document.getElementById('cw-stop');
    const clearBtn = document.getElementById('cw-clear');
    const status = document.getElementById('cw-status');
    const histRoot = document.getElementById('cw-history');
    const histClear = document.getElementById('cw-history-clear');
    const saveSettingsBtn = document.getElementById('cw-save-settings');

    const setStatus = (msg, isErr=false) => {
        if (!status) return;
        status.textContent = msg || '';
        status.style.color = isErr ? '#b91c1c' : '';
    };

    const historyKey = 'cw.history.v1';
    const applyCwSettingsFromDevice = (s) => {
        if (!s) return;
        if (wpm && s.cwWpm != null) wpm.value = String(s.cwWpm);
        if (tone && s.cwToneHz != null) tone.value = String(s.cwToneHz);
        if (vol && s.cwVolume != null) vol.value = String(s.cwVolume);
        if (repeat && s.cwRepeat != null) repeat.value = String(s.cwRepeat);
        if (wpmVal) wpmVal.textContent = wpm ? wpm.value : '';
        if (toneVal) toneVal.textContent = tone ? tone.value : '';
        if (volVal) volVal.textContent = vol ? vol.value : '';
        if (repeatVal) repeatVal.textContent = repeat ? repeat.value : '';
    };

    const renderHistory = () => {
        if (!histRoot) return;
        const items = cwLoadJson(historyKey, []);
        histRoot.innerHTML = '';
        if (!Array.isArray(items) || items.length === 0) {
            histRoot.innerHTML = '<div class="form-help">No messages yet.</div>';
            return;
        }
        items.slice(0, 12).forEach((it) => {
            const row = document.createElement('div');
            row.className = 'cw-hrow';
            const left = document.createElement('div');
            left.className = 'cw-hleft';
            const t = document.createElement('div');
            t.className = 'cw-htxt';
            t.textContent = it.text || '';
            const meta = document.createElement('div');
            meta.className = 'cw-hmeta';
            const ts = it.ts ? new Date(Number(it.ts)) : null;
            const tsStr = ts && !Number.isNaN(ts.getTime())
                ? ts.toLocaleString(undefined, { year: 'numeric', month: '2-digit', day: '2-digit', hour: 'numeric', minute: '2-digit', second: '2-digit' })
                : '';
            meta.textContent =
                (tsStr ? (tsStr + ' • ') : '') +
                (it.wpm || '--') + ' WPM • ' + (it.toneHz || '--') + ' Hz • Vol ' + (it.volume || '--') + ' • x' + (it.repeat || 1);
            left.appendChild(t);
            left.appendChild(meta);

            // Click anywhere on the row to load.
            row.style.cursor = 'pointer';
            row.addEventListener('click', (ev) => {
                if (text) text.value = cwSanitizeMessageText(it.text || '');
                setStatus('Loaded message from history.');
            });

            row.appendChild(left);
            histRoot.appendChild(row);
        });
    };

    const pushHistory = (entry) => {
        const items = cwLoadJson(historyKey, []);
        const stamped = Object.assign({ ts: Date.now() }, entry);
        const next = [stamped].concat(Array.isArray(items) ? items : []);
        // De-dupe by text+params
        const seen = new Set();
        const deduped = [];
        for (const e of next) {
            const k = JSON.stringify([e.text, e.wpm, e.toneHz, e.volume, e.repeat]);
            if (seen.has(k)) continue;
            seen.add(k);
            deduped.push(e);
            if (deduped.length >= 12) break;
        }
        cwSaveJson(historyKey, deduped);
        renderHistory();
    };

    renderHistory();

    // Load global CW settings from device.
    (async () => {
        try {
            const resp = await fetchWithRetry('/api/audio/settings', { method: 'GET' }, 8000, 1);
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            const data = await resp.json();
            applyCwSettingsFromDevice(data);
        } catch (e) {
            // Fall back to existing default slider values.
            if (wpmVal && wpm) wpmVal.textContent = wpm.value;
            if (toneVal && tone) toneVal.textContent = tone.value;
            if (volVal && vol) volVal.textContent = vol.value;
            if (repeatVal && repeat) repeatVal.textContent = repeat.value;
        }
    })();

    if (text) {
        // Enforce CW-compatible characters and uppercase as the user types.
        text.addEventListener('input', () => {
            const before = text.value || '';
            const after = cwSanitizeMessageText(before);
            if (after !== before) {
                const pos = text.selectionStart || 0;
                text.value = after;
                try { text.setSelectionRange(Math.min(pos, after.length), Math.min(pos, after.length)); } catch {}
            }
        });
        // Also sanitize on paste (some browsers won't fire input soon enough for selection preservation).
        text.addEventListener('paste', () => {
            setTimeout(() => {
                const before = text.value || '';
                const after = cwSanitizeMessageText(before);
                if (after !== before) text.value = after;
            }, 0);
        });
    }

    const bindSlider = (el, out) => {
        if (!el || !out) return;
        el.addEventListener('input', () => { out.textContent = el.value; });
    };
    bindSlider(wpm, wpmVal);
    bindSlider(tone, toneVal);
    bindSlider(vol, volVal);
    bindSlider(repeat, repeatVal);

    if (clearBtn && text) clearBtn.addEventListener('click', () => { text.value = ''; setStatus('Cleared.'); });

    if (histClear) {
        histClear.addEventListener('click', () => {
            cwSaveJson(historyKey, []);
            renderHistory();
            setStatus('History cleared.');
        });
    }

    if (saveSettingsBtn) {
        saveSettingsBtn.addEventListener('click', async () => {
            if (connectionLost) return;
            const payload = {
                commands: [
                    { action: 'set', param: 'cw.wpm', value: String(wpm ? wpm.value : '18') },
                    { action: 'set', param: 'cw.toneHz', value: String(tone ? tone.value : '600') },
                    { action: 'set', param: 'cw.volume', value: String(vol ? vol.value : '60') },
                    { action: 'set', param: 'cw.repeat', value: String(repeat ? repeat.value : '1') },
                ]
            };
            setStatus('Saving CW settings…');
            try {
                const r1 = await fetchWithRetry('/api/cmd', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(payload)
                }, 12000, 1);
                if (!r1.ok) throw new Error('HTTP ' + r1.status);
                const r2 = await fetchWithRetry('/api/cw/save', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({ save: true })
                }, 12000, 1);
                if (!r2.ok) throw new Error('HTTP ' + r2.status);
                setStatus('CW settings saved to device.');
            } catch (e) {
                setStatus('Save failed: ' + (e && e.message ? e.message : String(e)), true);
            }
        });
    }

    async function cwSend() {
        if (connectionLost) return;
        const msg = text ? cwSanitizeMessageText(text.value || '') : '';
        if (text) text.value = msg;
        if (!msg) { setStatus('Type a message first.', true); return; }

        const payload = {
            text: msg,
            wpm: wpm ? parseInt(wpm.value, 10) : 18,
            toneHz: tone ? parseInt(tone.value, 10) : 700,
            volume: vol ? parseInt(vol.value, 10) : 60,
            repeat: repeat ? parseInt(repeat.value, 10) : 1
        };

        setStatus('Sending…');
        // Lock controls while sending. Keep the textarea enabled (readonly) so selection highlight works.
        const lock = (on) => {
            const els = [wpm, tone, vol, repeat, sendBtn, clearBtn, saveSettingsBtn];
            els.forEach(el => { if (el) el.disabled = !!on; });
            if (text) text.readOnly = !!on;
            if (stopBtn) stopBtn.disabled = false; // always allow stop
        };
        lock(true);
        cwStartSendingUi(payload.text, payload.wpm, payload.repeat);
        try {
            const resp = await fetchWithRetry('/api/audio/morse', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(payload)
            }, 12000, 1);
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            setStatus('Sent to device. Playing…');
            pushHistory(payload);
        } catch (e) {
            cwStopSendingUi();
            lock(false);
            setStatus('Failed to send: ' + (e && e.message ? e.message : String(e)), true);
        }
    }

    async function cwStop() {
        if (connectionLost) return;
        setStatus('Stopping…');
        cwStopSendingUi();
        // Unlock controls immediately (best-effort; device stop follows).
        const els = [wpm, tone, vol, repeat, sendBtn, clearBtn, saveSettingsBtn];
        els.forEach(el => { if (el) el.disabled = false; });
        if (text) text.readOnly = false;
        try {
            const resp = await fetchWithRetry('/api/audio/morse', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({stop: true})
            }, 8000, 1);
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            setStatus('Stopped.');
        } catch (e) {
            setStatus('Stop failed: ' + (e && e.message ? e.message : String(e)), true);
        }
    }

    if (sendBtn) sendBtn.addEventListener('click', cwSend);
    if (stopBtn) stopBtn.addEventListener('click', cwStop);

    hideLoadingPopup();
}
function initAudioGraph() {
    const canvas = document.getElementById('audio-graph');
    if (!canvas) {
        console.error('Audio graph canvas not found');
        return;
    }
    const ctx = canvas.getContext('2d');
    const themeColor = (name, fallback) => getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
    console.log('Audio graph initialized, canvas found');
    
    function resizeCanvas() {
        const container = canvas.parentElement;
        if (container) {
            canvas.width = container.clientWidth - 40;
            canvas.height = window.innerWidth <= 768 ? 200 : 300;
        }
    }
    
    function drawGraph() {
        // Don't draw if tab is not visible (but continue running interval for WebSocket data updates)
        if (!isTabVisible) {
            return;
        }
        
        // Ensure canvas exists
        if (!canvas) {
            console.error('Canvas not found in drawGraph');
            return;
        }
        
        const container = canvas.parentElement;
        if (container) {
            const newWidth = container.clientWidth - 40;
            const newHeight = window.innerWidth <= 768 ? 200 : (window.innerWidth <= 1024 ? 250 : 300);
            if (canvas.width !== newWidth || canvas.height !== newHeight) {
                canvas.width = newWidth;
                canvas.height = newHeight;
            }
        }
        
        const width = canvas.width;
        const height = canvas.height;
        
        // Define margins for axes
        const leftMargin = 50;  // Space for left Y axis labels
        const rightMargin = 50; // Space for right Y axis labels
        const topMargin = 20;   // Space for top labels
        const bottomMargin = 30; // Space for bottom X axis labels
        const graphWidth = width - leftMargin - rightMargin;
        const graphHeight = height - topMargin - bottomMargin;
        const graphX = leftMargin;
        const graphY = topMargin;
        
        ctx.clearRect(0, 0, width, height);
        ctx.fillStyle = themeColor('--ui-panel', '#FFFFFF');
        ctx.fillRect(0, 0, width, height);
        
        // Draw grid lines
        ctx.strokeStyle = themeColor('--ui-border', '#E0E0E0');
        ctx.lineWidth = 1;
        for (let i = 0; i <= 4; i++) {
            const y = graphY + (graphHeight / 4) * i;
            ctx.beginPath();
            ctx.moveTo(graphX, y);
            ctx.lineTo(graphX + graphWidth, y);
            ctx.stroke();
        }
        
        // Draw threshold line (horizontal dotted line based on current threshold)
        // The threshold is converted to dB using the same logic as recording:
        // Mapping: 0 = -80db, 100 = 20db
        let thresholdDb = currentAudioThreshold - 80;
        
        // Map dB to Y position (right Y axis range: -80 dB to 0 dB)
        const dbMin = -80;
        const dbMax = 0;
        const thresholdY = graphY + graphHeight - ((thresholdDb - dbMin) / (dbMax - dbMin)) * graphHeight;
        
        ctx.strokeStyle = themeColor('--ui-accent', '#00008B');
        ctx.setLineDash([5, 5]);
        ctx.lineWidth = 4; // Thicker line
        ctx.beginPath();
        ctx.moveTo(graphX, thresholdY);
        ctx.lineTo(graphX + graphWidth, thresholdY);
        ctx.stroke();
        ctx.setLineDash([]);
        
        // Draw threshold label above the threshold line in the center of the graph
        ctx.fillStyle = themeColor('--ui-accent', '#00008B');
        ctx.font = window.innerWidth <= 480 ? '9px Arial' : '11px Arial';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'bottom'; // Align to bottom so text appears above the line
        //const thresholdLabel = 'Threshold ( ' + currentAudioThreshold + '% / ' + thresholdDb.toFixed(1) + ' dB )';
        const thresholdLabel = 'Threshold ( ' + currentAudioThreshold + ' )';
        ctx.fillText(thresholdLabel, graphX + graphWidth / 2, thresholdY - 5); // 5 pixels above the line
        
        // Draw bars based on dB scale (right Y-axis)
        // dB range: -80 dB (bottom) to 0 dB (top)
        // Map data to time: T-10s (left) to T-0s (right, now)
        ctx.globalAlpha = 0.5; // Constant transparency
        if (audioGraphData.length > 0) {
            const nowTime = Date.now();
            const tenSecondsAgo = nowTime - 10000; // 10 seconds ago
            const timeRange = 10000; // 10 seconds in milliseconds
            const barWidth = graphWidth / audioGraphMaxPoints;
            
            audioGraphData.forEach((dataPoint, index) => {
                // Handle both object format {db, clipping, timestamp} and legacy number format
                const dbValue = typeof dataPoint === 'object' ? dataPoint.db : dataPoint;
                const isClipping = typeof dataPoint === 'object' ? dataPoint.clipping : false;
                const timestamp = typeof dataPoint === 'object' ? (dataPoint.timestamp || 0) : 0;
                
                // Map timestamp to X position: older data on left, newer on right
                // Graph shows T-10s (left) to T-0s (right, now)
                let x;
                if (timestamp > 0) {
                    // Time-based positioning: map timestamp to graph position
                    const timeOffset = nowTime - timestamp; // How many ms ago
                    const timeRatio = Math.max(0, Math.min(1, timeOffset / timeRange)); // 0 = now, 1 = 10s ago
                    x = graphX + (graphWidth * (1 - timeRatio)); // Invert: older = left (timeRatio=1), newer = right (timeRatio=0)
                } else {
                    // Legacy: use index-based positioning (assume newest data is at end)
                    const indexFromEnd = audioGraphData.length - 1 - index;
                    x = graphX + (indexFromEnd * barWidth);
                }
                
                // Clamp dB value to display range (-80 to 0 dB)
                const clampedDb = Math.max(dbMin, Math.min(dbMax, dbValue));
                // Map dB to Y position: -80 dB (bottom) to 0 dB (top)
                const barY = graphY + graphHeight - ((clampedDb - dbMin) / (dbMax - dbMin)) * graphHeight;
                // Draw bar from bottom (graphY + graphHeight) to the dB position
                const barHeight = (graphY + graphHeight) - barY;
                
                // Only draw if bar height is positive (audio is above -80 dB) and within graph bounds
                if (barHeight > 0 && x >= graphX && x <= graphX + graphWidth) {
                    // Determine color based on threshold and clipping (constant color, no stale detection)
                    if (isClipping) {
                        ctx.fillStyle = '#FF0000'; // Red for clipping
                    } else if (dbValue >= thresholdDb) {
                        ctx.fillStyle = '#FF8C00'; // Orange for above threshold
                    } else {
                        ctx.fillStyle = '#D3D3D3'; // Light gray for below threshold
                    }
                    
                    ctx.fillRect(x, barY, barWidth - 2, barHeight);
                }
            });
        }
        // Reset transparency to fully opaque for other drawing operations
        ctx.globalAlpha = 1.0;
        
        // Draw left Y axis labels (Percentage)
        ctx.fillStyle = '#000000';
        ctx.font = window.innerWidth <= 480 ? '10px Arial' : '12px Arial';
        ctx.textAlign = 'right';
        ctx.textBaseline = 'middle';
        for (let i = 0; i <= 4; i++) {
            const percentage = 80 - (i * 20);
            const y = graphY + (graphHeight / 4) * i;
            ctx.fillText(percentage, graphX - 10, y);
        }
        
        // Draw right Y axis labels (Decibel)
        // Decibel range: -120 dB (bottom) to 0 dB (top) approximately
        // For display, we'll use a more practical range: -80 dB to 0 dB
        ctx.textAlign = 'left';
        for (let i = 0; i <= 4; i++) {
            const db = dbMax - (i * (dbMax - dbMin) / 4);
            const y = graphY + (graphHeight / 4) * i;
            ctx.fillText(db.toFixed(0) + ' dB', graphX + graphWidth + 10, y);
        }
        
        // Draw X axis labels below the graph
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText('-10s', graphX + 20, graphY + graphHeight + 10);
        ctx.fillText('Now', graphX + graphWidth - 20, graphY + graphHeight + 10);
        
        // Draw axis lines
        ctx.strokeStyle = '#000000';
        ctx.lineWidth = 1;
        // Left Y axis
        ctx.beginPath();
        ctx.moveTo(graphX, graphY);
        ctx.lineTo(graphX, graphY + graphHeight);
        ctx.stroke();
        // Right Y axis
        ctx.beginPath();
        ctx.moveTo(graphX + graphWidth, graphY);
        ctx.lineTo(graphX + graphWidth, graphY + graphHeight);
        ctx.stroke();
        // X axis
        ctx.beginPath();
        ctx.moveTo(graphX, graphY + graphHeight);
        ctx.lineTo(graphX + graphWidth, graphY + graphHeight);
        ctx.stroke();
    }
    
    // Always start graph interval if tab is visible (needed for both WebSocket and polling)
    if (isTabVisible) {
        if (audioGraphInterval) {
            clearInterval(audioGraphInterval); // Clear any existing interval
        }
        audioGraphInterval = setInterval(drawGraph, 100);
        console.log('Audio graph interval started, will draw every 100ms');
        // Draw immediately
        drawGraph();
    } else {
        console.log('Audio graph interval not started - tab not visible');
    }
}

// WebSocket update handler for audio data
function updateAudioDataFromWebSocket(data) {
    hideConnectionError();
    
    const recordingBtn = document.getElementById('recording-btn');
    const idleBtn = document.getElementById('idle-btn');
    
    // Handle both new format (with samples array) and legacy format (single sample)
    const hasSamples = data.samples && Array.isArray(data.samples) && data.samples.length > 0;
    const latestSample = hasSamples ? data.samples[data.samples.length - 1] : data;
    
    if (latestSample.isRecording !== undefined ? latestSample.isRecording : data.isRecording) {
        if (recordingBtn) recordingBtn.style.display = 'inline-block';
        if (idleBtn) idleBtn.style.display = 'none';
    } else {
        if (recordingBtn) recordingBtn.style.display = 'none';
        if (idleBtn) idleBtn.style.display = 'inline-block';
    }
    
    // Update UI with latest sample values
    if (document.getElementById('current-level')) {
        const currentLevel = latestSample.currentLevel !== undefined ? latestSample.currentLevel : data.currentLevel;
        const currentDb = latestSample.currentDb !== undefined ? latestSample.currentDb : data.currentDb;
        document.getElementById('current-level').textContent = 
            currentLevel.toFixed(1) + ' / ' + currentDb.toFixed(1) + ' dB';
    }
    if (document.getElementById('min-level')) {
        document.getElementById('min-level').textContent = 
            data.minLevel.toFixed(1) + ' / ' + data.minDb.toFixed(1) + ' dB';
    }
    if (document.getElementById('max-level')) {
        document.getElementById('max-level').textContent = 
            data.maxLevel.toFixed(1) + ' / ' + data.maxDb.toFixed(1) + ' dB';
    }
    
    // Add samples to buffer (with 1 second delay before displaying)
    const now = Date.now();
    const sampleInterval = 100; // 100ms between samples
    
    if (hasSamples) {
        // New format: add all samples to buffer with 1 second delay
        data.samples.forEach((sample, index) => {
            const isClipping = sample.dynamicRangeUtil >= 100;
            // Calculate when this sample should be displayed (1 second from now)
            const displayTimestamp = now + AUDIO_GRAPH_DELAY_MS - ((data.samples.length - 1 - index) * sampleInterval);
            audioGraphBuffer.push({ 
                db: sample.currentDb, 
                clipping: isClipping, 
                timestamp: displayTimestamp 
            });
        });
    } else {
        // Legacy format: single sample
        const isClipping = data.dynamicRangeUtil >= 100;
        const displayTimestamp = now + AUDIO_GRAPH_DELAY_MS;
        audioGraphBuffer.push({ db: data.currentDb, clipping: isClipping, timestamp: displayTimestamp });
    }
    
    // Start the interval to add samples from buffer to graph if not already running
    // This runs continuously to smoothly add samples one at a time
    if (!audioGraphBufferInterval) {
        audioGraphBufferInterval = setInterval(function() {
            if (document.hidden) return;

            const currentTime = Date.now();
            // Add one sample from buffer that is ready to be displayed (smooth addition)
            if (audioGraphBuffer.length > 0 && audioGraphBuffer[0].timestamp <= currentTime) {
                const sample = audioGraphBuffer.shift();
                // Use actual display time for X-axis mapping to "Now"; releaseTime was only for buffer gating
                audioGraphData.push({ db: sample.db, clipping: sample.clipping, timestamp: currentTime });
            }
            
            // Keep only the last 10 seconds of displayed data
            if (audioGraphData.length > audioGraphMaxPoints) {
                audioGraphData.shift();
            }
            
            // Remove data points older than 10 seconds
            const tenSecondsAgo = currentTime - 10000;
            while (audioGraphData.length > 0 && audioGraphData[0].timestamp < tenSecondsAgo) {
                audioGraphData.shift();
            }
        }, AUDIO_SAMPLE_ADD_INTERVAL);
    }
    
    console.log('Audio samples buffered, buffer length:', audioGraphBuffer.length, 'graph length:', audioGraphData.length, 'samples received:', hasSamples ? data.samples.length : 1);
}

async function loadAudioData() {
    // Don't poll if tab is not visible
    if (!isTabVisible || pollingPaused) {
        return;
    }
    
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/audio/stats', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError(); // Connection successful
        
        // Use same update logic as WebSocket handler
        updateAudioDataFromWebSocket(data);
    } catch (error) {
        console.error('Error loading audio data:', error);
        if (error.name === 'AbortError' || error.message.includes('Failed to fetch')) {
            showConnectionError();
            stopAllPolling();
        }
    }
}

// Network Page
let networkConfigInitialized = false;

function initNetworkPage() {
    if (connectionLost) {
        hideLoadingPopup();
        return;
    }
    networkConfigInitialized = false; // Reset on page init
    
    // Common Save/Cancel for Network page
    const networkSaveBtn = document.getElementById('network-save-btn');
    const networkCancelBtn = document.getElementById('network-cancel-btn');
    const wifiConfigsContainer = document.getElementById('wifi-configs');
    function setNetworkDirty() {
        if (networkSaveBtn) networkSaveBtn.disabled = false;
    }
    function clearNetworkDirty() {
        if (networkSaveBtn) networkSaveBtn.disabled = true;
    }
    if (networkCancelBtn) {
        networkCancelBtn.addEventListener('click', async function() {
            await loadNetworkConfig();
            clearNetworkDirty();
        });
    }
    if (networkSaveBtn) {
        networkSaveBtn.addEventListener('click', async function() {
            if (!await showConfirmModal('Save all WiFi network settings?', 'Network Settings')) return;
            let ok = true;
            for (let i = 0; i < 3; i++) {
                const result = await saveWiFiConfig(i, true);
                if (!result) ok = false;
            }
            if (ok) {
                clearNetworkDirty();
                await showInfoModal('All network settings saved.', 'Network Settings');
            } else {
                await showInfoModal('Some settings could not be saved. Please try again.', 'Network Settings');
            }
        });
    }
    if (wifiConfigsContainer) {
        wifiConfigsContainer.addEventListener('change', setNetworkDirty);
        wifiConfigsContainer.addEventListener('input', setNetworkDirty);
    }
    clearNetworkDirty();
    
    // Subscribe to WebSocket if connected, otherwise use polling
    if (wsConnected && useWebSocket) {
        subscribeToPage('network');
        hideLoadingPopup();
        // Load initial data once
        loadNetworkConfig();
    } else {
        loadNetworkConfig().then(() => {
            hideLoadingPopup();
            if (!connectionLost && !networkDataInterval && isTabVisible && !useWebSocket) {
                // Poll every second for network config changes
                networkDataInterval = setInterval(loadNetworkConfig, 1000);
            }
        });
    }
}

// WebSocket update handler for network config
function updateNetworkDataFromWebSocket(data) {
    hideConnectionError();
    
    const container = document.getElementById('wifi-configs');
    if (!container) {
        console.error('WiFi configs container not found');
        return;
    }
    
    // Only skip UI update if data hasn't changed AND we've already initialized
    if (data.changed === false && networkConfigInitialized) {
        return; // Nothing changed, skip UI update
    }
    
    // Mark as initialized after first successful load
    networkConfigInitialized = true;
    
    // Reuse the same rendering logic as loadNetworkConfig
    container.innerHTML = '';
    
    let configs = data.wifiConfigs || [];
    
    // Ensure we always have exactly 3 configs
    while (configs.length < 3) {
        configs.push({
            ssid: '',
            password: '',
            staticIp: '',
            subnet: '',
            gateway: '',
            staticIpEnabled: false
        });
    }
    
    // Only show the first 3 configs (in case API returns more)
    configs.slice(0, 3).forEach((config, index) => {
        const staticIpEnabled = config.staticIpEnabled || false;
        const card = document.createElement('div');
        card.className = 'wifi-config-card';
        card.innerHTML = `
            <form class="wifi-config-form" data-index="${index}">
                <h3>WiFi Network ${index + 1}</h3>
                <div class="wifi-credentials-row">
                    <div class="form-group wifi-ssid-group">
                        <label>SSID:</label>
                        <input type="text" class="form-control wifi-ssid" value="${config.ssid || ''}" data-index="${index}">
                    </div>
                    <div class="form-group wifi-password-group">
                        <label>Password:</label>
                        <input type="password" class="form-control wifi-password" value="${config.password || ''}" data-index="${index}">
                    </div>
                </div>
                <div class="form-group">
                    <label class="checkbox-label">
                        <input type="checkbox" class="wifi-static-ip-enabled" ${staticIpEnabled ? 'checked' : ''} data-index="${index}">
                        <span>Use Static IP</span>
                    </label>
                </div>
                <div class="static-ip-fields" style="display: ${staticIpEnabled ? 'block' : 'none'};" data-index="${index}">
                    <div class="form-group">
                        <label>Static IP:</label>
                        <input type="text" class="form-control wifi-static-ip" value="${config.staticIp || ''}" placeholder="192.168.1.100" data-index="${index}">
                    </div>
                    <div class="form-group">
                        <label>Subnet Mask:</label>
                        <input type="text" class="form-control wifi-subnet" value="${config.subnet || ''}" placeholder="255.255.255.0" data-index="${index}">
                    </div>
                    <div class="form-group">
                        <label>Gateway:</label>
                        <input type="text" class="form-control wifi-gateway" value="${config.gateway || ''}" placeholder="192.168.1.1" data-index="${index}">
                    </div>
                </div>
            </form>
        `;
        container.appendChild(card);
    });
    
    document.querySelectorAll('.wifi-static-ip-enabled').forEach(checkbox => {
        checkbox.addEventListener('change', function() {
            const index = parseInt(this.getAttribute('data-index'));
            const staticIpFields = document.querySelector(`.static-ip-fields[data-index="${index}"]`);
            if (staticIpFields) {
                staticIpFields.style.display = this.checked ? 'block' : 'none';
            }
        });
    });
    
    // After re-rendering, clear dirty so Save is disabled until user edits
    const networkSaveBtn = document.getElementById('network-save-btn');
    if (networkSaveBtn) networkSaveBtn.disabled = true;
}

async function loadNetworkConfig() {
    // Don't poll if tab is not visible
    if (!isTabVisible || pollingPaused) {
        return;
    }
    
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/network/config', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError(); // Connection successful
        
        // Use same update logic as WebSocket handler
        updateNetworkDataFromWebSocket(data);
        
        // The rest of the rendering is now in updateNetworkDataFromWebSocket
        return;
        
        // Old code below (kept for reference but should not execute)
        const container = document.getElementById('wifi-configs');
        if (!container) {
            console.error('WiFi configs container not found');
            return;
        }
        
        // Only skip UI update if data hasn't changed AND we've already initialized
        if (data.changed === false && networkConfigInitialized) {
            return; // Nothing changed, skip UI update
        }
        
        // Mark as initialized after first successful load
        networkConfigInitialized = true;
        
        container.innerHTML = '';
        
        // Always show 3 network configuration groups, even if empty
        // If API doesn't return wifiConfigs or returns empty array, create 3 empty configs
        let configs = data.wifiConfigs || [];
        
        // Ensure we always have exactly 3 configs
        while (configs.length < 3) {
            configs.push({
                ssid: '',
                password: '',
                staticIp: '',
                subnet: '',
                gateway: '',
                staticIpEnabled: false
            });
        }
        
        // Only show the first 3 configs (in case API returns more)
        configs.slice(0, 3).forEach((config, index) => {
            const staticIpEnabled = config.staticIpEnabled || false;
            const card = document.createElement('div');
            card.className = 'wifi-config-card';
            card.innerHTML = `
                <form class="wifi-config-form" data-index="${index}">
                    <h3>WiFi Network ${index + 1}</h3>
                    <div class="wifi-credentials-row">
                        <div class="form-group wifi-ssid-group">
                            <label>
                                SSID
                                <span class="help-icon" data-help-key="net.wifiSsid">🛈</span>
                            </label>
                            <input type="text" class="form-control wifi-ssid" value="${config.ssid || ''}" data-index="${index}" autocomplete="username">
                        </div>
                        <div class="form-group wifi-password-group">
                            <label>
                                Password
                                <span class="help-icon" data-help-key="net.wifiPassword">🛈</span>
                            </label>
                            <input type="password" class="form-control wifi-password" value="${config.password || ''}" data-index="${index}" autocomplete="current-password">
                        </div>
                    </div>
                    <div class="form-group">
                        <label class="checkbox-label">
                            <input type="checkbox" class="wifi-static-ip-enabled" ${staticIpEnabled ? 'checked' : ''} data-index="${index}">
                            <span>Use Static IP</span>
                            <span class="help-icon-inline" data-help-key="net.staticIpEnabled">🛈</span>
                        </label>
                    </div>
                    <div class="static-ip-fields" style="display: ${staticIpEnabled ? 'block' : 'none'};" data-index="${index}">
                        <div class="form-group">
                            <label>
                                Static IP
                                <span class="help-icon" data-help-key="net.staticIp">🛈</span>
                            </label>
                            <input type="text" class="form-control wifi-static-ip" value="${config.staticIp || ''}" placeholder="192.168.1.100" data-index="${index}">
                        </div>
                        <div class="form-group">
                            <label>
                                Subnet Mask
                                <span class="help-icon" data-help-key="net.subnet">🛈</span>
                            </label>
                            <input type="text" class="form-control wifi-subnet" value="${config.subnet || ''}" placeholder="255.255.255.0" data-index="${index}">
                        </div>
                        <div class="form-group">
                            <label>
                                Gateway
                                <span class="help-icon" data-help-key="net.gateway">🛈</span>
                            </label>
                            <input type="text" class="form-control wifi-gateway" value="${config.gateway || ''}" placeholder="192.168.1.1" data-index="${index}">
                        </div>
                    </div>
                </form>
            `;
            container.appendChild(card);
        });
        
        // Prevent form submission
        document.querySelectorAll('.wifi-config-form').forEach(form => {
            form.addEventListener('submit', function(e) {
                e.preventDefault();
            });
        });
        
        // Add static IP toggle listeners
        document.querySelectorAll('.wifi-static-ip-enabled').forEach(checkbox => {
            checkbox.addEventListener('change', function() {
                const index = parseInt(this.getAttribute('data-index'));
                const staticIpFields = document.querySelector(`.static-ip-fields[data-index="${index}"]`);
                if (staticIpFields) {
                    staticIpFields.style.display = this.checked ? 'block' : 'none';
                }
            });
        });

        // Attach help handlers for dynamically created help icons
        initHelpIcons();
    } catch (error) {
        console.error('Error loading network config:', error);
        showConnectionError();
        hideLoadingPopup();
    }
}

async function saveWiFiConfig(index, skipConfirm) {
    const card = document.querySelector(`.wifi-config-card:nth-child(${index + 1})`);
    if (!card) return false;
    const ssid = card.querySelector('.wifi-ssid').value;
    const password = card.querySelector('.wifi-password').value;
    const staticIpEnabled = card.querySelector('.wifi-static-ip-enabled').checked;
    const staticIp = staticIpEnabled ? card.querySelector('.wifi-static-ip').value : '';
    const subnet = staticIpEnabled ? card.querySelector('.wifi-subnet').value : '';
    const gateway = staticIpEnabled ? card.querySelector('.wifi-gateway').value : '';
    
    try {
        const response = await fetch('/api/network/save', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                index: index,
                ssid: ssid,
                password: password,
                staticIpEnabled: staticIpEnabled,
                staticIp: staticIp,
                subnet: subnet,
                gateway: gateway
            })
        });
        const data = await response.json();
        if (data.success) {
            if (!skipConfirm) await showInfoModal('WiFi configuration saved successfully', 'Network Settings');
            return true;
        }
        if (!skipConfirm) await showInfoModal('Failed to save: ' + (data.message || 'Unknown error'), 'Network Settings');
        return false;
    } catch (error) {
        console.error('Error saving WiFi config:', error);
        if (!skipConfirm) await showInfoModal('Error saving WiFi configuration', 'Network Settings');
        return false;
    }
}

// Advanced Page
function initAdvancedPage() {
    hideLoadingPopup();
    renderThemeControls();
    const updateBtn = document.getElementById('update-firmware-btn');
    if (updateBtn) {
        updateBtn.addEventListener('click', updateFirmware);
    }
    const firmwareFileInput = document.getElementById('firmware-file-input');
    if (firmwareFileInput) {
        firmwareFileInput.addEventListener('change', function() {
            const file = this.files[0];
            if (file) {
                if (!file.name.endsWith('.bin')) {
                    showInfoModal('Please select a .bin file', 'Firmware Update');
                    this.value = '';
                    return;
                }
                const fileSizeMB = (file.size / (1024 * 1024)).toFixed(2);
                console.log('Selected firmware file: ' + file.name + ' (' + fileSizeMB + ' MB)');
            }
        });
    }
    const rebootBtn = document.getElementById('reboot-btn');
    if (rebootBtn) {
        rebootBtn.addEventListener('click', rebootDevice);
    }
    const setDefaultBtn = document.getElementById('set-default-btn');
    if (setDefaultBtn) {
        setDefaultBtn.addEventListener('click', setDefaultSettings);
    }
    const factoryResetBtn = document.getElementById('factory-reset-btn');
    if (factoryResetBtn) {
        factoryResetBtn.addEventListener('click', factoryReset);
    }
    const exportBtn = document.getElementById('export-settings-btn');
    if (exportBtn) {
        exportBtn.addEventListener('click', exportSettings);
    }
    const importBtn = document.getElementById('import-settings-btn');
    if (importBtn) {
        importBtn.addEventListener('click', () => {
            document.getElementById('settings-file-input').click();
        });
    }
    const fileInput = document.getElementById('settings-file-input');
    if (fileInput) {
        fileInput.addEventListener('change', importSettings);
    }
    
    // Common Save/Cancel for Advanced page
    const advancedSaveBtn = document.getElementById('advanced-save-btn');
    const advancedCancelBtn = document.getElementById('advanced-cancel-btn');
    function setAdvancedDirty() {
        if (advancedSaveBtn) advancedSaveBtn.disabled = false;
    }
    function clearAdvancedDirty() {
        if (advancedSaveBtn) advancedSaveBtn.disabled = true;
    }
    if (advancedCancelBtn) {
        advancedCancelBtn.addEventListener('click', async function() {
            await loadSdCardSettings();
            await loadTimezoneFromServer();
            await loadWifiTxPowerSettings();
            await loadUploadLocations();
            clearAdvancedDirty();
        });
    }
    if (advancedSaveBtn) {
        advancedSaveBtn.addEventListener('click', async function() {
            if (!await showConfirmModal('Save all changes to SD card, timezone, WiFi Tx Power, and upload locations?', 'Save Advanced Settings')) return;
            let ok = true;
            ok = await saveSdCardSettings(true) && ok;
            ok = await saveTimezoneSettings(true) && ok;
            ok = await saveWifiTxPowerSettings(true) && ok;
            ok = await saveUploadLocations(true) && ok;
            if (ok) {
                clearAdvancedDirty();
                await showInfoModal('All advanced settings saved.', 'Advanced Settings');
            } else {
                await showInfoModal('Some settings could not be saved. Check the values and try again.', 'Advanced Settings');
            }
        });
    }

    // Add SD card settings: mark dirty on change
    const testSdCardBtn = document.getElementById('test-sd-card-btn');
    if (testSdCardBtn) {
        testSdCardBtn.addEventListener('click', testSdCard);
    }

    ['sd-card-enabled', 'record-to-sd-card', 'sd-card-mode-select', 'sd-card-format-if-mount-failed'].forEach(function(id) {
        const el = document.getElementById(id);
        if (el) el.addEventListener('change', setAdvancedDirty);
    });

    // Load SD card settings when page initializes
    loadSdCardSettings();

    // Upload Locations
    loadUploadLocations();
    const uploadUseCustomHostCheckbox = document.getElementById('upload-use-custom-host');
    const uploadRegionOhio = document.getElementById('upload-region-ohio');
    const uploadRegionOregon = document.getElementById('upload-region-oregon');
    const uploadRegionVirginia = document.getElementById('upload-region-virginia');
    function toggleCustomHostFields(show) {
        const el = document.getElementById('upload-custom-host-fields');
        if (el) el.style.display = show ? 'flex' : 'none';
    }
    if (uploadUseCustomHostCheckbox) {
        toggleCustomHostFields(uploadUseCustomHostCheckbox.checked);
        uploadUseCustomHostCheckbox.addEventListener('change', function() {
            const useCustom = this.checked;
            toggleCustomHostFields(useCustom);
            [uploadRegionOhio, uploadRegionOregon, uploadRegionVirginia].forEach(function(cb) {
                if (cb) {
                    cb.disabled = useCustom;
                    if (useCustom) {
                        cb.checked = false;
                    } else {
                        cb.checked = true;
                    }
                }
            });
            setAdvancedDirty();
        });
    }
    ['upload-region-ohio', 'upload-region-oregon', 'upload-region-virginia', 'upload-use-custom-host', 'upload-custom-host', 'upload-custom-port'].forEach(function(id) {
        const el = document.getElementById(id);
        if (el) el.addEventListener('change', setAdvancedDirty);
    });
    const uploadCustomHost = document.getElementById('upload-custom-host');
    const uploadCustomPort = document.getElementById('upload-custom-port');
    if (uploadCustomHost) uploadCustomHost.addEventListener('input', setAdvancedDirty);
    if (uploadCustomPort) uploadCustomPort.addEventListener('input', setAdvancedDirty);
    [0, 1, 2, 3].forEach(function(i) {
        const testBtn = document.getElementById('test-upload-region-' + i);
        if (testBtn) {
            testBtn.addEventListener('click', function() {
                if (i === 3) testUploadHost();
                else testUploadRegion(i);
            });
        }
    });
    
    // WiFi Tx Power settings
    const wifiTxPowerSlider = document.getElementById('wifi-tx-power');
    const wifiTxPowerValue = document.getElementById('wifi-tx-power-value');
    const localHostnameInput = document.getElementById('local-hostname');
    if(echoDevice) {
        const mqttKeyInput = document.getElementById('mqtt-key');
        const mqttKeyClear = document.getElementById('mqtt-key-clear');
        if (mqttKeyInput) mqttKeyInput.addEventListener('input', setAdvancedDirty);
        if (mqttKeyClear) mqttKeyClear.addEventListener('change', setAdvancedDirty);
    }else {
        const mqttKeyInput = document.getElementById('mqtt-key');
        if (mqttKeyInput) {
            const mqttGroup = mqttKeyInput.closest('.form-group');
            if (mqttGroup) {
                mqttGroup.style.display = 'none';
            }
        }
    }
    if (localHostnameInput) localHostnameInput.addEventListener('input', setAdvancedDirty);
    if (wifiTxPowerSlider && wifiTxPowerValue) {
        wifiTxPowerSlider.addEventListener('input', function() {
            wifiTxPowerValue.textContent = this.value;
            setAdvancedDirty();
        });
    }
    loadWifiTxPowerSettings();

    // Timezone controls: mark dirty on change
    const timezoneSelect = document.getElementById('timezone-select');
    if (timezoneSelect) timezoneSelect.addEventListener('change', setAdvancedDirty);
    initTimezoneControls();

    // After all loads, ensure Save is disabled until user changes something
    clearAdvancedDirty();

    // Toggle SD extra settings visibility when enable checkbox changes
    const sdEnableCheckbox = document.getElementById('sd-card-enabled');
    if (sdEnableCheckbox) {
        sdEnableCheckbox.addEventListener('change', function() {
            const extra = document.getElementById('sd-card-extra-settings');
            if (extra) {
                extra.style.display = this.checked ? 'block' : 'none';
            }
        });
    }
}

async function updateFirmware() {
    const fileInput = document.getElementById('firmware-file-input');
    if (!fileInput || !fileInput.files || !fileInput.files[0]) {
        await showInfoModal('Please select a firmware file', 'Firmware Update');
        return;
    }
    
    const file = fileInput.files[0];
    if (!file.name.endsWith('.bin')) {
        await showInfoModal('Please select a .bin file', 'Firmware Update');
        return;
    }
    
    const fileSizeMB = (file.size / (1024 * 1024)).toFixed(2);
    if (!await showConfirmModal('Upload and update firmware with ' + file.name + ' (' + fileSizeMB + ' MB)?\n\nThis will reboot the device.', 'Upload Firmware')) {
        return;
    }
    
    const updateBtn = document.getElementById('update-firmware-btn');
    const progressDiv = document.getElementById('firmware-progress');
    const progressBar = document.getElementById('firmware-progress-bar');
    const progressText = document.getElementById('firmware-progress-text');
    
    updateBtn.disabled = true;
    updateBtn.textContent = '\u23F3 Uploading...';
    progressDiv.style.display = 'block';
    progressBar.style.width = '0%';
    progressText.textContent = '0%';
    
    try {
        const formData = new FormData();
        formData.append('firmware', file);
        
        const xhr = new XMLHttpRequest();
        
        xhr.upload.addEventListener('progress', function(e) {
            if (e.lengthComputable) {
                const percentComplete = (e.loaded / e.total) * 100;
                progressBar.style.width = percentComplete + '%';
                progressText.textContent = Math.round(percentComplete) + '%';
            }
        });
        
        xhr.addEventListener('load', function() {
            if (xhr.status === 200) {
                try {
                    const data = JSON.parse(xhr.responseText);
                    if (jsonApiOk(data)) {
                        progressBar.style.width = '100%';
                        progressText.textContent = '100% - Flashing firmware...';
                        updateBtn.textContent = '\uD83D\uDD04 Update Complete - Rebooting...';
                        showRebootCountdown();
                    } else {
                        throw new Error(data.message || 'Update failed');
                    }
                } catch (e) {
                    showInfoModal('Failed to update firmware: ' + (e.message || 'Unknown error'), 'Firmware Update Failed');
                    updateBtn.disabled = false;
                    updateBtn.textContent = '\uD83D\uDCE4 Upload & Update Firmware';
                    progressDiv.style.display = 'none';
                }
            } else {
                throw new Error('HTTP ' + xhr.status);
            }
        });
        
        xhr.addEventListener('error', function() {
            showInfoModal('Error uploading firmware file', 'Firmware Update Error');
            updateBtn.disabled = false;
            updateBtn.textContent = '\uD83D\uDCE4 Upload & Update Firmware';
            progressDiv.style.display = 'none';
        });
        
        xhr.open('POST', '/api/advanced/update-firmware');
        xhr.send(formData);
        
    } catch (error) {
        console.error('Error updating firmware:', error);
        await showInfoModal('Error updating firmware: ' + error.message, 'Firmware Update Error');
        updateBtn.disabled = false;
        updateBtn.textContent = '\uD83D\uDCE4 Upload & Update Firmware';
        progressDiv.style.display = 'none';
    }
}

async function rebootDevice() {
    if (!await showConfirmModal('Reboot the device?', 'Reboot Device')) {
        return;
    }
    try {
        const response = await fetch('/api/advanced/reboot', {method: 'POST'});
        const data = await response.json();
        if (response.ok && jsonApiOk(data)) {
            await showInfoModal('Device rebooting...', 'Reboot Device');
        }
    } catch (error) {
        console.error('Error rebooting:', error);
    }
}

async function setDefaultSettings() {
    if (!await showConfirmModal('Set Default will reset all settings to default values, but keep WiFi settings. Continue?', 'Set Default')) {
        return;
    }
    if (!await showConfirmModal('Are you sure? This will reset all audio, upload, RTC, SD card, timezone, and log settings to defaults. WiFi settings will be preserved.', 'Set Default')) {
        return;
    }
    try {
        const response = await fetch('/api/advanced/set-default', {method: 'POST'});
        const data = await response.json();
        if (response.ok && jsonApiOk(data)) {
            await showInfoModal(data.message || 'Settings reset to defaults. Device will reboot.', 'Set Default');
        } else {
            await showInfoModal('Failed to set defaults: ' + (data.message || 'Unknown error'), 'Set Default');
        }
    } catch (error) {
        console.error('Error setting defaults:', error);
        await showInfoModal('Error setting defaults', 'Set Default');
    }
}

async function factoryReset() {
    if (!await showConfirmModal('Factory reset will erase all settings. Continue?', 'Factory Reset')) {
        return;
    }
    if (!await showConfirmModal('Are you absolutely sure? This cannot be undone.', 'Factory Reset')) {
        return;
    }
    try {
        const response = await fetch('/api/advanced/factory-reset', {method: 'POST'});
        const data = await response.json();
        if (response.ok && jsonApiOk(data)) {
            await showInfoModal(data.message || 'Factory reset complete. Device will reboot.', 'Factory Reset');
        } else {
            await showInfoModal('Failed to factory reset: ' + (data.message || 'Unknown error'), 'Factory Reset');
        }
    } catch (error) {
        console.error('Error factory resetting:', error);
        await showInfoModal('Error performing factory reset', 'Factory Reset');
    }
}

async function loadSdCardSettings() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/advanced/sd-card-settings', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError();
        
        if (data.useSdCard !== undefined) {
            const checkbox = document.getElementById('sd-card-enabled');
            if (checkbox) checkbox.checked = data.useSdCard;
            const extra = document.getElementById('sd-card-extra-settings');
            if (extra) extra.style.display = data.useSdCard ? 'block' : 'none';
        }
        
        if (data.recordToSdCard !== undefined) {
            const checkbox = document.getElementById('record-to-sd-card');
            if (checkbox) checkbox.checked = data.recordToSdCard;
        }
        
        // Map current mode1bit + frequency into preset for dropdown
        if (data.mode1bit !== undefined && data.frequency !== undefined) {
            const select = document.getElementById('sd-card-mode-select');
            if (select) {
                const mode1bit = !!data.mode1bit;
                const freq = data.frequency;
                let preset = 'medium'; // sensible default
                if (mode1bit && freq === 5000000) {
                    preset = 'sloth';
                } else if (mode1bit && freq === 10000000) {
                    preset = 'slow';
                } else if (!mode1bit && freq === 5000000) {
                    preset = 'medium';
                } else if (!mode1bit && freq === 10000000) {
                    preset = 'fast';
                } else if (!mode1bit && freq === 20000000) {
                    preset = 'insane';
                }
                select.value = preset;
            }
        }
        
        if (data.formatIfMountFailed !== undefined) {
            const checkbox = document.getElementById('sd-card-format-if-mount-failed');
            if (checkbox) checkbox.checked = data.formatIfMountFailed;
        }
    } catch (error) {
        console.error('Error loading SD card settings:', error);
    }
}

async function loadWifiTxPowerSettings() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/advanced/wifi-tx-power-settings', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        
        hideConnectionError();
        
        if (data.wifiTxPower !== undefined) {
            const slider = document.getElementById('wifi-tx-power');
            const valueDisplay = document.getElementById('wifi-tx-power-value');
            if (slider) {
                slider.value = data.wifiTxPower;
            }
            if (valueDisplay) {
                valueDisplay.textContent = data.wifiTxPower;
            }
        }
        if (data.hostname !== undefined) {
            const hostnameInput = document.getElementById('local-hostname');
            if (hostnameInput) hostnameInput.value = data.hostname;
        }
        const mqttKeyStatus = document.getElementById('mqtt-key-status');
        if (mqttKeyStatus && data.mqttKeyConfigured) {
            mqttKeyStatus.textContent = 'An MQTT key is configured. Leave blank to keep it unchanged; enter a new value to replace it. A reboot is required.';
        }
    } catch (error) {
        console.error('Error loading WiFi Tx Power settings:', error);
    }
}

async function saveWifiTxPowerSettings(skipConfirm) {
    if (connectionLost) return false;
    
    const txPowerInput = document.getElementById('wifi-tx-power');
    const hostnameInput = document.getElementById('local-hostname');
    const mqttKeyInput = document.getElementById('mqtt-key');
    const mqttKeyClear = document.getElementById('mqtt-key-clear');
    if (!txPowerInput) return;
    
    const txPower = parseInt(txPowerInput.value, 10);
    if (isNaN(txPower) || txPower < 1 || txPower > 10) {
        if (!skipConfirm) await showInfoModal('WiFi Tx Power must be between 1 and 10', 'WiFi Tx Power');
        return false;
    }
    
    if (!skipConfirm && !await showConfirmModal('Save WiFi Tx Power setting? The device may need to reconnect for changes to take effect.', 'WiFi Tx Power')) {
        return;
    }
    
    try {
        // TO-DO Update to avoid the multiple fetch
        const hostname = hostnameInput?.value.trim().toLowerCase();
        if (!hostname || !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(hostname)) {
            if (!skipConfirm) await showInfoModal('Hostname must contain 1-63 letters, numbers, or interior hyphens.', 'Local Hostname');
            return false;
        }
        const hostnameResponse = await fetchWithRetry('/api/audio/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({param: 'hostname', value: hostname})
        });
        if (!hostnameResponse.ok || !(await hostnameResponse.json()).success) throw new Error('Hostname response was not ok');

        const mqttKey = mqttKeyInput?.value || '';
        if (mqttKey.length > 0 || mqttKeyClear?.checked) {
            const mqttResponse = await fetchWithRetry('/api/audio/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({param: 'mqtt.key', value: mqttKey})
            });
            if (!mqttResponse.ok || !(await mqttResponse.json()).success) throw new Error('MQTT key response was not ok');
            mqttKeyInput.value = '';
            if (mqttKeyClear) mqttKeyClear.checked = false;
        }

        const response = await fetchWithRetry('/api/audio/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({param: 'txpower', value: txPower.toString()})
        });
        
        if (!response.ok) throw new Error('Network response was not ok');
        
        const data = await response.json();
        if (data.success) {
            if (!skipConfirm) await showInfoModal('WiFi Tx Power setting saved successfully.', 'WiFi Tx Power');
            return true;
        }
        if (!skipConfirm) await showInfoModal('Failed to save WiFi Tx Power setting: ' + (data.message || 'Unknown error'), 'WiFi Tx Power');
        return false;
    } catch (error) {
        console.error('Error saving WiFi Tx Power setting:', error);
        if (!skipConfirm) await showInfoModal('Error saving WiFi Tx Power setting: ' + error.message, 'WiFi Tx Power');
        return false;
    }
}

async function saveSdCardSettings(skipConfirm) {
    if (connectionLost) return false;
    
    const useSdCard = document.getElementById('sd-card-enabled')?.checked || false;
    const recordToSdCard = document.getElementById('record-to-sd-card')?.checked || false;
    const modeSelect = document.getElementById('sd-card-mode-select');
    let mode1bit = false;
    let frequency = 10000000;
    if (modeSelect) {
        switch (modeSelect.value) {
            case 'sloth': // 1-bit, 5 MHz
                mode1bit = true;
                frequency = 5000000;
                break;
            case 'slow': // 1-bit, 10 MHz
                mode1bit = true;
                frequency = 10000000;
                break;
            case 'medium': // 4-bit, 5 MHz
                mode1bit = false;
                frequency = 5000000;
                break;
            case 'fast': // 4-bit, 10 MHz
                mode1bit = false;
                frequency = 10000000;
                break;
            case 'insane': // 4-bit, 20 MHz
                mode1bit = false;
                frequency = 20000000;
                break;
            default:
                mode1bit = false;
                frequency = 10000000;
                break;
        }
    }

    const formatIfMountFailed = document.getElementById('sd-card-format-if-mount-failed')?.checked || false;
    
    if (!skipConfirm && !await showConfirmModal('Save SD card settings? The device may need to reboot for changes to take effect.', 'SD Card Settings')) {
        return false;
    }
    
    try {
        // Save each setting individually using the existing settings API
        const settings = [
            { param: 'sdcard.usesdcard', value: useSdCard ? 'true' : 'false' },
            { param: 'sdcard.recordtosdcard', value: recordToSdCard ? 'true' : 'false' },
            { param: 'sdcard.mode1bit', value: mode1bit ? 'true' : 'false' },
            { param: 'sdcard.frequency', value: frequency.toString() },
            { param: 'sdcard.formatifmountfailed', value: formatIfMountFailed ? 'true' : 'false' }
        ];
        
        let allSuccess = true;
        for (const setting of settings) {
            const response = await fetchWithRetry('/api/audio/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({param: setting.param, value: setting.value})
            });
            
            if (!response.ok) {
                allSuccess = false;
                break;
            }
            
            const data = await response.json();
            if (!data.success) {
                allSuccess = false;
                break;
            }
        }
        
        if (allSuccess) {
            if (!skipConfirm) await showInfoModal('SD card settings saved successfully. Device will reboot to apply changes.', 'SD Card Settings');
            if (!skipConfirm) setTimeout(() => { rebootDevice(); }, 2000);
            return true;
        }
        if (!skipConfirm) await showInfoModal('Failed to save some SD card settings. Please try again.', 'SD Card Settings');
        return false;
    } catch (error) {
        console.error('Error saving SD card settings:', error);
        if (!skipConfirm) await showInfoModal('Error saving SD card settings: ' + error.message, 'SD Card Settings');
        return false;
    }
}

async function testSdCard() {
    if (connectionLost) return;

    try {
        const response = await fetchWithRetry('/api/advanced/sd-card-test', {
            method: 'POST'
        }, 10000, 1);

        const data = await response.json().catch(() => ({}));

        if (!response.ok || !data || !data.success) {
            const msg = (data && data.message) ? data.message : 'SD card test failed or SD card is not available.';
            await showInfoModal('SD card test failed: ' + msg, 'SD Card Test');
            return;
        }

        const parts = [];
        if (data.mounted) {
            parts.push('Status: Mounted');
        } else {
            parts.push('Status: Not mounted');
        }
        if (data.mode) {
            const freq = data.frequencyHz || 0;
            parts.push('Mode: ' + data.mode + (freq ? (' @ ' + freq + ' Hz') : ''));
        }
        if (typeof data.totalMB === 'number') {
            parts.push('Total: ' + data.totalMB.toFixed(1) + ' MB');
        }
        if (typeof data.usedMB === 'number' && typeof data.freeMB === 'number') {
            parts.push('Used: ' + data.usedMB.toFixed(1) + ' MB');
            parts.push('Free: ' + data.freeMB.toFixed(1) + ' MB');
        }
        if (data.cardType) {
            parts.push('Card: ' + data.cardType);
        }

        const message = parts.join('\n');
        await showInfoModal(message || 'SD card test completed.', 'SD Card Test');
    } catch (error) {
        console.error('Error testing SD card:', error);
        await showInfoModal('Error testing SD card: ' + error.message, 'SD Card Test');
    }
}

let uploadLocationsCache = null;

async function loadUploadLocations() {
    if (connectionLost) return;
    try {
        const response = await fetchWithRetry('/api/advanced/upload-locations', {}, 8000, 2);
        if (!response.ok) throw new Error('Network response was not ok');
        const data = await response.json();
        uploadLocationsCache = data;
        hideConnectionError();
        const regionCheckboxes = [
            document.getElementById('upload-region-ohio'),
            document.getElementById('upload-region-oregon'),
            document.getElementById('upload-region-virginia')
        ];
        if (data.regions && Array.isArray(data.regions)) {
            data.regions.forEach(function(r, i) {
                if (regionCheckboxes[i]) {
                    regionCheckboxes[i].checked = !!r.enabled;
                    regionCheckboxes[i].disabled = !!data.useCustomHost;
                }
            });
        }
        const useCustomHostCb = document.getElementById('upload-use-custom-host');
        if (useCustomHostCb) useCustomHostCb.checked = !!data.useCustomHost;
        const customHostFields = document.getElementById('upload-custom-host-fields');
        if (customHostFields) customHostFields.style.display = data.useCustomHost ? 'flex' : 'none';
        const customHostInput = document.getElementById('upload-custom-host');
        if (customHostInput) customHostInput.value = data.customHost || '';
        const customPortInput = document.getElementById('upload-custom-port');
        if (customPortInput) customPortInput.value = data.customPort !== undefined ? String(data.customPort) : '7001';
    } catch (error) {
        console.error('Error loading upload locations:', error);
    }
}

async function testUploadEndpoint(host, port, label) {
    if (connectionLost) return;
    const h = (host || '').trim();
    const p = (port === undefined || port === null || port === '') ? 7001 : (typeof port === 'number' ? port : parseInt(port, 10));
    if (!h) {
        await showInfoModal('No host configured for ' + label + '.', 'Test ' + label);
        return;
    }
    if (isNaN(p) || p < 1 || p > 65535) {
        await showInfoModal('Invalid port for ' + label + '. Use 1–65535.', 'Test ' + label);
        return;
    }
    [0, 1, 2, 3].forEach(function(i) {
        const b = document.getElementById('test-upload-region-' + i);
        if (b) b.disabled = true;
    });
    try {
        const response = await fetchWithRetry('/api/advanced/test-upload-host', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ host: h, port: p })
        }, 15000, 1);
        const data = await response.json().catch(function() { return {}; });
        if (data.success) {
            await showInfoModal('Connection to ' + label + ' (' + h + ':' + p + ') succeeded.', 'Test ' + label);
        } else {
            await showInfoModal((data.message || 'Connection failed.') + '\n\nCheck host, port, and network.', 'Test ' + label);
        }
    } catch (error) {
        console.error('Error testing upload endpoint:', error);
        await showInfoModal('Test failed: ' + error.message, 'Test ' + label);
    }
    [0, 1, 2, 3].forEach(function(i) {
        const b = document.getElementById('test-upload-region-' + i);
        if (b) b.disabled = false;
    });
}

async function saveUploadLocations(skipConfirm) {
    if (connectionLost) return false;
    const useCustomHost = document.getElementById('upload-use-custom-host')?.checked || false;
    const customHost = (document.getElementById('upload-custom-host')?.value || '').trim();
    const customPortInput = document.getElementById('upload-custom-port');
    const customPort = customPortInput && customPortInput.value !== '' ? parseInt(customPortInput.value, 10) : 7001;
    const settingsToSend = [];
    if (useCustomHost) {
        settingsToSend.push({ param: 'upload.useCustomHost', value: 'true' });
        settingsToSend.push({ param: 'upload.customHost', value: customHost });
        settingsToSend.push({ param: 'upload.customPort', value: String(customPort >= 1 && customPort <= 65535 ? customPort : 7001) });
        settingsToSend.push({ param: 'upload.enabled[0]', value: 'false' });
        settingsToSend.push({ param: 'upload.enabled[1]', value: 'false' });
        settingsToSend.push({ param: 'upload.enabled[2]', value: 'false' });
        settingsToSend.push({ param: 'upload.enabled[3]', value: 'true' });
    } else {
        settingsToSend.push({ param: 'upload.useCustomHost', value: 'false' });
        settingsToSend.push({ param: 'upload.enabled[3]', value: 'false' });
        settingsToSend.push({ param: 'upload.enabled[0]', value: document.getElementById('upload-region-ohio')?.checked ? 'true' : 'false' });
        settingsToSend.push({ param: 'upload.enabled[1]', value: document.getElementById('upload-region-oregon')?.checked ? 'true' : 'false' });
        settingsToSend.push({ param: 'upload.enabled[2]', value: document.getElementById('upload-region-virginia')?.checked ? 'true' : 'false' });
        if (customHost !== '' || customPort !== 7001) {
            settingsToSend.push({ param: 'upload.customHost', value: customHost });
            settingsToSend.push({ param: 'upload.customPort', value: String(customPort >= 1 && customPort <= 65535 ? customPort : 7001) });
        }
    }
    try {
        let allSuccess = true;
        for (const s of settingsToSend) {
            const response = await fetchWithRetry('/api/audio/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ param: s.param, value: s.value })
            });
            if (!response.ok) { allSuccess = false; break; }
            const data = await response.json();
            if (!data.success) { allSuccess = false; break; }
        }
        if (allSuccess) {
            if (!skipConfirm) await showInfoModal('Upload locations saved successfully.', 'Upload Locations');
            loadUploadLocations();
            return true;
        }
        if (!skipConfirm) await showInfoModal('Failed to save some upload location settings. Please try again.', 'Upload Locations');
        return false;
    } catch (error) {
        console.error('Error saving upload locations:', error);
        if (!skipConfirm) await showInfoModal('Error saving upload locations: ' + error.message, 'Upload Locations');
        return false;
    }
}

function testUploadHost() {
    const host = (document.getElementById('upload-custom-host')?.value || '').trim();
    const portInput = document.getElementById('upload-custom-port');
    const port = portInput && portInput.value !== '' ? parseInt(portInput.value, 10) : 7001;
    testUploadEndpoint(host, port, 'Custom');
}

function testUploadRegion(index) {
    if (!uploadLocationsCache || !uploadLocationsCache.regions || !uploadLocationsCache.regions[index]) {
        showInfoModal('Load upload locations first.', 'Test');
        return;
    }
    const r = uploadLocationsCache.regions[index];
    const host = r.host !== undefined ? String(r.host).trim() : '';
    const port = r.port !== undefined && r.port !== null && r.port !== '' ? (typeof r.port === 'number' ? r.port : parseInt(r.port, 10)) : 7001;
    testUploadEndpoint(host, port, r.name || ('Region ' + index));
}

async function exportSettings() {
    try {
        const response = await fetch('/api/advanced/export-settings');
        const data = await response.json();
        const blob = new Blob([JSON.stringify(data, null, 2)], {type: 'application/json'});
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'boondock-settings.json';
        a.click();
        URL.revokeObjectURL(url);
    } catch (error) {
        console.error('Error exporting settings:', error);
        await showInfoModal('Error exporting settings', 'Export Settings');
    }
}

async function importSettings(event) {
    const file = event.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = async function(e) {
        try {
            const settings = JSON.parse(e.target.result);
            const response = await fetch('/api/advanced/import-settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(settings)
            });
            const data = await response.json();
            if (data.success) {
                await showInfoModal('Settings imported successfully. Device will reboot.', 'Import Settings');
            } else {
                await showInfoModal('Failed to import settings: ' + (data.message || 'Unknown error'), 'Import Settings');
            }
        } catch (error) {
            console.error('Error importing settings:', error);
            await showInfoModal('Error importing settings', 'Import Settings');
        }
    };
    reader.readAsText(file);
}
