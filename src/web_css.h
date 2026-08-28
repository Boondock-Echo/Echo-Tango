#ifndef WEB_CSS_H
#define WEB_CSS_H

// Embedded CSS content for Main SPA (Single Page Application)
static const char cssContent[] PROGMEM = R"(
/* Theme palettes and component rules */
html[data-ui-theme="night-ops"] {
    --ui-background: #0d0b0a;
    --ui-surface: #14110f;
    --ui-panel: #191412;
    --ui-border: #6a3d28;
    --ui-accent: #f36d2d;
    --ui-text: #fff1e8;
    --ui-muted: #cfbaaa;
    --ui-success: #8fae7e;
    --ui-warning: #d6a85f;
    --ui-danger: #c96b4b;
    --ui-button-text: #2b140b;
}

html[data-ui-theme="ember-command"] {
    --ui-background: #f5eee7;
    --ui-surface: #fffaf5;
    --ui-panel: #ffffff;
    --ui-border: #c98962;
    --ui-accent: #b84f1f;
    --ui-text: #38251d;
    --ui-muted: #785f52;
    --ui-success: #5f7d52;
    --ui-warning: #9a6014;
    --ui-danger: #a84732;
    --ui-button-text: #ffffff;
}

.theme-choice-btn {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 12px;
    text-align: left;
    background: var(--ui-panel);
    color: var(--ui-text);
    border: 1px solid var(--ui-border);
    border-radius: 10px;
}

.theme-choice-btn.active {
    background: var(--ui-accent);
    color: var(--ui-button-text);
}

* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

/* Main Application Layout (SPA Mode) */
html, body {
    margin: 0;
    padding: 0;
    width: 100%;
    height: 100%;
    overflow: hidden;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
    color: var(--ui-text);
}

body:has(.app-container) {
    position: relative;
    background: var(--ui-background);
}

.app-container {
    display: flex;
    width: 100vw;
    height: 100vh;
    background: var(--ui-background);
    overflow: hidden;
    position: relative;
}

.sidebar {
    width: 280px;
    background: var(--ui-background);
    color: var(--ui-text);
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
    overflow-y: auto;
    overflow-x: hidden;
}

.sidebar-header {
    padding: 24px 20px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.1);
    flex-shrink: 0;
}

.sidebar-header h1 {
    font-size: 22px;
    font-weight: 600;
    margin: 0;
    color: var(--ui-text);
}

.sidebar-firmware {
    font-size: 12px;
    color: var(--ui-text);
    margin-top: 6px;
    margin-bottom: 0;
}

.sidebar-nav {
    flex: 1;
    padding: 12px 0;
    overflow-y: auto;
}

.nav-item {
    display: flex;
    align-items: center;
    padding: 14px 20px;
    color: var(--ui-text);
    text-decoration: none;
    transition: background 0.2s;
    gap: 12px;
    cursor: pointer;
}

.nav-item:hover {
    background: rgba(255, 255, 255, 0.1);
}

.nav-item.active {
    background: var(--ui-accent);
    color: var(--ui-button-text);
}

.nav-icon {
    font-size: 20px;
    flex-shrink: 0;
}

.nav-label {
    font-size: 15px;
    font-weight: 500;
}

.sidebar-footer {
    padding: 16px 20px;
    border-top: 1px solid rgba(255, 255, 255, 0.1);
    font-size: 11px;
    flex-shrink: 0;
}

.footer-info {
    margin-bottom: 8px;
}

.footer-info > div:first-child {
    opacity: 0.7;
    margin-bottom: 4px;
    font-size: 10px;
}

.device-id {
    font-family: 'Courier New', monospace;
    font-weight: 600;
    font-size: 18px;
    word-break: break-all;
}

.footer-copyright {
    margin-top: 8px;
    opacity: 0.7;
    font-size: 10px;
}

.footer-website {
    margin-top: 4px;
    opacity: 0.7;
    font-size: 10px;
}

.main-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    background: var(--ui-surface);
    min-width: 0;
    overflow: hidden;
}

.content-header {
    background: var(--ui-surface);
    padding: 20px 32px;
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: 16px;
    border-bottom: 2px solid var(--ui-accent);
    flex-shrink: 0;
}

.content-header-main {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
    align-items: flex-start;
}

.page-title-extra {
    font-size: 13px;
    font-weight: 400;
    color: var(--ui-muted);
    margin: 0;
    max-width: 720px;
    line-height: 1.45;
}

.page-title-extra .page-title-extra-tz {
    display: block;
    margin-top: 6px;
    padding-top: 6px;
    border-top: 1px solid rgba(0, 0, 0, 0.08);
}

.page-title {
    display: flex;
    align-items: center;
    gap: 12px;
    font-size: 26px;
    font-weight: 600;
    color: var(--ui-text);
    margin: 0;
}

