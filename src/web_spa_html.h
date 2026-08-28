#ifndef WEB_SPA_HTML_H
#define WEB_SPA_HTML_H

#include "config.h"

// Single Page Application HTML content (PRODUCT_BROWSER_TITLE from build env: TANGO / ECHO)
static const char spaHtmlContent[] PROGMEM =
    R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)"
    PRODUCT_BROWSER_TITLE
    R"(</title>
    <link rel="stylesheet" href="/app.css">

    <script>
        (function() {
            var selected = 'night-ops';
            try {
                var saved = JSON.parse(localStorage.getItem('boondock.uiThemeState.v2'));
                if (saved && (saved.selected === 'night-ops' || saved.selected === 'ember-command')) {
                    selected = saved.selected;
                }
            } catch (error) {}
            document.documentElement.dataset.uiTheme = selected;
        })();
    </script>
</head>
<body>
    <div class="app-container">
        <aside class="sidebar">
            <div class="sidebar-header">
                <h1>)"
    PRODUCT_BROWSER_TITLE
    R"(</h1>
                <div id="sidebar-firmware" class="sidebar-firmware">Firmware : --</div>
            </div>
            <nav class="sidebar-nav">
                <a href="#home" class="nav-item" data-route="home">
                    <span class="nav-icon">🏠</span>
                    <span class="nav-label">Home</span>
                </a>
                <a href="#audio" class="nav-item" data-route="audio">
                    <span class="nav-icon">⏺️</span>
                    <span class="nav-label">Recorder</span>
                </a>
                <a href="#live-audio" class="nav-item" data-route="live-audio">
                    <span class="nav-icon">🔊</span>
                    <span class="nav-label">Live Audio</span>
                </a>
)"
#if defined(ECHO)
R"(
                <a href="#player-tx" class="nav-item" data-route="player-tx">
                    <span class="nav-icon">🎚️</span>
                    <span class="nav-label">Player &amp; TX</span>
                </a>
                <a href="#cw" class="nav-item" data-route="cw">
                    <span class="nav-icon">📡</span>
                    <span class="nav-label">CW</span>
                </a>
)"
#endif
R"(
                <a href="#recordings" class="nav-item" data-route="recordings">
                    <span class="nav-icon">📁</span>
                    <span class="nav-label">Recordings</span>
                </a>
                <a href="#network" class="nav-item" data-route="network">
                    <span class="nav-icon">🌐</span>
                    <span class="nav-label">Network</span>
                </a>
                <a href="#advanced" class="nav-item" data-route="advanced">
                    <span class="nav-icon">⚙️</span>
                    <span class="nav-label">Advanced</span>
                </a>
            </nav>
            <div class="sidebar-footer">
                <div class="footer-info">
                    <div>MAC ADDRESS / DEVICE ID</div>
                    <div id="device-id" class="device-id">Loading...</div>
                </div>
                <div class="footer-copyright">© Boondock Technologies 2025</div>
                <div class="footer-website">www.boondockecho.com</div>
            </div>
        </aside>
        <main class="main-content">
            <header class="content-header">
                <div class="content-header-main">
                    <h2 class="page-title" id="page-title">
                        <span class="title-icon" id="title-icon">📊</span>
                        <span id="title-text">Device Summary</span>
                    </h2>
                    <p id="page-title-extra" class="page-title-extra" style="display:none;" role="note"></p>
                </div>
                <div class="header-time-container">
                    <div class="header-time" id="current-time">--:--:-- --</div>
                    <div class="header-timezone" id="header-timezone">--</div>
                </div>
            </header>
            <div class="content-body" id="content-body">
                <!-- Content will be dynamically loaded here -->
            </div>
        </main>
    </div>
    
    <!-- Global Modal -->
    <div id="global-modal-overlay" class="popup-overlay" style="display: none;">
        <div class="popup-content global-modal">
            <div class="modal-header">
                <span id="global-modal-icon" class="modal-icon">ℹ️</span>
                <h3 id="global-modal-title" class="modal-title">Title</h3>
            </div>
            <div class="modal-body">
                <p id="global-modal-message"></p>
            </div>
            <div class="modal-footer">
                <button id="global-modal-cancel" class="btn-secondary">Cancel</button>
                <button id="global-modal-confirm" class="btn-primary">OK</button>
            </div>
        </div>
    </div>
    
    <!-- Loading Popup -->
    <div id="loading-popup" class="popup-overlay" style="display: none;">
        <div class="popup-content">
            <div class="popup-spinner"></div>
            <div class="popup-message">Loading...</div>
        </div>
    </div>
    
    <!-- Connection Error Popup -->
    <div id="connection-error-popup" class="popup-overlay" style="display: none;">
        <div class="popup-content popup-error">
            <div class="popup-icon">⚠️</div>
            <div class="popup-title">Connection Lost</div>
            <div class="popup-message">Unable to connect to the device. Please check your connection and try again.</div>
            <div class="popup-buttons">
                <button id="refresh-page-btn" class="btn-primary">Refresh Page</button>
            </div>
        </div>
    </div>
    
    <!-- Firmware Reboot Countdown Popup -->
    <div id="reboot-countdown-popup" class="popup-overlay" style="display: none;">
        <div class="popup-content">
            <div class="popup-icon">🔄</div>
            <div class="popup-title">Firmware Update Complete</div>
            <div class="popup-message">Device is rebooting. This page will refresh automatically.</div>
            <div class="countdown-display">
                <div class="countdown-number" id="countdown-number">30</div>
                <div class="countdown-label">seconds</div>
            </div>
            <div class="popup-buttons">
                <button id="refresh-now-btn" class="btn-primary">Refresh Now</button>
            </div>
        </div>
    </div>
    
    <script src="/app.js"></script>
</body>
</html>
)";

#endif // WEB_SPA_HTML_H
