#ifndef WEB_JS_H
#define WEB_JS_H

// Embedded JavaScript content for WiFi setup page
static const char jsContent[] PROGMEM = R"(
const API_BASE = '';
let deviceMac = '';
let firmwareVersion = '';
let currentStep = 1;
let modalResolve = null;
document.addEventListener('DOMContentLoaded', function() {
    loadDeviceInfo();
    scanWiFi();
    setupEventListeners();
    goToStep(1);
    setupModalListeners();
});
function setupModalListeners() {
    document.getElementById('modal-confirm').addEventListener('click', function() {
        const overlay = document.getElementById('modal-overlay');
        overlay.classList.remove('show');
        if (modalResolve) {
            modalResolve(true);
            modalResolve = null;
        }
    });
    document.getElementById('modal-cancel').addEventListener('click', function() {
        const overlay = document.getElementById('modal-overlay');
        overlay.classList.remove('show');
        if (modalResolve) {
            modalResolve(false);
            modalResolve = null;
        }
    });
    document.getElementById('modal-overlay').addEventListener('click', function(e) {
        if (e.target === this) {
            this.classList.remove('show');
            if (modalResolve) {
                modalResolve(false);
                modalResolve = null;
            }
        }
    });
    document.getElementById('alert-ok').addEventListener('click', function() {
        const overlay = document.getElementById('alert-overlay');
        overlay.classList.remove('show');
    });
    document.getElementById('alert-overlay').addEventListener('click', function(e) {
        if (e.target === this) {
            this.classList.remove('show');
        }
    });
}
function showAlert(message, title = 'Notice') {
    return new Promise((resolve) => {
        document.getElementById('alert-title').textContent = title;
        document.getElementById('alert-message').textContent = message;
        const overlay = document.getElementById('alert-overlay');
        overlay.classList.add('show');
        const okBtn = document.getElementById('alert-ok');
        const handler = function() {
            overlay.classList.remove('show');
            okBtn.removeEventListener('click', handler);
            resolve();
        };
        okBtn.addEventListener('click', handler);
    });
}
function showConfirm(message, title = 'Confirm') {
    return new Promise((resolve) => {
        document.getElementById('modal-title').textContent = title;
        document.getElementById('modal-message').textContent = message;
        const overlay = document.getElementById('modal-overlay');
        overlay.classList.add('show');
        modalResolve = resolve;
    });
}
function setupEventListeners() {
    document.getElementById('scan-btn').addEventListener('click', scanWiFi);
    document.getElementById('wifi-scan').addEventListener('change', function() {
        const selected = this.value;
        if (selected) {
            document.getElementById('wifi-ssid').value = selected;
        }
    });
    document.getElementById('next-btn-1').addEventListener('click', async function() {
        const ssid = document.getElementById('wifi-ssid').value.trim();
        if (!ssid) {
            await showAlert('Please enter a WiFi network name', 'Input Required');
            return;
        }
        connectToWiFi();
    });
    document.getElementById('back-btn-2').addEventListener('click', function() {
        goToStep(1);
    });
    document.getElementById('back-btn-3').addEventListener('click', function() {
        goToStep(1);
    });
    document.getElementById('copy-device-id').addEventListener('click', copyDeviceId);
    document.getElementById('save-reboot-btn').addEventListener('click', saveAndReboot);
}
function goToStep(step) {
    document.querySelectorAll('.wizard-step').forEach(el => {
        el.classList.remove('active');
    });
    const targetStep = document.getElementById('step-' + step);
    if (targetStep) {
        targetStep.classList.add('active');
    }
    document.querySelectorAll('.progress-step').forEach((el, index) => {
        const stepNum = index + 1;
        el.classList.remove('active', 'completed');
        if (stepNum < step) {
            el.classList.add('completed');
        } else if (stepNum === step) {
            el.classList.add('active');
        }
    });
    currentStep = step;
}
async function loadDeviceInfo() {
    try {
        const response = await fetch('/api/device-info');
        const data = await response.json();
        deviceMac = data.mac || 'Unknown';
        firmwareVersion = data.firmware || 'Unknown';
        let deviceType = 'WiFi Setup';
        if (firmwareVersion.includes('ECHO')) {
            deviceType = 'Echo WiFi Setup';
        } else if (firmwareVersion.includes('TANGO')) {
            deviceType = 'Tango WiFi Setup';
        }
        document.getElementById('device-type').textContent = deviceType;
        document.getElementById('firmware-version-header').textContent = firmwareVersion;
    } catch (error) {
        console.error('Error loading device info:', error);
    }
}
async function scanWiFi() {
    const select = document.getElementById('wifi-scan');
    const scanBtn = document.getElementById('scan-btn');
    select.innerHTML = '<option value="">Scanning for networks...</option>';
    select.disabled = true;
    scanBtn.disabled = true;
    scanBtn.textContent = 'Scanning...';
    try {
        const response = await fetch('/api/wifi/scan');
        const data = await response.json();
        select.innerHTML = '<option value="">Select a network...</option>';
        if (data.networks && data.networks.length > 0) {
            const uniqueNetworks = new Map();
            data.networks.forEach(network => {
                const ssid = network.ssid;
                if (!ssid || ssid.length === 0) return;
                if (!uniqueNetworks.has(ssid)) {
                    uniqueNetworks.set(ssid, network);
                } else {
                    const existing = uniqueNetworks.get(ssid);
                    if (network.rssi > existing.rssi) {
                        uniqueNetworks.set(ssid, network);
                    }
                }
            });
            const sortedNetworks = Array.from(uniqueNetworks.values()).sort((a, b) => b.rssi - a.rssi);
            sortedNetworks.forEach(network => {
                const option = document.createElement('option');
                option.value = network.ssid;
                option.textContent = `${network.ssid} ${network.encrypted ? '(🔒)' : '(Open)'} (${network.rssi} dBm)`;
                select.appendChild(option);
            });
        } else {
            select.innerHTML = '<option value="">No networks found</option>';
        }
    } catch (error) {
        console.error('Error scanning WiFi:', error);
        select.innerHTML = '<option value="">Scan failed - try again</option>';
    } finally {
        select.disabled = false;
        scanBtn.disabled = false;
        scanBtn.textContent = 'Refresh Scan';
    }
}
async function connectToWiFi() {
    const ssid = document.getElementById('wifi-ssid').value.trim();
    const password = document.getElementById('wifi-password').value;
    goToStep(2);
    document.getElementById('connecting-message').textContent = 'Connecting to ' + ssid + '... Please wait.';
    try {
        const testResponse = await fetch('/api/wifi/test', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ssid: ssid, password: password})
        });
        const testData = await testResponse.json();
        if (!testData.success) {
            goToStep(1);
            await showAlert('Connection failed: ' + (testData.message || 'Unknown error') + '\n\nPlease check your SSID and password and try again.', 'Connection Failed');
            return;
        }
        document.getElementById('connecting-message').textContent = 'Connection successful! Getting device information...';
        await new Promise(resolve => setTimeout(resolve, 1000));
        const deviceInfoResponse = await fetch('/api/device-info');
        const deviceInfo = await deviceInfoResponse.json();
        const deviceIp = deviceInfo.ip || 'Not available';
        goToStep(3);
        document.getElementById('device-id').textContent = deviceMac;
        document.getElementById('ip-address').textContent = deviceIp;
    } catch (error) {
        console.error('Error connecting to WiFi:', error);
        goToStep(1);
        await showAlert('Failed to connect. Please try again.', 'Connection Error');
    }
}
async function saveAndReboot() {
    const ssid = document.getElementById('wifi-ssid').value.trim();
    const password = document.getElementById('wifi-password').value;
    if (!ssid) {
        await showAlert('Please enter a WiFi network name', 'Input Required');
        return;
    }
    const confirmed = await showConfirm('Save WiFi credentials and reboot the device? The device will connect to the network after reboot.', 'Confirm Save & Reboot');
    if (!confirmed) {
        return;
    }
    const saveBtn = document.getElementById('save-reboot-btn');
    saveBtn.disabled = true;
    saveBtn.textContent = 'Saving...';
    try {
        const response = await fetch('/api/wifi/save', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ssid: ssid, password: password})
        });
        const data = await response.json();
        if (data.success) {
            saveBtn.textContent = 'Saved! Rebooting...';
            saveBtn.style.background = 'var(--orange)';
            setTimeout(async () => {
                await showAlert('WiFi credentials saved! Device is rebooting. Please wait and then connect to the configured WiFi network.', 'Success');
            }, 1000);
        } else {
            await showAlert('Failed to save: ' + (data.message || 'Unknown error'), 'Save Failed');
            saveBtn.disabled = false;
            saveBtn.textContent = 'Save & Reboot';
        }
    } catch (error) {
        console.error('Error saving WiFi:', error);
        await showAlert('Failed to save WiFi credentials. Please try again.', 'Error');
        saveBtn.disabled = false;
        saveBtn.textContent = 'Save & Reboot';
    }
}
function copyDeviceId() {
    const deviceIdElement = document.getElementById('device-id');
    const deviceId = deviceIdElement.textContent;
    navigator.clipboard.writeText(deviceId).then(() => {
        const btn = document.getElementById('copy-device-id');
        const originalText = btn.textContent;
        btn.textContent = 'Copied!';
        btn.style.background = 'var(--orange)';
        btn.style.color = 'var(--white)';
        btn.style.borderColor = 'var(--orange)';
        setTimeout(() => {
            btn.textContent = originalText;
            btn.style.background = '';
            btn.style.color = '';
            btn.style.borderColor = '';
        }, 2000);
    }).catch(async (err) => {
        console.error('Failed to copy:', err);
        await showAlert('Failed to copy Device ID. Please copy it manually.', 'Copy Failed');
    });
}
)";

#endif // WEB_JS_H