.title-icon {
    font-size: 30px;
}

.header-time-container {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 4px;
    flex-shrink: 0;
    padding-top: 4px;
}

.header-time {
    font-size: 24px;
    font-weight: 500;
    color: var(--ui-text);
    height: 24px;
    line-height: 1;
}

.header-timezone {
    font-size: 12px;
    font-weight: 400;
    color: var(--ui-muted);
    opacity: 0.7;
    line-height: 1;
}

.content-body {
    flex: 1;
    padding: 32px;
    overflow-y: auto;
    overflow-x: hidden;
    min-height: 0;
    display: flex;
    flex-direction: column;
    background: var(--ui-surface);
}

/* Home Page Styles */
.home-top-section {
    flex: 0 0 auto;
    margin-bottom: 24px;
}

.summary-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 20px;
}

/* Repeater section (separate from KPI cards) */
.repeater-section {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 18px 20px;
    margin-top: 8px;
}

.repeater-header {
    display: flex;
    flex-direction: column;
    gap: 4px;
    margin-bottom: 12px;
}

.repeater-header h3 {
    margin: 0;
    color: var(--ui-text);
    font-size: 18px;
    font-weight: 600;
}

.repeater-subtitle {
    color: var(--ui-muted);
    opacity: 0.8;
    font-size: 13px;
    line-height: 1.35;
}

.repeater-controls {
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.repeater-control {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    padding: 10px 0;
    border-top: 1px solid rgba(0, 0, 0, 0.06);
}

.repeater-control:first-child {
    border-top: 0;
    padding-top: 0;
}

.repeater-label {
    font-weight: 600;
    color: var(--ui-muted);
    font-size: 14px;
}

.repeater-value {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 12px;
    flex-wrap: wrap;
    color: var(--ui-text);
    font-weight: 500;
    font-size: 14px;
}

.repeater-status {
    font-size: 13px;
    opacity: 0.9;
}

.repeater-radios {
    gap: 14px;
}

.radio-inline {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-weight: 500;
    color: var(--ui-text);
}

.home-terminal-section {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-top: 2px solid var(--ui-border);
    padding-top: 24px;
}

.terminal-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 12px;
}

.terminal-header h3 {
    margin: 0;
    color: var(--ui-text);
    font-size: 18px;
    font-weight: 600;
}

.terminal-output {
    flex: 1;
    background: var(--ui-panel);
    color: var(--ui-text);
    font-family: 'Courier New', 'Consolas', 'Monaco', monospace;
    font-size: 13px;
    padding: 16px;
    border-radius: 8px;
    overflow-y: auto;
    overflow-x: auto;
    white-space: pre-wrap;
    word-wrap: break-word;
    min-height: 200px;
    max-height: 100%;
    line-height: 1.5;
}

.terminal-output .log-line {
    margin: 2px 0;
}

.terminal-output .log-error {
    color: #f48771;
}

.terminal-output .log-warning {
    color: #cca700;
}

.terminal-output .log-info {
    color: #4ec9b0;
}

.terminal-output .log-event {
    color: #4fc1ff;
}

.terminal-output .log-fatal {
    color: #f48771;
    font-weight: bold;
}

.summary-card {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 20px;
}

.summary-card h3 {
    color: var(--ui-text);
    font-size: 18px;
    margin-bottom: 16px;
    font-weight: 600;
}

