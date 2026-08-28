#ifndef WEB_AP_CSS_H
#define WEB_AP_CSS_H

// Embedded CSS content for AP setup page (WiFi configuration)
static const char apCssContent[] PROGMEM = R"(
/* Boondock Color Palette */
:root {
    --white: #FFFFFF;
    --light-orange: #FFD283;
    --light-yellow: #FFFACA;
    --bright-blue: #0587C7;
    --dark-blue: #002942;
    --dark-gray: #202020;
    --orange: #F36D22;
    --red: #D42329;
    --cyan: #03BBDF;
    --light-blue: #A4EDFF;
}

* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
    background: linear-gradient(135deg, var(--dark-blue) 0%, var(--bright-blue) 100%);
    color: var(--dark-gray);
    min-height: 100vh;
    padding: 16px;
    display: flex;
    justify-content: center;
    align-items: center;
    overflow-x: hidden;
}

.container {
    width: 100%;
    max-width: 600px;
    background: var(--white);
    border-radius: 16px;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.2);
    overflow: hidden;
    display: flex;
    flex-direction: column;
    max-height: calc(100vh - 32px);
    min-height: auto;
}

header {
    background: linear-gradient(135deg, var(--orange) 0%, var(--red) 100%);
    color: var(--white);
    padding: 20px;
    text-align: center;
    flex-shrink: 0;
}

header h1 {
    font-size: 24px;
    font-weight: 600;
    margin: 0;
}

header h1 span {
    font-weight: 300;
    opacity: 0.95;
}

.firmware-version {
    font-size: 12px;
    font-weight: 400;
    opacity: 0.9;
    margin-top: 6px;
    letter-spacing: 0.5px;
}

main {
    flex: 1;
    padding: 24px;
    overflow-y: auto;
    overflow-x: hidden;
    min-height: 0;
}

/* Wizard Progress Indicator */
.wizard-progress {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 24px;
    padding: 0 8px;
}

.progress-step {
    display: flex;
    flex-direction: column;
    align-items: center;
    flex: 1;
    position: relative;
}

.step-number {
    width: 40px;
    height: 40px;
    border-radius: 50%;
    background: var(--light-blue);
    color: var(--dark-blue);
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 600;
    font-size: 18px;
    transition: all 0.3s ease;
    border: 3px solid var(--light-blue);
    flex-shrink: 0;
}

.progress-step.active .step-number {
    background: var(--bright-blue);
    color: var(--white);
    border-color: var(--bright-blue);
    box-shadow: 0 0 0 4px rgba(5, 135, 199, 0.2);
}

.progress-step.completed .step-number {
    background: var(--orange);
    color: var(--white);
    border-color: var(--orange);
}

.step-label {
    margin-top: 8px;
    font-size: 11px;
    color: var(--dark-gray);
    text-align: center;
    font-weight: 500;
    white-space: nowrap;
}

.progress-step.active .step-label {
    color: var(--bright-blue);
    font-weight: 600;
}

.progress-line {
    flex: 1;
    height: 3px;
    background: var(--light-blue);
    margin: 0 8px;
    margin-top: -25px;
    transition: all 0.3s ease;
    min-width: 20px;
}

.progress-step.completed ~ .progress-line {
    background: var(--orange);
}

/* Wizard Steps - CRITICAL: Hide inactive steps */
.wizard-step {
    display: none !important;
}

.wizard-step.active {
    display: block !important;
    animation: fadeIn 0.3s ease;
}

@keyframes fadeIn {
    from {
        opacity: 0;
        transform: translateY(10px);
    }
    to {
        opacity: 1;
        transform: translateY(0);
    }
}

/* Connecting Step */
.connecting-content {
    text-align: center;
    padding: 40px 20px;
}

.spinner-large {
    border: 4px solid var(--light-blue);
    border-top: 4px solid var(--bright-blue);
    border-radius: 50%;
    width: 60px;
    height: 60px;
    animation: spin 1s linear infinite;
    margin: 0 auto 24px;
}

.connecting-message {
    color: var(--dark-blue);
    font-size: 16px;
    font-weight: 500;
}

@keyframes spin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
}

/* Device Info */
.device-info {
    display: flex;
    flex-direction: column;
    gap: 16px;
    margin: 16px 0;
}

.info-value-group {
    display: flex;
    align-items: center;
    gap: 12px;
    flex-wrap: wrap;
}

.device-id {
    font-family: 'Courier New', monospace;
    font-weight: 600;
    color: var(--bright-blue);
    font-size: 16px;
    word-break: break-all;
    flex: 1;
    min-width: 200px;
}

