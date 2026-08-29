#ifndef WEB_HTML_H
#define WEB_HTML_H

// Embedded HTML content for WiFi setup page
static const char htmlContent[] PROGMEM = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Boondock WiFi Setup</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <header>
            <h1>Boondock <span id="device-type">WiFi Setup</span></h1>
            <div id="firmware-version-header" class="firmware-version">Loading...</div>
        </header>
        <main>
            <div class="wizard-progress">
                <div class="progress-step active" id="step-indicator-1">
                    <div class="step-number">1</div>
                    <div class="step-label">Connect WiFi</div>
                </div>
                <div class="progress-line"></div>
                <div class="progress-step" id="step-indicator-2">
                    <div class="step-number">2</div>
                    <div class="step-label">Connecting</div>
                </div>
                <div class="progress-line"></div>
                <div class="progress-step" id="step-indicator-3">
                    <div class="step-number">3</div>
                    <div class="step-label">Complete</div>
                </div>
            </div>
            <div class="wizard-step active" id="step-1">
                <div class="card">
                    <h2>Step 1: Connect to WiFi</h2>
                    <p class="instruction">Select a WiFi network from the list below or enter network details manually.</p>
                    <div class="form-group">
                        <label for="wifi-scan">Available Networks:</label>
                        <div class="wifi-scan-group">
                            <select id="wifi-scan" class="form-control">
                                <option value="">Scanning for networks...</option>
                            </select>
                            <button id="scan-btn" class="btn-secondary">Refresh Scan</button>
                        </div>
                    </div>
                    <div class="form-group">
                        <label for="wifi-ssid">Network Name (SSID):</label>
                        <input type="text" id="wifi-ssid" class="form-control" placeholder="Enter WiFi network name" required>
                    </div>
                    <div class="form-group">
                        <label for="wifi-password">Password:</label>
                        <input type="password" id="wifi-password" class="form-control" placeholder="Enter WiFi password">
                        <small>Leave blank for open networks</small>
                    </div>
                    <div class="form-actions">
                        <button id="next-btn-1" class="btn-primary">Next</button>
                    </div>
                </div>
            </div>
            <div class="wizard-step" id="step-2">
                <div class="card">
                    <h2>Step 2: Connecting...</h2>
                    <div class="connecting-content">
                        <div class="spinner-large"></div>
                        <p class="connecting-message" id="connecting-message">Connecting to WiFi network... Please wait.</p>
                    </div>
                    <div class="form-actions">
                        <button id="back-btn-2" class="btn-secondary">Back</button>
                    </div>
                </div>
            </div>
            <div class="wizard-step" id="step-3">
                <div class="card">
                    <h2>Step 3: Device Information</h2>
                    <p class="instruction">Your device has been successfully connected! Please copy and save the Device ID below for registration.</p>
                    <div class="device-info">
                        <div class="info-item">
                            <label>Device ID (MAC Address):</label>
                            <div class="info-value-group">
                                <span id="device-id" class="device-id">Loading...</span>
                                <button id="copy-device-id" class="btn-secondary">Copy</button>
                            </div>
                        </div>
                        <div class="info-item">
                            <label>IP Address:</label>
                            <span id="ip-address" class="info-value">Loading...</span>
                        </div>
                    </div>
                    <div class="form-actions">
                        <button id="back-btn-3" class="btn-secondary">Back</button>
                        <button id="save-reboot-btn" class="btn-primary">Save & Reboot</button>
                    </div>
                </div>
            </div>
        </main>
        <footer>
            <p>Copyright &copy; 2025 Boondock Technologies</p>
        </footer>
    </div>
    
    <!-- Modal Dialog -->
    <div id="modal-overlay" class="modal-overlay">
        <div class="modal-dialog">
            <div class="modal-header">
                <h3 id="modal-title">Confirm</h3>
            </div>
            <div class="modal-body">
                <p id="modal-message"></p>
            </div>
            <div class="modal-footer">
                <button id="modal-cancel" class="btn-secondary">Cancel</button>
                <button id="modal-confirm" class="btn-primary">Confirm</button>
            </div>
        </div>
    </div>
    
    <!-- Alert Dialog -->
    <div id="alert-overlay" class="modal-overlay">
        <div class="modal-dialog">
            <div class="modal-header">
                <h3 id="alert-title">Notice</h3>
            </div>
            <div class="modal-body">
                <p id="alert-message"></p>
            </div>
            <div class="modal-footer">
                <button id="alert-ok" class="btn-primary">OK</button>
            </div>
        </div>
    </div>
    
    <script src="/script.js"></script>
</body>
</html>
)";

#endif // WEB_HTML_H