.info-list {
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.info-row {
    display: flex;
    justify-content: space-between;
    padding: 8px 0;
    border-bottom: 1px solid rgba(0, 0, 0, 0.05);
}

.info-label {
    font-weight: 600;
    color: var(--ui-muted);
    font-size: 14px;
}

.info-value {
    color: var(--ui-text);
    font-weight: 500;
    font-size: 14px;
}

/* Audio Page Styles */
.status-buttons {
    display: flex;
    gap: 16px;
    margin-bottom: 24px;
    align-items: center;
    flex-wrap: wrap;
}

.status-buttons .status-btn.dynamic-range {
    margin-left: auto;
}

.status-buttons .status-btn.dynamic-range.low {
    background: var(--ui-border);
    color: var(--ui-text);
}

.status-buttons .status-btn.dynamic-range.good {
    background: var(--ui-accent);
    color: var(--ui-panel);
}

.status-buttons .status-btn.dynamic-range.high {
    background: var(--ui-warning);
    color: var(--ui-panel);
}

.status-buttons .status-btn.dynamic-range.clipping {
    background: var(--ui-danger);
    color: var(--ui-panel);
}

.status-btn {
    padding: 12px 24px;
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.3s;
    white-space: nowrap;
}

.status-btn.recording {
    background: var(--ui-warning);
    color: var(--ui-panel);
}

.status-btn.idle {
    background: var(--ui-accent);
    color: var(--ui-panel);
}

.audio-levels {
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 32px;
    margin-bottom: 24px;
    padding: 16px;
    background: var(--ui-background);
    border-radius: 8px;
}

.level-item {
    display: flex;
    flex-direction: column;
    gap: 6px;
}

.level-item-left {
    text-align: left;
    align-items: flex-start;
}

.level-item-center {
    text-align: center;
    align-items: center;
}

.level-item-right {
    text-align: right;
    align-items: flex-end;
}

.level-label {
    font-size: 13px;
    font-weight: 600;
    color: var(--ui-muted);
}

.level-value {
    font-size: 20px;
    font-weight: 600;
    color: var(--ui-text);
    font-family: 'Courier New', monospace;
}

.audio-graph-container {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 8px;
    padding: 20px;
    margin-bottom: 24px;
}

.audio-graph-input-channel-bar {
    margin-top: 16px;
    padding-top: 16px;
    border-top: 1px solid var(--ui-border, #ddd);
}

.audio-graph-input-channel-inner {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 12px 20px;
}

.audio-graph-input-channel-label {
    font-weight: 600;
    font-size: 14px;
    color: var(--ui-text);
    display: inline-flex;
    align-items: center;
    gap: 6px;
}

.audio-graph-channel-fieldset {
    border: none;
    padding: 0;
    margin: 0;
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px 16px;
}

.audio-graph-channel-option {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    margin: 0;
    font-size: 14px;
}

#audio-graph {
    width: 100%;
    height: 300px;
}

.audio-controls {
    margin-top: 16px;
    display: grid;
    grid-template-columns: repeat(2, minmax(260px, 1fr));
    gap: 20px 32px;
}

.control-group {
    display: flex;
    flex-direction: column;
    gap: 10px;
}

.control-group label {
    font-size: 15px;
    font-weight: 500;
    color: var(--ui-muted);
}

.control-group label input[type="checkbox"] {
    margin-right: 8px;
}

.slider-container {
    display: flex;
    align-items: center;
    gap: 16px;
}

.slider {
    flex: 1;
    height: 6px;
    border-radius: 3px;
    background: var(--ui-muted);
    outline: none;
    -webkit-appearance: none;
}

.slider::-moz-range-track {
    background: var(--ui-muted);
}

.slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--ui-accent);
    cursor: pointer;
    border: 2px solid var(--ui-panel);
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
}

.slider::-moz-range-thumb {
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--ui-accent);
    cursor: pointer;
    border: 2px solid var(--ui-panel);
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
}

.slider-value {
    min-width: 60px;
    text-align: right;
    font-weight: 600;
    color: var(--ui-text);
    font-size: 15px;
}

.clipping-btn {
    padding: 12px 24px;
    background: var(--ui-danger);
    color: var(--ui-panel);
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.3s;
}

.clipping-btn:hover {
    transform: translateY(-2px);
    box-shadow: 0 4px 8px rgba(211, 35, 41, 0.3);
}

/* Network Page Styles */

.audio-settings-card {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 20px 20px 16px;
    margin-top: 16px;
}

.audio-actions {
    margin-top: 20px;
    display: flex;
    justify-content: flex-end;
}

.audio-actions .btn-primary,
.audio-actions .btn-secondary {
    min-width: 160px;
}
.wifi-config-card {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 24px;
    margin-bottom: 24px;
}

.wifi-config-card h3 {
    color: var(--ui-text);
    margin-bottom: 16px;
    font-size: 20px;
    font-weight: 600;
}

.wifi-credentials-row {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
    margin-bottom: 16px;
}

.wifi-ssid-group,
.wifi-password-group {
    margin-bottom: 0;
}

.checkbox-label {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    font-weight: 600;
    color: var(--ui-text);
    font-size: 14px;
}

.checkbox-label input[type="checkbox"] {
    width: 18px;
    height: 18px;
    cursor: pointer;
    accent-color: var(--ui-accent);
}

.static-ip-fields {
    margin-top: 16px;
    padding-top: 16px;
    border-top: 1px solid var(--ui-border);
}

/* Form Controls */
.form-group {
    margin-bottom: 16px;
}

.form-group label {
    display: block;
    margin-bottom: 6px;
    font-weight: 600;
    color: var(--ui-text);
    font-size: 14px;
}

.form-control {
    width: 100%;
    padding: 10px 12px;
    border: 2px solid var(--ui-border);
    border-radius: 8px;
    font-size: 15px;
    transition: all 0.3s ease;
    background: var(--ui-panel);
    color: var(--ui-muted);
    font-family: inherit;
}

input,
select,
textarea {
    background: var(--ui-surface);
    color: var(--ui-text);
    border-color: var(--ui-border);
}