.info-value {
    color: var(--dark-gray);
    font-size: 15px;
    word-break: break-all;
}

.card {
    background: var(--white);
    border-radius: 12px;
    padding: 20px;
    margin-bottom: 16px;
    border: 1px solid rgba(0, 0, 0, 0.1);
}

.card h2 {
    color: var(--dark-blue);
    font-size: 20px;
    margin-bottom: 12px;
    font-weight: 600;
}

.instruction {
    color: var(--dark-gray);
    margin-bottom: 16px;
    line-height: 1.6;
    font-size: 14px;
}

.info-item {
    display: flex;
    flex-direction: column;
    gap: 6px;
}

.info-item label {
    font-weight: 600;
    color: var(--dark-blue);
    font-size: 14px;
}

.info-item span {
    color: var(--dark-gray);
    font-size: 15px;
    word-break: break-all;
}

.mac-address {
    font-family: 'Courier New', monospace;
    font-weight: 600;
    color: var(--bright-blue);
    font-size: 16px;
}

.form-group {
    margin-bottom: 16px;
}

.form-group label {
    display: block;
    margin-bottom: 6px;
    font-weight: 600;
    color: var(--dark-blue);
    font-size: 14px;
}

.wifi-scan-group {
    display: flex;
    gap: 10px;
    align-items: stretch;
    flex-wrap: wrap;
}

.wifi-scan-group .form-control {
    flex: 1;
    min-width: 200px;
}

.wifi-scan-group .btn-secondary {
    flex: 0 0 auto;
    min-width: 120px;
    white-space: nowrap;
}

.form-control {
    width: 100%;
    padding: 12px;
    border: 2px solid var(--light-blue);
    border-radius: 8px;
    font-size: 15px;
    transition: all 0.3s ease;
    background: var(--white);
    color: var(--dark-gray);
    font-family: inherit;
}

.form-control:focus {
    outline: none;
    border-color: var(--bright-blue);
    box-shadow: 0 0 0 3px rgba(5, 135, 199, 0.1);
}

.form-control:disabled {
    background: var(--light-yellow);
    cursor: not-allowed;
}

small {
    display: block;
    margin-top: 6px;
    color: #666;
    font-size: 12px;
}

.form-actions {
    display: flex;
    gap: 10px;
    margin-top: 20px;
    flex-wrap: wrap;
}

.btn-primary, .btn-secondary {
    padding: 12px 24px;
    border: none;
    border-radius: 8px;
    font-size: 15px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.3s ease;
    flex: 1;
    min-width: 120px;
}

.btn-primary {
    background: linear-gradient(135deg, var(--bright-blue) 0%, var(--cyan) 100%);
    color: var(--white);
}

.btn-primary:hover:not(:disabled) {
    transform: translateY(-2px);
    box-shadow: 0 5px 15px rgba(5, 135, 199, 0.3);
}

.btn-primary:disabled {
    opacity: 0.6;
    cursor: not-allowed;
}

.btn-secondary {
    background: var(--white);
    color: var(--bright-blue);
    border: 2px solid var(--bright-blue);
}

.btn-secondary:hover:not(:disabled) {
    background: var(--light-blue);
    transform: translateY(-2px);
}

.btn-secondary:disabled {
    opacity: 0.6;
    cursor: not-allowed;
}

.status-message {
    margin-top: 20px;
    padding: 16px;
    border-radius: 8px;
    display: none;
    font-size: 14px;
    line-height: 1.6;
}

.status-message.show {
    display: block;
}

.status-message.success {
    background: var(--light-yellow);
    color: var(--dark-blue);
    border: 2px solid var(--orange);
}

.status-message.error {
    background: #ffe6e6;
    color: var(--red);
    border: 2px solid var(--red);
}

.status-message.info {
    background: var(--light-blue);
    color: var(--dark-blue);
    border: 2px solid var(--bright-blue);
}

.status-message.loading {
    background: var(--light-yellow);
    color: var(--dark-blue);
    border: 2px solid var(--orange);
}

footer {
    background: var(--dark-gray);
    color: var(--white);
    padding: 16px;
    text-align: center;
    font-size: 11px;
    flex-shrink: 0;
}

footer p {
    margin: 0;
    opacity: 0.9;
}

/* Modal Dialog */
.modal-overlay {
    display: none;
    position: fixed;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: rgba(0, 0, 0, 0.6);
    z-index: 1000;
    align-items: center;
    justify-content: center;
    animation: fadeIn 0.2s ease;
}

.modal-overlay.show {
    display: flex;
}