.form-control:focus {
    outline: none;
    border-color: var(--ui-accent);
    box-shadow: 0 0 0 3px rgba(5, 135, 199, 0.1);
}

.btn-primary, .btn-secondary {
    padding: 12px 24px;
    border: none;
    border-radius: 8px;
    font-size: 15px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.3s ease;
}

.btn-primary:hover:not(:disabled),
.btn-secondary:hover:not(:disabled) {
    transform: translateY(-2px);
}

.btn-primary {
    background: var(--ui-accent);
    color: var(--ui-button-text);
    border-color: var(--ui-accent);
}

.btn-primary:hover:not(:disabled) {
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-accent) 35%, transparent);
}

.btn-primary:disabled {
    opacity: 0.6;
    cursor: not-allowed;
}

.btn-secondary {
    background: var(--ui-panel);
    color: var(--ui-accent);
    border: 2px solid var(--ui-accent);
}

/* Button semantic variants */
.btn-danger {
    background: var(--ui-danger);
    color: var(--ui-button-text);
    border-color: var(--ui-danger);
}

.btn-danger:hover:not(:disabled) {
    background: var(--ui-danger);
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-danger) 35%, transparent);
}

.btn-warning {
    background: var(--ui-warning);
    color: var(--ui-button-text);
    border-color: var(--ui-warning);
}

.btn-warning:hover:not(:disabled) {
    background: var(--ui-warning);
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-warning) 35%, transparent);
}

.btn-success {
    background: var(--ui-success);
    color: var(--ui-button-text);
    border-color: var(--ui-success);
}

.btn-success:hover:not(:disabled) {
    background: var(--ui-success);
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-success) 35%, transparent);
}

.btn-info {
    background: var(--ui-accent);
    color: var(--ui-button-text);
    border-color: var(--ui-accent);
}

.btn-info:hover:not(:disabled) {
    background: var(--ui-accent);
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-accent) 35%, transparent);
}

.btn-secondary:not(.btn-info):not(.btn-warning):not(.btn-danger):not(.btn-success):hover:not(:disabled) {
    background: var(--ui-accent);
    color: var(--ui-button-text);
    border-color: var(--ui-accent);
    box-shadow: 0 5px 15px color-mix(in srgb, var(--ui-accent) 35%, transparent);
}

button:focus-visible,
.btn-secondary:focus-visible {
    outline: 3px solid var(--ui-accent);
    outline-offset: 2px;
}

.btn-secondary:disabled {
    opacity: 0.6;
    cursor: not-allowed;
}

/* Recordings Page Styles */
.recordings-controls {
    display: flex;
    flex-direction: column;
    gap: 16px;
    margin-bottom: 24px;
}

.date-navigation {
    display: flex;
    align-items: center;
    gap: 16px;
    flex-wrap: wrap;
}

.date-display {
    flex: 1;
    min-width: 200px;
}

.date-display input[type="date"] {
    width: 100%;
    padding: 10px 12px;
    border: 2px solid var(--ui-border);
    border-radius: 8px;
    font-size: 15px;
}

.recordings-list {
    display: flex;
    flex-direction: column;
    gap: 12px;
    min-height: 300px;
}

.pagination {
    display: flex;
    justify-content: center;
    align-items: center;
    gap: 20px;
    margin-top: 24px;
    padding: 16px;
    flex-wrap: wrap;
}

.page-info {
    font-weight: 600;
    color: var(--ui-text);
    min-width: 120px;
    text-align: center;
    font-size: 14px;
}

.pagination button:disabled {
    opacity: 0.5;
    cursor: not-allowed;
}

.recording-item {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 8px;
    padding: 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 16px;
}

.recording-info {
    flex: 1;
    min-width: 0;
}

.recording-name {
    font-weight: 600;
    color: var(--ui-text);
    margin-bottom: 6px;
    font-size: 15px;
}

.recording-meta {
    font-size: 12px;
    color: var(--ui-muted);
}

.recording-actions {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
}

.inline-audio-player {
    flex: 1;
    min-width: 200px;
    max-width: 400px;
    height: 32px;
}

/* Logs Page Styles */
.logs-controls {
    display: flex;
    gap: 16px;
    margin-bottom: 24px;
    flex-wrap: wrap;
}

.logs-controls .form-control {
    flex: 1;
    min-width: 200px;
}

.log-content {
    background: var(--ui-muted);
    color: var(--ui-panel);
    padding: 20px;
    border-radius: 8px;
    font-family: 'Courier New', monospace;
    font-size: 12px;
    max-height: calc(100vh - 300px);
    overflow-y: auto;
    white-space: pre-wrap;
    word-wrap: break-word;
    line-height: 1.5;
}

.info-message {
    padding: 24px;
    text-align: center;
    color: var(--ui-muted);
    font-size: 15px;
}

.loading {
    text-align: center;
    padding: 40px;
    color: var(--ui-muted);
    font-size: 16px;
}

/* Helper text (narrative / instructions above controls) */
.form-help {
    margin: 0 0 1.25rem 0;
    line-height: 1.55;
    color: var(--ui-muted);
    font-size: 14px;
}

/* Advanced Page Styles */
.advanced-section {
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 24px;
    margin-bottom: 24px;
}

.advanced-section h3 {
    color: var(--ui-text);
    margin-bottom: 0.5rem;
    margin-top: 0;
    font-size: 20px;
    font-weight: 600;
}

.advanced-section h3 + .form-help {
    margin-top: 0;
    margin-bottom: 1.25rem;
}

.advanced-section .form-group {
    margin-bottom: 1.25rem;
}

.advanced-section .form-group:last-of-type {
    margin-bottom: 0;
}

.advanced-section .action-buttons {
    margin-top: 1.25rem;
    gap: 14px;
}

.theme-warning {
    background: color-mix(in srgb, var(--ui-warning) 14%, var(--ui-panel));
    border: 1px solid var(--ui-warning);
    color: var(--ui-text);
}

/* Upload Locations: region rows and custom row spacing */
.upload-regions-group {
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.upload-region-row {
    display: inline-flex;
    align-items: center;
    gap: 12px;
}

.upload-custom-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 16px;
}

#upload-custom-host-fields.upload-custom-fields {
    flex: 1 1 auto;
    flex-wrap: wrap;
    align-items: center;
    gap: 14px;
    min-width: 0;
}

#firmware-progress {
    margin-top: 16px;
}

#firmware-progress-bar {
    height: 24px;
    background: var(--ui-accent);
    border-radius: 4px;
    transition: width 0.3s ease;
}

#firmware-progress-text {
    margin-top: 8px;
    font-size: 13px;
    color: var(--ui-muted);
    text-align: center;
}

input[type="file"] {
    padding: 8px;
    border: 2px solid var(--ui-border);
    border-radius: 8px;
    background: var(--ui-panel);
    cursor: pointer;
    font-size: 14px;
}

input[type="file"]:hover {
    border-color: var(--ui-accent);
}

.action-buttons {
    display: flex;
    gap: 14px;
    flex-wrap: wrap;
    align-items: center;
}

/* Popup Overlay */
.popup-overlay {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.7);
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 10000;
}

.popup-content {
    background: var(--ui-panel);
    border-radius: 12px;
    padding: 32px;
    max-width: 450px;
    width: 90%;
    max-height: 90vh;
    overflow-y: auto;
    text-align: center;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.3);
}

.popup-spinner {
    border: 4px solid var(--ui-border);
    border-top: 4px solid var(--ui-accent);
    border-radius: 50%;
    width: 50px;
    height: 50px;
    animation: spin 1s linear infinite;
    margin: 0 auto 20px;
}

@keyframes spin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
}

/* Flashing status text for Recording/Uploading indicators */
.status-flash {
    animation: statusFlash 1s ease-in-out infinite;
}

@keyframes statusFlash {
    0% { opacity: 1; }
    50% { opacity: 0.2; }
    100% { opacity: 1; }
}

.popup-icon {
    font-size: 48px;
    margin-bottom: 16px;
}

.popup-title {
    font-size: 24px;
    font-weight: bold;
    color: var(--ui-muted);
    margin-bottom: 16px;
}

.popup-message {
    font-size: 16px;
    color: var(--ui-muted);
    margin-bottom: 24px;
    line-height: 1.5;
}

.popup-error .popup-content {
    border: 2px solid var(--ui-danger);
}

.popup-buttons {
    display: flex;
    gap: 12px;
    justify-content: center;
    flex-wrap: wrap;
}

.popup-buttons .btn-primary {
    padding: 10px 20px;
    font-size: 16px;
}

.countdown-display {
    margin: 24px 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
}

.countdown-number {
    font-size: 48px;
    font-weight: bold;
    color: var(--ui-accent);
    font-family: 'Courier New', monospace;
    line-height: 1;
}

.countdown-label {
    font-size: 14px;
    color: var(--ui-muted);
    text-transform: uppercase;
    letter-spacing: 1px;
}

/* Help icons for per-setting help */
.help-icon,
.help-icon-inline {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-left: 6px;
    font-size: 16px;
    cursor: pointer;
    color: var(--ui-accent);
    transition: transform 0.15s ease, color 0.15s ease;
}

.help-icon:hover,
.help-icon-inline:hover {
    transform: translateY(-1px);
    color: var(--ui-warning);
}

/* Global Modal (SPA) */
.global-modal .modal-header {
    padding: 0 0 12px 0;
    border-bottom: 1px solid rgba(0, 0, 0, 0.05);
    margin-bottom: 8px;
}