.modal-dialog {
    background: var(--white);
    border-radius: 12px;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.3);
    max-width: 400px;
    width: 90%;
    max-height: 90vh;
    overflow-y: auto;
    animation: slideUp 0.3s ease;
}

@keyframes slideUp {
    from {
        opacity: 0;
        transform: translateY(20px);
    }
    to {
        opacity: 1;
        transform: translateY(0);
    }
}

.modal-header {
    padding: 16px 20px;
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);
}

.modal-header h3 {
    margin: 0;
    color: var(--dark-blue);
    font-size: 18px;
    font-weight: 600;
}

.modal-body {
    padding: 20px;
}

.modal-body p {
    margin: 0;
    color: var(--dark-gray);
    font-size: 15px;
    line-height: 1.6;
}

.modal-footer {
    padding: 16px 20px;
    border-top: 1px solid rgba(0, 0, 0, 0.1);
    display: flex;
    gap: 10px;
    justify-content: flex-end;
    flex-wrap: wrap;
}

.modal-footer .btn-primary,
.modal-footer .btn-secondary {
    min-width: 100px;
    flex: 0 0 auto;
}

/* Tablet Styles (768px - 1024px) */
@media (min-width: 768px) and (max-width: 1024px) {
    body {
        padding: 12px;
    }
    
    .container {
        max-width: 550px;
        max-height: calc(100vh - 24px);
    }
    
    header {
        padding: 18px;
    }
    
    header h1 {
        font-size: 22px;
    }
    
    main {
        padding: 20px;
    }
    
    .wizard-progress {
        margin-bottom: 20px;
    }
    
    .step-number {
        width: 36px;
        height: 36px;
        font-size: 16px;
    }
    
    .step-label {
        font-size: 10px;
    }
}

/* Mobile Styles (max-width: 767px) */
@media (max-width: 767px) {
    body {
        padding: 0;
        align-items: flex-start;
    }
    
    .container {
        max-width: 100%;
        border-radius: 0;
        max-height: 100vh;
        min-height: 100vh;
    }
    
    header {
        padding: 18px 16px;
    }
    
    header h1 {
        font-size: 20px;
    }
    
    .firmware-version {
        font-size: 11px;
    }
    
    main {
        padding: 20px 16px;
    }
    
    .wizard-progress {
        margin-bottom: 20px;
        padding: 0 4px;
    }
    
    .step-number {
        width: 32px;
        height: 32px;
        font-size: 14px;
        border-width: 2px;
    }
    
    .step-label {
        font-size: 9px;
        margin-top: 6px;
    }
    
    .progress-line {
        margin: 0 4px;
        margin-top: -20px;
        min-width: 10px;
    }
    
    .card {
        padding: 16px;
    }
    
    .card h2 {
        font-size: 18px;
    }
    
    .instruction {
        font-size: 13px;
    }
    
    .wifi-scan-group {
        flex-direction: column;
    }
    
    .wifi-scan-group .form-control,
    .wifi-scan-group .btn-secondary {
        width: 100%;
        min-width: 100%;
    }
    
    .form-actions {
        flex-direction: column;
    }
    
    .btn-primary, .btn-secondary {
        width: 100%;
        min-width: 100%;
    }
    
    .device-id {
        font-size: 14px;
        min-width: 100%;
    }
    
    .modal-dialog {
        width: 95%;
        max-width: 95%;
    }
}

/* Small Mobile Styles (max-width: 480px) */
@media (max-width: 480px) {
    header {
        padding: 16px 14px;
    }
    
    header h1 {
        font-size: 18px;
    }
    
    main {
        padding: 16px 12px;
    }
    
    .wizard-progress {
        margin-bottom: 16px;
    }
    
    .step-number {
        width: 28px;
        height: 28px;
        font-size: 12px;
    }
    
    .step-label {
        font-size: 8px;
    }
    
    .card {
        padding: 14px;
    }
    
    .card h2 {
        font-size: 16px;
    }
    
    .form-control {
        font-size: 14px;
        padding: 10px;
    }
    
    .btn-primary, .btn-secondary {
        padding: 10px 20px;
        font-size: 14px;
    }
}

/* Large Screen Optimization (1920px and above - 1080p) */
@media (min-width: 1920px) {
    body {
        padding: 24px;
    }
    
    .container {
        max-width: 650px;
        max-height: calc(100vh - 48px);
    }
    
    header {
        padding: 24px;
    }
    
    header h1 {
        font-size: 26px;
    }
    
    main {
        padding: 32px;
    }
    
    .step-number {
        width: 44px;
        height: 44px;
        font-size: 20px;
    }
    
    .step-label {
        font-size: 12px;
    }
}
)";

#endif // WEB_AP_CSS_H