.global-modal .modal-icon {
    margin-right: 8px;
    font-size: 20px;
    vertical-align: middle;
}

.global-modal .modal-title {
    margin: 0;
    font-size: 20px;
    font-weight: 600;
    color: var(--ui-text);
}

.global-modal .modal-body {
    padding: 8px 0 16px 0;
}

.global-modal .modal-body p {
    margin: 0;
    font-size: 15px;
    color: var(--ui-muted);
    line-height: 1.5;
}

.global-modal .modal-footer {
    padding-top: 12px;
    border-top: 1px solid rgba(0, 0, 0, 0.05);
    display: flex;
    gap: 10px;
    justify-content: flex-end;
    flex-wrap: wrap;
}

.global-modal .modal-footer .btn-primary,
.global-modal .modal-footer .btn-secondary {
    min-width: 110px;
}

/* Tablet Styles (768px - 1024px) */
@media (min-width: 768px) and (max-width: 1024px) {
    .sidebar {
        width: 240px;
    }
    
    .content-header {
        padding: 18px 24px;
    }
    
    .page-title {
        font-size: 24px;
    }
    
    .content-body {
        padding: 24px;
    }
    
    .summary-grid {
        grid-template-columns: repeat(2, 1fr);
    }
    
    .wifi-credentials-row {
        grid-template-columns: 1fr;
    }
}

/* Mobile Styles (max-width: 767px) */
@media (max-width: 767px) {
    .app-container {
        flex-direction: column;
    }
    
    .sidebar {
        width: 100%;
        height: auto;
        max-height: 60px;
        overflow: hidden;
        transition: max-height 0.3s ease;
    }
    
    .sidebar.expanded {
        max-height: 100vh;
    }
    
    .sidebar-header {
        padding: 14px 16px;
        cursor: pointer;
        display: flex;
        justify-content: space-between;
        align-items: center;
    }
    
    .sidebar-header::after {
        content: '☰';
        font-size: 24px;
    }
    
    .sidebar.expanded .sidebar-header::after {
        content: '✕';
    }
    
    .sidebar-nav {
        display: none;
    }
    
    .sidebar.expanded .sidebar-nav {
        display: block;
    }
    
    .sidebar-footer {
        display: none;
    }
    
    .sidebar.expanded .sidebar-footer {
        display: block;
    }
    
    .main-content {
        width: 100%;
        height: calc(100vh - 60px);
    }
    
    .content-header {
        padding: 14px 16px;
        flex-wrap: wrap;
        gap: 10px;
    }
    
    .page-title {
        font-size: 20px;
    }
    
    .title-icon {
        font-size: 24px;
    }
    
    .header-time {
        font-size: 14px;
    }
    
    .header-timezone {
        font-size: 10px;
    }
    
    .content-body {
        padding: 16px;
    }
    
    .summary-grid {
        grid-template-columns: 1fr;
        gap: 16px;
    }
    
    .audio-levels {
        flex-direction: column;
        gap: 16px;
    }
    
    .audio-controls {
        grid-template-columns: 1fr;
    }
    
    .status-buttons {
        flex-direction: column;
        align-items: stretch;
    }
    
    .status-buttons .status-btn.dynamic-range {
        margin-left: 0;
    }
    
    .status-btn {
        width: 100%;
    }
    
    .audio-graph-container {
        padding: 12px;
    }

    .audio-graph-input-channel-inner {
        flex-direction: column;
        align-items: flex-start;
    }
    
    #audio-graph {
        height: 200px;
    }
    
    .wifi-credentials-row {
        grid-template-columns: 1fr;
        gap: 12px;
    }
    
    .wifi-config-card {
        padding: 16px;
    }
    
    .recordings-controls,
    .logs-controls {
        flex-direction: column;
    }
    
    .recordings-controls .form-control,
    .logs-controls .form-control {
        width: 100%;
    }
    
    .date-navigation {
        flex-direction: column;
        gap: 12px;
    }
    
    .date-navigation button {
        width: 100%;
    }
    
    .pagination {
        flex-direction: column;
        gap: 12px;
    }
    
    .pagination button {
        width: 100%;
    }
    
    .recording-item {
        flex-direction: column;
        align-items: flex-start;
        gap: 12px;
    }
    
    .recording-actions {
        width: 100%;
    }
    
    .recording-actions .btn-secondary {
        width: 100%;
    }
    
    .action-buttons {
        flex-direction: column;
    }
    
    .action-buttons .btn-primary,
    .action-buttons .btn-secondary {
        width: 100%;
    }
    
    .advanced-section {
        padding: 16px;
    }
    
    .popup-content {
        padding: 24px;
        width: 95%;
    }
}

/* Small Mobile Styles (max-width: 480px) */
@media (max-width: 480px) {
    .content-header {
        padding: 12px 14px;
    }
    
    .page-title {
        font-size: 18px;
    }
    
    .content-body {
        padding: 12px;
    }
    
    .summary-card {
        padding: 16px;
    }
    
    .info-row {
        flex-direction: column;
        gap: 4px;
    }
    
    .level-value {
        font-size: 18px;
    }
    
    #audio-graph {
        height: 150px;
    }
    
    .terminal-output {
        font-size: 11px;
        padding: 12px;
    }
}

/* Large Screen Optimization (1920px and above - 1080p) */
@media (min-width: 1920px) {
    .sidebar {
        width: 300px;
    }
    
    .content-header {
        padding: 24px 40px;
    }
    
    .content-body {
        padding: 40px;
    }
    
    .summary-grid {
        grid-template-columns: repeat(4, 1fr);
    }
    
    #audio-graph {
        height: 350px;
    }
}

/* Live Audio Page Styles */
/* Live Audio: compact hint under session heading */
.live-audio-session-hint {
    font-size: 12px;
    line-height: 1.4;
    margin: 0 0 10px 0;
}

.live-audio-container {
    display: flex;
    flex-direction: column;
    gap: 24px;
}

.live-audio-status {
    display: flex;
    flex-direction: column;
    gap: 20px;
    background: var(--ui-panel);
    border: 2px solid var(--ui-border);
    border-radius: 12px;
    padding: 20px;
}

.live-audio-controls-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.status-indicator {
    display: flex;
    align-items: center;
    gap: 12px;
}

.status-dot {
    width: 12px;
    height: 12px;
    border-radius: 50%;
    background: #999;
    flex-shrink: 0;
}

.status-dot.pulsing {
    animation: pulse 2s infinite;
}

/* --- CW (Morse) page --- */
.cw-layout {
  display: grid;
  grid-template-columns: 1fr;
  gap: 16px;
}
.cw-panel {
  background: var(--card-bg, #fff);
  border: 1px solid var(--ui-border, #e5e7eb);
  border-radius: 12px;
  padding: 14px;
  box-shadow: 0 1px 1px rgba(0,0,0,0.03);
}
.cw-settings-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
  margin-top: 10px;
}
@media (max-width: 900px) {
  .cw-settings-grid { grid-template-columns: 1fr; }
}
.cw-field label { display:block; margin-bottom:6px; font-weight: 600; }
.cw-preset-actions { display:flex; gap:8px; margin-top:8px; flex-wrap:wrap; }
.cw-message { margin-top: 12px; }
.cw-textarea { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; }
.cw-actions { display:flex; gap:10px; margin-top:10px; flex-wrap:wrap; }
.cw-status { margin-top:8px; }
.cw-history { display:flex; flex-direction:column; gap: 10px; }
.cw-history {
  max-height: 340px; /* ~5 items */
  overflow: auto;
  padding-right: 4px;
}
.cw-hrow { display:flex; gap:10px; justify-content:space-between; align-items:flex-start; padding: 10px; border-radius: 10px; border: 1px solid rgba(0,0,0,0.10); background: rgba(255,255,255,0.7); }
.cw-hleft { display:flex; flex-direction:column; gap: 4px; }
.cw-htxt { font-weight: 700; }
.cw-hmeta { font-size: 12px; color: var(--ui-muted, #6b7280); }
.cw-history-actions { margin-top: 10px; }

@keyframes pulse {
    0%, 100% {
        opacity: 1;
    }
    50% {
        opacity: 0.5;
    }
}

#status-text {
    font-size: 16px;
    font-weight: 500;
    color: var(--ui-muted);
}

.audio-controls-live {
    display: flex;
    gap: 12px;
}

.live-audio-info {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
    gap: 20px;
}

.live-audio-visualizer {
    width: 100%;
    margin-top: 10px;
}

#live-audio-visualizer-canvas {
    width: 100%;
    height: 200px;
    display: block;
    border-radius: 8px;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

/* Recordings (inbox browser) */
.recordings-page {
    width: 100%;
    height: 100%;
    min-height: 0;
}

.recordings-notice-banner {
    background: var(--warning-bg, #fff3cd);
    border: 1px solid var(--warning-border, #ffc107);
    border-radius: 6px;
    padding: 10px 12px;
    margin-bottom: 12px;
    font-size: 13px;
    color: var(--ui-muted);
    line-height: 1.45;
}

.recordings-notice-banner .recordings-notice-lead {
    margin: 0;
}

.recordings-notice-banner .recordings-notice-sub {
    margin: 8px 0 0 0;
    padding-top: 8px;
    border-top: 1px solid rgba(0, 0, 0, 0.08);
    color: var(--ui-muted);
}

.recordings-layout {
    display: flex;
    flex-direction: row;
    gap: 16px;
    align-items: stretch;
    min-height: 360px;
    max-height: calc(100vh - 220px);
}

.recordings-pane {
    background: var(--ui-panel);
    color: var(--ui-text);
    border: 1px solid var(--ui-border);
    border-radius: 8px;
    padding: 12px 14px;
    box-shadow: none;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    min-height: 0;
}

.recordings-pane-tree {
    flex: 0 0 280px;
    max-width: 320px;
}

.recordings-pane-list {
    flex: 1 1 auto;
    min-width: 0;
}

.recordings-pane-title {
    font-size: 15px;
    margin: 0 0 10px 0;
    color: var(--ui-text);
}

.recordings-tree-root {
    overflow-y: auto;
    flex: 1 1 auto;
    min-height: 120px;
    font-size: 14px;
}

.recordings-tree-muted {
    font-size: 13px;
    color: var(--ui-muted);
    opacity: 0.85;
    padding-left: 4px;
}

.recordings-tree-row {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 4px;
}

.recordings-tree-toggle {
    width: 26px;
    height: 26px;
    padding: 0;
    border: 1px solid var(--ui-accent);
    background: var(--ui-background);
    border-radius: 4px;
    cursor: pointer;
    font-weight: bold;
    line-height: 1;
    flex-shrink: 0;
}

.recordings-tree-spacer {
    display: inline-block;
    width: 26px;
    flex-shrink: 0;
}

.recordings-tree-label {
    border: none;
    background: transparent;
    color: var(--ui-text);
    text-align: left;
    cursor: pointer;
    padding: 4px 6px;
    border-radius: 4px;
    flex: 1 1 auto;
}

.recordings-tree-label:hover {
    background: rgba(5, 135, 199, 0.12);
}

.recordings-tree-label.active {
    background: rgba(5, 135, 199, 0.22);
    font-weight: 600;
}

.recordings-day-label {
    font-size: 13px;
    color: var(--ui-muted);
    margin-bottom: 10px;
}

.recordings-table-loading {
    display: none;
    align-items: center;
    gap: 10px;
    margin-bottom: 10px;
    font-size: 13px;
    color: var(--ui-muted);
}

.recordings-table-loading-text {
    line-height: 1.3;
}

.recordings-spinner {
    display: inline-block;
    width: 18px;
    height: 18px;
    border: 2px solid rgba(5, 135, 199, 0.25);
    border-top-color: var(--ui-accent);
    border-radius: 50%;
    animation: recordings-spin 0.65s linear infinite;
    flex-shrink: 0;
    vertical-align: middle;
}

.recordings-tree-row-spinner {
    width: 14px;
    height: 14px;
    border-width: 2px;
}

@keyframes recordings-spin {
    to {
        transform: rotate(360deg);
    }
}

.recordings-table-row-enter {
    opacity: 0;
    transform: translateY(4px);
}

.recordings-table-row-enter.recordings-table-row-visible {
    opacity: 1;
    transform: translateY(0);
    transition: opacity 0.18s ease, transform 0.18s ease;
}

.recordings-table-empty {
    color: var(--ui-muted);
    font-style: italic;
    padding: 12px 10px;
}

.recordings-table-wrap {
    overflow-x: auto;
    flex: 1 1 auto;
    min-height: 0;
}

.recordings-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
}

.recordings-table th,
.recordings-table td {
    border-bottom: 1px solid rgba(0, 0, 0, 0.08);
    padding: 8px 10px;
    text-align: left;
}

.recordings-table th {
    background: rgba(5, 135, 199, 0.1);
    color: var(--ui-text);
    font-weight: 600;
}

/* Download uses <a>; strip default link underline so it matches Play (button). */
.recordings-table a.recordings-action-btn {
    text-decoration: none;
    display: inline-block;
    box-sizing: border-box;
}

.recordings-table a.recordings-action-btn:hover,
.recordings-table a.recordings-action-btn:focus {
    text-decoration: none;
}

.recordings-pagination-bar {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 12px;
    margin-top: 12px;
    flex-wrap: wrap;
    width: 100%;
}

.recordings-pagination-bar .recordings-pagination {
    margin-right: auto;
    margin-top: 0;
}

.recordings-pagination {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 12px;
    flex-wrap: wrap;
}

.recordings-refresh-btn {
    flex-shrink: 0;
}

.recordings-player {
    width: 100%;
    max-width: 420px;
    margin-top: 14px;
}

.recordings-error {
    color: var(--ui-danger);
    font-size: 13px;
    margin-top: 8px;
}

@media (max-width: 900px) {
    .recordings-layout {
        flex-direction: column;
        max-height: none;
    }
    .recordings-pane-tree {
        flex: 0 0 auto;
        max-width: none;
    }
}
)";

#endif // WEB_CSS_H
