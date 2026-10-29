#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/* ================= WIFI CONFIGURATION ================= */
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Fallback Access Point (if router connection fails)
const char* ap_ssid  = "ESP32-Draw-AP";
const char* ap_pass  = "12345678";

/* ================= MATRIX CONFIGURATION ================= */
#define PANEL_WIDTH      64
#define PANEL_HEIGHT     64   // Panel height of 64 requires PIN_E to be defined
#define PANELS_NUMBER    2    // 2 daisy-chained panels
#define PIN_E            21
#define PIN_D            2
#define DEFAULT_BRIGHTNESS 120  // Initial brightness (0-150)

// Invert clock phase: fixes last-column (x=127) mirroring onto first-column (x=0)
#define MATRIX_CLKPHASE  true

#define MATRIX_WIDTH  (PANEL_WIDTH * PANELS_NUMBER) // 128 pixels
#define MATRIX_HEIGHT PANEL_HEIGHT                   // 64 pixels

MatrixPanel_I2S_DMA* dma_display = nullptr;

/* ================= SECURITY & ACCESS CONTROL ================= */
#define REQUIRE_AUTH true
const char* ACCESS_PIN = "1234"; // Set your custom PIN (4-8 digits)

#define MAX_WS_CLIENTS 16
bool clientAuthenticated[MAX_WS_CLIENTS] = { false };

// Anti-Spam / Anti-Video Flood: Max 1 image upload every 1500ms
unsigned long lastImageUploadMs = 0;
const unsigned long MIN_IMAGE_INTERVAL_MS = 1500;

/* ================= SERVERS & STATE ================= */
WebServer server(80);
WebSocketsServer webSocket(81);

// Persistent Framebuffer to sync matrix with newly connected web clients
uint16_t canvasBuffer[MATRIX_WIDTH * MATRIX_HEIGHT];

/* ================= WEB UI (HTML / CSS / JS) ================= */
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
  <title>HUB75 Live Matrix (128x64)</title>
  <style>
    :root {
      --bg-color: #0f1117;
      --card-bg: #1a1d26;
      --border-color: #2e3446;
      --text-color: #e2e8f0;
      --text-muted: #94a3b8;
      --accent: #3b82f6;
      --accent-hover: #2563eb;
      --success: #22c55e;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; user-select: none; -webkit-tap-highlight-color: transparent; }
    body {
      background-color: var(--bg-color);
      color: var(--text-color);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 12px;
      overflow-x: hidden;
    }
    header {
      width: 100%; max-width: 1040px;
      display: flex; justify-content: space-between; align-items: center;
      margin-bottom: 12px; flex-wrap: wrap; gap: 8px;
    }
    .title-group h1 { font-size: 1.2rem; font-weight: 700; display: flex; align-items: center; gap: 6px; }
    .badge { font-size: 0.7rem; background: #2563eb33; color: #60a5fa; padding: 2px 6px; border-radius: 9999px; border: 1px solid #3b82f644; }
    .status-badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 20px; font-size: 0.8rem; background: var(--card-bg); border: 1px solid var(--border-color); }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; background: #ef4444; box-shadow: 0 0 6px #ef4444; transition: all 0.3s ease; }
    .status-dot.connected { background: #22c55e; box-shadow: 0 0 8px #22c55e; }
    
    .workspace { width: 100%; max-width: 1040px; display: flex; flex-direction: column; align-items: center; gap: 12px; }
    
    /* Responsive Canvas Card */
    .canvas-card {
      background: #000; padding: 8px; border-radius: 12px; border: 2px solid var(--border-color);
      box-shadow: 0 8px 24px rgba(0,0,0,0.6); width: 100%; display: flex; flex-direction: column; align-items: center;
      touch-action: none;
    }
    .canvas-wrapper {
      position: relative;
      width: 100%;
      max-width: 896px;
      aspect-ratio: 128 / 64;
      touch-action: none;
      background: #050505;
      border: 1px solid #333;
      border-radius: 6px;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    canvas#matrixCanvas {
      width: 100% !important;
      height: 100% !important;
      display: block;
      image-rendering: pixelated;
      cursor: crosshair;
      touch-action: none;
      user-select: none;
      -webkit-user-select: none;
    }
    .canvas-info {
      width: 100%; display: flex; justify-content: space-between; align-items: center;
      color: var(--text-muted); font-size: 0.72rem; margin-top: 6px; font-family: monospace;
      flex-wrap: wrap; gap: 4px;
    }
    
    .controls-grid { width: 100%; display: grid; grid-template-columns: 1fr; gap: 12px; }
    @media (min-width: 768px) { .controls-grid { grid-template-columns: 1fr 1fr; } }
    
    .panel { background: var(--card-bg); border: 1px solid var(--border-color); border-radius: 12px; padding: 14px; display: flex; flex-direction: column; gap: 10px; }
    .panel-title { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-muted); font-weight: 700; }
    
    .palette { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
    .color-swatch { width: 38px; height: 38px; border-radius: 8px; border: 2px solid transparent; cursor: pointer; transition: transform 0.15s; outline: none; touch-action: manipulation; }
    .color-swatch:hover { transform: scale(1.08); }
    .color-swatch.active { border-color: #fff; box-shadow: 0 0 10px rgba(255,255,255,0.6); transform: scale(1.05); }
    .color-picker-wrapper { position: relative; width: 38px; height: 38px; border-radius: 8px; overflow: hidden; border: 2px solid var(--border-color); cursor: pointer; display: flex; align-items: center; justify-content: center; background: conic-gradient(red, yellow, lime, aqua, blue, magenta, red); touch-action: manipulation; }
    .color-picker-wrapper input[type="color"] { position: absolute; top: -10px; left: -10px; width: 60px; height: 60px; opacity: 0; cursor: pointer; }
    
    .tool-group { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
    .btn {
      background: #272c3d; color: var(--text-color); border: 1px solid var(--border-color);
      padding: 8px 12px; border-radius: 8px; cursor: pointer; font-size: 0.82rem; font-weight: 600;
      display: inline-flex; align-items: center; justify-content: center; gap: 6px; transition: background 0.15s;
      touch-action: manipulation; min-height: 38px;
    }
    .btn:hover { background: #333a4f; }
    .btn:active { transform: scale(0.97); }
    .btn.active { background: var(--accent); border-color: var(--accent-hover); color: #fff; }
    .btn-success { background: #1b4332; border-color: #2d6a4f; color: #d8f3dc; }
    .btn-success:hover { background: #2d6a4f; }
    .btn-danger { background: #451a1a; border-color: #7f1d1d; color: #fca5a5; }
    .btn-danger:hover { background: #5c2020; }
    .upload-status { font-size: 0.8rem; color: #60a5fa; font-family: monospace; display: none; }

    /* Sliders */
    .slider-row { display: flex; align-items: center; gap: 10px; width: 100%; }
    .slider-row input[type="range"] {
      flex: 1; accent-color: var(--accent); cursor: pointer; height: 8px; border-radius: 4px;
      touch-action: manipulation;
    }

    /* Crop Modal */
    .modal-overlay {
      display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;
      background: rgba(0,0,0,0.75); backdrop-filter: blur(5px); z-index: 100;
      align-items: center; justify-content: center; padding: 12px;
    }
    .modal-overlay.open { display: flex; }
    .modal-box {
      background: var(--card-bg); border: 1px solid var(--border-color); border-radius: 12px;
      padding: 18px; width: 100%; max-width: 540px; box-shadow: 0 20px 40px rgba(0,0,0,0.7);
      max-height: 92vh; overflow-y: auto;
    }
    .modal-box h3 { margin-bottom: 6px; font-size: 1.1rem; }
    .crop-container {
      position: relative; width: 100%; max-height: 280px;
      background: #000; border-radius: 8px; overflow: hidden;
      display: flex; align-items: center; justify-content: center;
      margin: 10px 0; touch-action: none;
    }
    #cropCanvas { display: block; max-width: 100%; max-height: 260px; cursor: move; touch-action: none; }
    .crop-controls {
      display: flex; align-items: center; gap: 10px; margin-bottom: 12px;
      font-size: 0.82rem; color: var(--text-muted);
    }
    .crop-controls input[type="range"] {
      flex: 1; accent-color: var(--accent); cursor: pointer; touch-action: manipulation;
    }
  </style>
</head>
<body>
  <header>
    <div class="title-group">
      <h1>HUB75 Live Matrix <span class="badge">128 × 64 px</span></h1>
    </div>
    <div style="display: flex; align-items: center; gap: 8px;">
      <button class="btn" id="pinSettingsBtn" style="padding: 4px 10px; font-size: 0.75rem;" title="Access PIN Settings">🔒 PIN</button>
      <div class="status-badge">
        <div id="statusDot" class="status-dot"></div>
        <span id="statusText">Connecting...</span>
      </div>
    </div>
  </header>

  <div class="workspace">
    <div class="canvas-card">
      <div class="canvas-wrapper">
        <canvas id="matrixCanvas" width="128" height="64"></canvas>
      </div>
      <div class="canvas-info">
        <span id="cursorPos">X: --, Y: --</span>
        <span id="uploadStatusText" class="upload-status">Uploading: 0%</span>
        <span>DAISYCHAIN 2×(64×64)</span>
      </div>
    </div>

    <div class="controls-grid">
      <!-- Colors & Photo Upload -->
      <div class="panel">
        <div class="panel-title">Colors & Palette</div>
        <div class="palette" id="palette">
          <button class="color-swatch active" style="background: #ff0000;" data-color="#ff0000" title="Red"></button>
          <button class="color-swatch" style="background: #00ff00;" data-color="#00ff00" title="Green"></button>
          <button class="color-swatch" style="background: #0088ff;" data-color="#0088ff" title="Blue"></button>
          <button class="color-swatch" style="background: #ffff00;" data-color="#ffff00" title="Yellow"></button>
          <button class="color-swatch" style="background: #ff8800;" data-color="#ff8800" title="Orange"></button>
          <button class="color-swatch" style="background: #00ffff;" data-color="#00ffff" title="Cyan"></button>
          <button class="color-swatch" style="background: #ff00ff;" data-color="#ff00ff" title="Magenta"></button>
          <button class="color-swatch" style="background: #ffffff;" data-color="#ffffff" title="White"></button>
          <button class="color-swatch" style="background: #ff66cc;" data-color="#ff66cc" title="Pink"></button>
          <button class="color-swatch" style="background: #8800ff;" data-color="#8800ff" title="Purple"></button>
          <div class="color-picker-wrapper" title="Custom Color">
            <input type="color" id="customColorPicker" value="#ff0000">
          </div>
        </div>

        <div class="panel-title" style="margin-top: 6px;">Photo Upload</div>
        <div class="tool-group">
          <input type="file" id="imageInput" accept="image/*" style="display:none">
          <button class="btn btn-success" id="uploadBtn">📷 Choose & Crop Photo</button>
        </div>
      </div>

      <!-- Drawing Tools -->
      <div class="panel">
        <div class="panel-title">Drawing Tools</div>
        <div class="tool-group">
          <button class="btn active" id="penBtn">✏️ Draw</button>
          <button class="btn" id="eraserBtn">🧽 Eraser</button>
          <button class="btn active" id="brush1">1px</button>
          <button class="btn" id="brush2">2px</button>
          <button class="btn" id="brush3">3px</button>
        </div>

        <div class="panel-title" style="margin-top: 6px;">Canvas Actions</div>
        <div class="tool-group">
          <button class="btn btn-danger" id="clearBtn">🗑️ Clear</button>
          <button class="btn" id="fillBtn">🪣 Fill Color</button>
          <button class="btn" id="syncBtn">🔄 Sync</button>
        </div>
      </div>

      <!-- Matrix Brightness (0 - 150) -->
      <div class="panel" style="grid-column: 1 / -1;">
        <div style="display: flex; justify-content: space-between; align-items: center;">
          <div class="panel-title">💡 Matrix Brightness (0 - 150)</div>
          <span id="brightnessVal" style="font-family: monospace; font-weight: 700; color: #60a5fa; font-size: 0.85rem;">120</span>
        </div>
        <div class="slider-row">
          <span style="font-size: 1rem;">🌑</span>
          <input type="range" id="brightnessSlider" min="0" max="150" value="120" step="1">
          <span style="font-size: 1rem;">🌕</span>
        </div>
      </div>
    </div>
  </div>

  <!-- Browser-Side Photo Crop/Cut Modal (100% Client-Side, 0 ESP32 Overhead) -->
  <div class="modal-overlay" id="cropModal">
    <div class="modal-box">
      <h3>✂️ Cut / Crop Photo (128 × 64)</h3>
      <p style="font-size: 0.8rem; color: var(--text-muted);">
        Drag the image or use the zoom slider to frame your photo perfectly for the display.
      </p>

      <div class="crop-container">
        <canvas id="cropCanvas" width="512" height="256"></canvas>
      </div>

      <div class="crop-controls">
        <span>Zoom:</span>
        <input type="range" id="zoomSlider" min="0.1" max="4" step="0.02" value="1">
        <span id="zoomVal" style="font-family: monospace; min-width: 38px;">1.0x</span>
        <button class="btn" id="resetCropBtn" style="padding: 4px 8px; font-size: 0.75rem;">Reset</button>
      </div>

      <div style="display:flex; justify-content: flex-end; gap: 8px;">
        <button class="btn" id="cancelCropBtn">Cancel</button>
        <button class="btn btn-success" id="applyCropBtn">✂️ Apply & Send to Matrix</button>
      </div>
    </div>
  </div>

  <!-- PIN Authentication Modal -->
  <div class="modal-overlay" id="pinModal">
    <div class="modal-box" style="max-width: 380px; text-align: center;">
      <div style="font-size: 2.2rem; margin-bottom: 8px;">🔒</div>
      <h3 style="margin-bottom: 6px;">Matrix Access PIN</h3>
      <p style="font-size: 0.8rem; color: var(--text-muted); margin-bottom: 14px;">
        This matrix is protected with a security PIN. Enter PIN to draw or upload images.
      </p>
      <input type="password" id="pinInput" placeholder="Enter PIN"
             style="width: 100%; padding: 10px 14px; border-radius: 8px; border: 1px solid var(--border-color); background: #12151e; color: #fff; font-size: 1.15rem; text-align: center; letter-spacing: 0.25em; margin-bottom: 10px; outline: none;">
      <div id="pinErrorMsg" style="color: #ef4444; font-size: 0.8rem; margin-bottom: 10px; display: none;">❌ Incorrect PIN. Please try again.</div>
      <div style="display: flex; gap: 8px; justify-content: center;">
        <button class="btn" id="closePinModalBtn" style="flex: 1; justify-content: center;">Cancel</button>
        <button class="btn btn-success" id="submitPinBtn" style="flex: 1; justify-content: center;">Unlock</button>
      </div>
    </div>
  </div>

  <script>
    const WIDTH = 128, HEIGHT = 64;
    const canvas = document.getElementById('matrixCanvas');
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    const cursorPos = document.getElementById('cursorPos');
    const statusDot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const uploadStatusText = document.getElementById('uploadStatusText');
    const brightnessSlider = document.getElementById('brightnessSlider');
    const brightnessVal = document.getElementById('brightnessVal');

    // PIN Authentication
    let currentPin = localStorage.getItem('matrix_pin') || '';
    const pinModal = document.getElementById('pinModal');
    const pinInput = document.getElementById('pinInput');
    const pinErrorMsg = document.getElementById('pinErrorMsg');
    const submitPinBtn = document.getElementById('submitPinBtn');
    const closePinModalBtn = document.getElementById('closePinModalBtn');
    const pinSettingsBtn = document.getElementById('pinSettingsBtn');

    function openPinModal() {
      pinModal.classList.add('open');
      pinInput.value = currentPin;
      pinErrorMsg.style.display = 'none';
      setTimeout(() => pinInput.focus(), 150);
    }
    function closePinModal() {
      pinModal.classList.remove('open');
    }
    pinSettingsBtn.addEventListener('click', openPinModal);
    closePinModalBtn.addEventListener('click', closePinModal);

    function sendAuth(pin) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      const encoder = new TextEncoder();
      const pinBytes = encoder.encode(pin);
      const buf = new Uint8Array(1 + pinBytes.length);
      buf[0] = 0x00; // Auth command
      buf.set(pinBytes, 1);
      ws.send(buf.buffer);
    }

    submitPinBtn.addEventListener('click', () => {
      const val = pinInput.value.trim();
      if (!val) return;
      currentPin = val;
      localStorage.setItem('matrix_pin', val);
      sendAuth(currentPin);
    });
    pinInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') submitPinBtn.click();
    });

    let isDrawing = false, currentColor = '#ff0000', currentTool = 'pen', brushSize = 1;
    let lastX = -1, lastY = -1;
    let ws = null, wsHost = window.location.hostname || '192.168.4.1', wsPort = 81;
    let isUploadingPhoto = false;

    let pendingPoints = [];
    let flushTimer = null;
    let canvasRect = null;

    function updateCanvasRect() {
      canvasRect = canvas.getBoundingClientRect();
    }
    window.addEventListener('resize', updateCanvasRect);
    window.addEventListener('scroll', updateCanvasRect, true);

    ctx.fillStyle = '#000000'; ctx.fillRect(0, 0, WIDTH, HEIGHT);

    function getWebSocketUrl() {
      const isHttps = window.location.protocol === 'https:';
      const host = window.location.host;
      const hostname = window.location.hostname || '192.168.4.1';
      const isDomain = hostname && !/^(\d{1,3}\.){3}\d{1,3}$/.test(hostname) && hostname !== 'localhost';

      if (isHttps || isDomain) {
        // Reverse-proxied via Cloudflare Tunnel / Nginx
        const proto = isHttps ? 'wss://' : 'ws://';
        return `${proto}${host}/ws`;
      } else {
        // Direct local LAN access to ESP32 (e.g., http://192.168.1.150)
        return `ws://${hostname}:81`;
      }
    }

    function connectWebSocket() {
      const url = getWebSocketUrl();
      statusText.textContent = 'Connecting...';
      statusDot.className = 'status-dot';
      try {
        ws = new WebSocket(url);
        ws.binaryType = 'arraybuffer';
        ws.onopen = () => {
          statusDot.className = 'status-dot connected';
          if (currentPin) {
            statusText.textContent = 'Authenticating...';
            sendAuth(currentPin);
          } else {
            statusText.textContent = 'Locked (Enter PIN)';
            openPinModal();
          }
        };
        ws.onclose = () => {
          statusDot.className = 'status-dot';
          statusText.textContent = 'Disconnected (Retrying...)';
          setTimeout(connectWebSocket, 2000);
        };
        ws.onerror = () => { statusDot.className = 'status-dot'; };
        ws.onmessage = (e) => {
          if (e.data instanceof ArrayBuffer) handleBinaryMessage(new Uint8Array(e.data));
        };
      } catch (e) { setTimeout(connectWebSocket, 3000); }
    }

    function hexToRgb(hex) {
      const bigint = parseInt(hex.replace('#', ''), 16);
      return { r: (bigint >> 16) & 255, g: (bigint >> 8) & 255, b: bigint & 255 };
    }

    function rgbTo565(r, g, b) {
      return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    function sendLine(x0, y0, x1, y1, colorHex, size) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      const rgb = (currentTool === 'eraser') ? { r: 0, g: 0, b: 0 } : hexToRgb(colorHex);
      ws.send(new Uint8Array([0x02, x0, y0, x1, y1, rgb.r, rgb.g, rgb.b, size]).buffer);
    }

    function sendClear() { if (ws && ws.readyState === WebSocket.OPEN) ws.send(new Uint8Array([0x03]).buffer); }
    function sendFill(colorHex) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      const rgb = hexToRgb(colorHex);
      ws.send(new Uint8Array([0x04, rgb.r, rgb.g, rgb.b]).buffer);
    }
    function sendSyncRequest() { if (ws && ws.readyState === WebSocket.OPEN) ws.send(new Uint8Array([0x06]).buffer); }

    function sendBrightness(val) {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      const b = Math.max(0, Math.min(150, parseInt(val, 10)));
      ws.send(new Uint8Array([0x07, b]).buffer);
    }

    let brightnessDebounce = null;
    brightnessSlider.addEventListener('input', (e) => {
      const val = e.target.value;
      brightnessVal.textContent = val;
      clearTimeout(brightnessDebounce);
      brightnessDebounce = setTimeout(() => {
        sendBrightness(val);
      }, 40);
    });
    async function uploadCanvasToMatrix() {
      if (!ws || ws.readyState !== WebSocket.OPEN || isUploadingPhoto) return;
      isUploadingPhoto = true;

      uploadStatusText.style.display = 'inline';
      uploadStatusText.textContent = 'Uploading: 0%';

      const imgData = ctx.getImageData(0, 0, WIDTH, HEIGHT);
      const rowsPerChunk = 4;
      const totalChunks = HEIGHT / rowsPerChunk; // 16 chunks

      try {
        for (let c = 0; c < totalChunks; c++) {
          if (!ws || ws.readyState !== WebSocket.OPEN) break;
          const startRow = c * rowsPerChunk;
          const buf = new Uint8Array(3 + rowsPerChunk * WIDTH * 2);
          buf[0] = 0x08;
          buf[1] = startRow;
          buf[2] = rowsPerChunk;

          let idx = 3;
          for (let r = 0; r < rowsPerChunk; r++) {
            const y = startRow + r;
            for (let x = 0; x < WIDTH; x++) {
              const pixelIdx = (y * WIDTH + x) * 4;
              const c565 = rgbTo565(
                imgData.data[pixelIdx],
                imgData.data[pixelIdx + 1],
                imgData.data[pixelIdx + 2]
              );
              buf[idx++] = c565 & 0xFF;
              buf[idx++] = (c565 >> 8) & 0xFF;
            }
          }

          ws.send(buf.buffer);
          const percent = Math.round(((c + 1) / totalChunks) * 100);
          uploadStatusText.textContent = `Uploading: ${percent}%`;

          await new Promise(r => setTimeout(r, 5));
        }

        uploadStatusText.textContent = 'Done!';
        setTimeout(() => { uploadStatusText.style.display = 'none'; }, 2000);
      } catch (err) {
        uploadStatusText.textContent = 'Upload Error';
      } finally {
        isUploadingPhoto = false;
      }
    }

    function handleBinaryMessage(data) {
      if (data.length === 0) return;
      const cmd = data[0];

      if (cmd === 0x00 && data.length >= 2) {
        if (data[1] === 0x01) {
          // Auth Success
          closePinModal();
          statusDot.className = 'status-dot connected';
          statusText.textContent = 'Live Connected';
          sendSyncRequest();
          sendBrightness(brightnessSlider.value);
        } else {
          // Auth Failed
          statusText.textContent = 'Auth Failed';
          statusDot.className = 'status-dot';
          pinErrorMsg.style.display = 'block';
          openPinModal();
        }
        return;
      }

      if (cmd === 0x08 && data.length >= 3) {
        const startY = data[1];
        const numRows = data[2];
        const expectedBytes = 3 + numRows * WIDTH * 2;
        if (data.length < expectedBytes) return;

        const imgData = ctx.getImageData(0, startY, WIDTH, numRows);
        let srcIdx = 3, dstIdx = 0;

        for (let i = 0; i < numRows * WIDTH; i++) {
          const c565 = data[srcIdx] | (data[srcIdx + 1] << 8);
          srcIdx += 2;
          imgData.data[dstIdx++] = Math.round(((c565 >> 11) & 0x1F) * 255 / 31);
          imgData.data[dstIdx++] = Math.round(((c565 >> 5) & 0x3F) * 255 / 63);
          imgData.data[dstIdx++] = Math.round((c565 & 0x1F) * 255 / 31);
          imgData.data[dstIdx++] = 255;
        }
        ctx.putImageData(imgData, 0, startY);
      } else if (cmd === 0x03) {
        ctx.fillStyle = '#000000'; ctx.fillRect(0, 0, WIDTH, HEIGHT);
      } else if (cmd === 0x04 && data.length >= 4) {
        ctx.fillStyle = `rgb(${data[1]},${data[2]},${data[3]})`; ctx.fillRect(0, 0, WIDTH, HEIGHT);
      }
    }

    function drawPoint(x, y, color, size) {
      ctx.fillStyle = color;
      const offset = Math.floor(size / 2);
      ctx.fillRect(x - offset, y - offset, size, size);
    }

    function drawLineLocal(x0, y0, x1, y1, color, size) {
      const dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0);
      const sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;
      let err = dx - dy, curX = x0, curY = y0;
      while (true) {
        drawPoint(curX, curY, color, size);
        if (curX === x1 && curY === y1) break;
        const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; curX += sx; }
        if (e2 < dx) { err += dx; curY += sy; }
      }
    }

    function flushPendingPoints() {
      if (!ws || ws.readyState !== WebSocket.OPEN || pendingPoints.length === 0) return;

      const activeCol = (currentTool === 'eraser') ? '#000000' : currentColor;
      const rgb = hexToRgb(activeCol);
      const count = Math.min(pendingPoints.length, 64);

      // Packet format: [0x05, r, g, b, brushSize, count, x0, y0, x1, y1, ...]
      const buf = new Uint8Array(6 + count * 2);
      buf[0] = 0x05;
      buf[1] = rgb.r;
      buf[2] = rgb.g;
      buf[3] = rgb.b;
      buf[4] = brushSize;
      buf[5] = count;

      for (let i = 0; i < count; i++) {
        buf[6 + i * 2]     = pendingPoints[i].x;
        buf[6 + i * 2 + 1] = pendingPoints[i].y;
      }

      ws.send(buf.buffer);

      if (isDrawing) {
        // Retain the last sent point as the starting anchor for the next segment
        const lastPt = pendingPoints[count - 1];
        pendingPoints = [lastPt, ...pendingPoints.slice(count)];
      } else {
        pendingPoints = [];
      }
    }

    function startFlushTimer() {
      if (!flushTimer) {
        flushTimer = setInterval(flushPendingPoints, 16); // 60 FPS smooth streaming
      }
    }

    function stopFlushTimer() {
      if (flushTimer) {
        clearInterval(flushTimer);
        flushTimer = null;
      }
    }

    function getCanvasCoords(clientX, clientY) {
      if (!canvasRect) updateCanvasRect();
      const x = Math.floor((clientX - canvasRect.left) * (WIDTH / canvasRect.width));
      const y = Math.floor((clientY - canvasRect.top) * (HEIGHT / canvasRect.height));
      return {
        x: Math.max(0, Math.min(WIDTH - 1, x)),
        y: Math.max(0, Math.min(HEIGHT - 1, y))
      };
    }

    function startDraw(e) {
      if (e.button !== undefined && e.button !== 0) return; // Only main button
      isDrawing = true;
      try {
        canvas.setPointerCapture(e.pointerId);
      } catch (err) {}
      updateCanvasRect();

      const pt = getCanvasCoords(e.clientX, e.clientY);
      lastX = pt.x;
      lastY = pt.y;

      const activeCol = (currentTool === 'eraser') ? '#000000' : currentColor;
      drawPoint(pt.x, pt.y, activeCol, brushSize);

      pendingPoints = [{ x: pt.x, y: pt.y }];
      startFlushTimer();

      cursorPos.textContent = `X: ${pt.x}, Y: ${pt.y}`;
      if (e.cancelable) e.preventDefault();
    }

    function moveDraw(e) {
      if (!isDrawing) {
        const pt = getCanvasCoords(e.clientX, e.clientY);
        cursorPos.textContent = `X: ${pt.x}, Y: ${pt.y}`;
        return;
      }

      // Read all coalesced high-frequency sub-events from fast gestures
      const events = (typeof e.getCoalescedEvents === 'function') ? e.getCoalescedEvents() : [e];
      const activeCol = (currentTool === 'eraser') ? '#000000' : currentColor;

      for (const ev of events) {
        const pt = getCanvasCoords(ev.clientX, ev.clientY);
        if (pt.x === lastX && pt.y === lastY) continue;

        drawLineLocal(lastX, lastY, pt.x, pt.y, activeCol, brushSize);
        pendingPoints.push({ x: pt.x, y: pt.y });

        lastX = pt.x;
        lastY = pt.y;
      }

      cursorPos.textContent = `X: ${lastX}, Y: ${lastY}`;

      if (pendingPoints.length >= 32) {
        flushPendingPoints();
      }

      if (e.cancelable) e.preventDefault();
    }

    function stopDraw(e) {
      if (!isDrawing) return;
      isDrawing = false;
      try {
        if (e && e.pointerId) canvas.releasePointerCapture(e.pointerId);
      } catch (err) {}

      stopFlushTimer();
      flushPendingPoints();
      pendingPoints = [];
      lastX = -1;
      lastY = -1;
      if (e && e.cancelable) e.preventDefault();
    }

    canvas.addEventListener('pointerdown', startDraw);
    canvas.addEventListener('pointermove', moveDraw);
    canvas.addEventListener('pointerup', stopDraw);
    canvas.addEventListener('pointercancel', stopDraw);

    const swatches = document.querySelectorAll('.color-swatch');
    swatches.forEach(btn => {
      btn.addEventListener('click', () => {
        swatches.forEach(s => s.classList.remove('active'));
        btn.classList.add('active');
        currentColor = btn.getAttribute('data-color');
        selectPen();
      });
    });

    document.getElementById('customColorPicker').addEventListener('input', (e) => {
      swatches.forEach(s => s.classList.remove('active'));
      currentColor = e.target.value;
      selectPen();
    });

    const penBtn = document.getElementById('penBtn');
    const eraserBtn = document.getElementById('eraserBtn');
    function selectPen() { currentTool = 'pen'; penBtn.classList.add('active'); eraserBtn.classList.remove('active'); }
    function selectEraser() { currentTool = 'eraser'; eraserBtn.classList.add('active'); penBtn.classList.remove('active'); }
    penBtn.addEventListener('click', selectPen);
    eraserBtn.addEventListener('click', selectEraser);

    const brushBtns = [
      { el: document.getElementById('brush1'), size: 1 },
      { el: document.getElementById('brush2'), size: 2 },
      { el: document.getElementById('brush3'), size: 3 }
    ];
    brushBtns[0].el.classList.add('active');
    brushBtns.forEach(item => {
      item.el.addEventListener('click', () => {
        brushBtns.forEach(b => b.el.classList.remove('active'));
        item.el.classList.add('active');
        brushSize = item.size;
      });
    });

    document.getElementById('clearBtn').addEventListener('click', () => {
      ctx.fillStyle = '#000000'; ctx.fillRect(0, 0, WIDTH, HEIGHT);
      sendClear();
    });
    document.getElementById('fillBtn').addEventListener('click', () => {
      const fillCol = (currentTool === 'eraser') ? '#000000' : currentColor;
      ctx.fillStyle = fillCol; ctx.fillRect(0, 0, WIDTH, HEIGHT);
      sendFill(fillCol);
    });
    document.getElementById('syncBtn').addEventListener('click', sendSyncRequest);

    // ================= CLIENT-SIDE PHOTO CROPPER =================
    const cropModal = document.getElementById('cropModal');
    const cropCanvas = document.getElementById('cropCanvas');
    const cropCtx = cropCanvas.getContext('2d');
    const imageInput = document.getElementById('imageInput');
    const uploadBtn = document.getElementById('uploadBtn');
    const zoomSlider = document.getElementById('zoomSlider');
    const zoomVal = document.getElementById('zoomVal');
    const resetCropBtn = document.getElementById('resetCropBtn');
    const cancelCropBtn = document.getElementById('cancelCropBtn');
    const applyCropBtn = document.getElementById('applyCropBtn');

    let loadedImage = null;
    let cropScale = 1.0;
    let cropBaseScale = 1.0;
    let cropOffsetX = 0;
    let cropOffsetY = 0;
    let isDraggingCrop = false;
    let cropDragStartX = 0;
    let cropDragStartY = 0;

    uploadBtn.addEventListener('click', () => imageInput.click());

    imageInput.addEventListener('change', (e) => {
      const file = e.target.files[0];
      if (!file) return;

      const reader = new FileReader();
      reader.onload = (event) => {
        const img = new Image();
        img.onload = () => {
          loadedImage = img;
          openCropModal();
        };
        img.src = event.target.result;
      };
      reader.readAsDataURL(file);
      imageInput.value = '';
    });

    function openCropModal() {
      cropModal.classList.add('open');
      const scaleX = cropCanvas.width / loadedImage.width;
      const scaleY = cropCanvas.height / loadedImage.height;
      cropBaseScale = Math.max(scaleX, scaleY);
      cropScale = 1.0;
      zoomSlider.value = 1.0;
      zoomVal.textContent = '1.0x';
      cropOffsetX = (cropCanvas.width - loadedImage.width * cropBaseScale) / 2;
      cropOffsetY = (cropCanvas.height - loadedImage.height * cropBaseScale) / 2;
      renderCrop();
    }

    function renderCrop() {
      if (!loadedImage) return;
      cropCtx.fillStyle = '#000000';
      cropCtx.fillRect(0, 0, cropCanvas.width, cropCanvas.height);

      const curScale = cropBaseScale * cropScale;
      const drawW = loadedImage.width * curScale;
      const drawH = loadedImage.height * curScale;

      cropCtx.drawImage(loadedImage, cropOffsetX, cropOffsetY, drawW, drawH);

      cropCtx.strokeStyle = 'rgba(59, 130, 246, 0.8)';
      cropCtx.lineWidth = 2;
      cropCtx.strokeRect(0, 0, cropCanvas.width, cropCanvas.height);
    }

    cropCanvas.addEventListener('mousedown', (e) => {
      isDraggingCrop = true;
      cropDragStartX = e.clientX - cropOffsetX;
      cropDragStartY = e.clientY - cropOffsetY;
    });

    window.addEventListener('mousemove', (e) => {
      if (!isDraggingCrop) return;
      cropOffsetX = e.clientX - cropDragStartX;
      cropOffsetY = e.clientY - cropDragStartY;
      renderCrop();
    });

    window.addEventListener('mouseup', () => { isDraggingCrop = false; });

    cropCanvas.addEventListener('touchstart', (e) => {
      if (e.touches.length === 1) {
        isDraggingCrop = true;
        cropDragStartX = e.touches[0].clientX - cropOffsetX;
        cropDragStartY = e.touches[0].clientY - cropOffsetY;
      }
    }, { passive: false });

    window.addEventListener('touchmove', (e) => {
      if (!isDraggingCrop || e.touches.length !== 1) return;
      cropOffsetX = e.touches[0].clientX - cropDragStartX;
      cropOffsetY = e.touches[0].clientY - cropDragStartY;
      renderCrop();
      if (e.cancelable) e.preventDefault();
    }, { passive: false });

    window.addEventListener('touchend', () => { isDraggingCrop = false; });

    cropCanvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      const zoomFactor = e.deltaY < 0 ? 1.08 : 0.92;
      const newScale = Math.max(0.1, Math.min(4.0, cropScale * zoomFactor));
      if (Math.abs(newScale - cropScale) < 0.001) return;

      const oldScale = cropScale;
      cropScale = newScale;
      zoomSlider.value = cropScale.toFixed(2);
      zoomVal.textContent = cropScale.toFixed(2) + 'x';

      // Zoom centered on cursor position in crop canvas
      const rect = cropCanvas.getBoundingClientRect();
      const mouseX = (e.clientX - rect.left) * (cropCanvas.width / rect.width);
      const mouseY = (e.clientY - rect.top) * (cropCanvas.height / rect.height);

      cropOffsetX = mouseX - (mouseX - cropOffsetX) * (cropScale / oldScale);
      cropOffsetY = mouseY - (mouseY - cropOffsetY) * (cropScale / oldScale);
      renderCrop();
    }, { passive: false });

    zoomSlider.addEventListener('input', (e) => {
      const oldScale = cropScale;
      cropScale = parseFloat(e.target.value);
      zoomVal.textContent = cropScale.toFixed(2) + 'x';

      const centerX = cropCanvas.width / 2;
      const centerY = cropCanvas.height / 2;
      cropOffsetX = centerX - (centerX - cropOffsetX) * (cropScale / oldScale);
      cropOffsetY = centerY - (centerY - cropOffsetY) * (cropScale / oldScale);
      renderCrop();
    });

    resetCropBtn.addEventListener('click', () => {
      cropScale = 1.0;
      zoomSlider.value = 1.0;
      zoomVal.textContent = '1.0x';
      cropOffsetX = (cropCanvas.width - loadedImage.width * cropBaseScale) / 2;
      cropOffsetY = (cropCanvas.height - loadedImage.height * cropBaseScale) / 2;
      renderCrop();
    });

    cancelCropBtn.addEventListener('click', () => {
      cropModal.classList.remove('open');
      loadedImage = null;
    });

    applyCropBtn.addEventListener('click', () => {
      if (!loadedImage) return;
      ctx.drawImage(cropCanvas, 0, 0, WIDTH, HEIGHT);
      cropModal.classList.remove('open');
      loadedImage = null;
      uploadCanvasToMatrix();
    });

    connectWebSocket();
  </script>
</body>
</html>
)rawliteral";

/* ================= FAST DRAWING UTILS ================= */
inline void drawPixelCanvas(int x, int y, uint16_t color, uint8_t brushSize) {
  if (brushSize <= 1) {
    if ((unsigned)x < MATRIX_WIDTH && (unsigned)y < MATRIX_HEIGHT) {
      dma_display->drawPixel(x, y, color);
      canvasBuffer[y * MATRIX_WIDTH + x] = color;
    }
  } else {
    int offset = brushSize / 2;
    int startOffset = -offset;
    int endOffset = (brushSize % 2 == 0) ? (offset - 1) : offset;
    for (int dy = startOffset; dy <= endOffset; dy++) {
      int py = y + dy;
      if ((unsigned)py >= MATRIX_HEIGHT) continue;
      uint16_t* rowPtr = &canvasBuffer[py * MATRIX_WIDTH];
      for (int dx = startOffset; dx <= endOffset; dx++) {
        int px = x + dx;
        if ((unsigned)px < MATRIX_WIDTH) {
          dma_display->drawPixel(px, py, color);
          rowPtr[px] = color;
        }
      }
    }
  }
}

void drawLineCanvas(int x0, int y0, int x1, int y1, uint16_t color, uint8_t brushSize) {
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  while (true) {
    drawPixelCanvas(x0, y0, color, brushSize);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* ================= CHUNKED FRAME SYNC ================= */
void sendFramebufferChunked(uint8_t clientNum) {
  uint8_t chunkBuf[3 + 4 * MATRIX_WIDTH * 2];
  chunkBuf[0] = 0x08; // Command: Image Chunk
  chunkBuf[2] = 4;    // 4 rows per chunk

  for (uint8_t c = 0; c < 16; c++) {
    uint8_t startY = c * 4;
    chunkBuf[1] = startY;
    memcpy(&chunkBuf[3], &canvasBuffer[startY * MATRIX_WIDTH], 4 * MATRIX_WIDTH * sizeof(uint16_t));
    webSocket.sendBIN(clientNum, chunkBuf, sizeof(chunkBuf));
    delay(2);
  }
}

/* ================= WEBSOCKET PROTOCOL ================= */
void handleBinaryCommand(uint8_t num, uint8_t * payload, size_t length) {
  if (length == 0) return;
  uint8_t cmd = payload[0];

  // 1. PIN Authentication: [0x00, ...PIN string]
  if (cmd == 0x00) {
    if (length < 2) return;
    char receivedPin[32] = {0};
    size_t pinLen = min(length - 1, (size_t)31);
    memcpy(receivedPin, payload + 1, pinLen);
    receivedPin[pinLen] = '\0';

    if (strcmp(receivedPin, ACCESS_PIN) == 0) {
      if (num < MAX_WS_CLIENTS) clientAuthenticated[num] = true;
      uint8_t ack[] = { 0x00, 0x01 }; // 0x00 Auth Success
      webSocket.sendBIN(num, ack, sizeof(ack));
      sendFramebufferChunked(num);
      Serial.printf("[%u] Client authenticated successfully!\n", num);
    } else {
      uint8_t nack[] = { 0x00, 0x00 }; // 0x00 Auth Failed
      webSocket.sendBIN(num, nack, sizeof(nack));
      Serial.printf("[%u] Invalid PIN attempted! Disconnecting...\n", num);
      webSocket.disconnect(num);
    }
    return;
  }

  // Reject all other commands if client is not authenticated!
  if (REQUIRE_AUTH && (num >= MAX_WS_CLIENTS || !clientAuthenticated[num])) {
    uint8_t nack[] = { 0x00, 0x00 };
    webSocket.sendBIN(num, nack, sizeof(nack));
    webSocket.disconnect(num);
    return;
  }

  // 2. Anti-Spam / Anti-Video Flood Rate Limiter for Photo Uploads
  if (cmd == 0x08) {
    if (payload[1] == 0) { // start of new image (row 0)
      unsigned long now = millis();
      if (now - lastImageUploadMs < MIN_IMAGE_INTERVAL_MS) {
        Serial.println("Image upload rejected: cooldown active (anti-spam)");
        return;
      }
      lastImageUploadMs = now;
    }
  }

  switch (cmd) {
    case 0x01: { // Single pixel: [0x01, x, y, r, g, b]
      if (length < 6) return;
      uint8_t x = payload[1], y = payload[2];
      if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;
      uint16_t color = dma_display->color565(payload[3], payload[4], payload[5]);
      drawPixelCanvas(x, y, color, 1);
      break;
    }
    case 0x02: { // Line: [0x02, x0, y0, x1, y1, r, g, b, brushSize]
      if (length < 9) return;
      uint8_t x0 = payload[1], y0 = payload[2];
      uint8_t x1 = payload[3], y1 = payload[4];
      if (x0 >= MATRIX_WIDTH || y0 >= MATRIX_HEIGHT || x1 >= MATRIX_WIDTH || y1 >= MATRIX_HEIGHT) return;
      uint16_t color = dma_display->color565(payload[5], payload[6], payload[7]);
      drawLineCanvas(x0, y0, x1, y1, color, payload[8]);
      break;
    }
    case 0x03: { // Clear
      dma_display->clearScreen();
      memset(canvasBuffer, 0, sizeof(canvasBuffer));
      webSocket.broadcastBIN(payload, length);
      break;
    }
    case 0x04: { // Fill: [0x04, r, g, b]
      if (length < 4) return;
      uint16_t color = dma_display->color565(payload[1], payload[2], payload[3]);
      dma_display->fillScreen(color);
      for (int i = 0; i < MATRIX_WIDTH * MATRIX_HEIGHT; i++) {
        canvasBuffer[i] = color;
      }
      webSocket.broadcastBIN(payload, length);
      break;
    }
    case 0x05: { // Batched polyline stroke: [0x05, r, g, b, brushSize, count, x0, y0, x1, y1, ...]
      if (length < 6) return;
      uint8_t r = payload[1], g = payload[2], b = payload[3], brushSize = payload[4];
      uint8_t count = payload[5];
      if (length < 6 + (size_t)count * 2) return;
      if (count == 0) return;

      uint16_t color = dma_display->color565(r, g, b);
      const uint8_t* pts = payload + 6;
      uint8_t prevX = pts[0], prevY = pts[1];

      if (count == 1) {
        drawPixelCanvas(prevX, prevY, color, brushSize);
      } else {
        for (uint8_t i = 1; i < count; i++) {
          uint8_t curX = pts[i * 2];
          uint8_t curY = pts[i * 2 + 1];
          drawLineCanvas(prevX, prevY, curX, curY, color, brushSize);
          prevX = curX;
          prevY = curY;
        }
      }
      break;
    }
    case 0x06: { // Sync request
      sendFramebufferChunked(num);
      break;
    }
    case 0x07: { // Set Brightness (0 - 150): [0x07, brightness]
      if (length < 2) return;
      uint8_t b = payload[1];
      if (b > 150) b = 150;
      dma_display->setBrightness8(b);
      break;
    }
    case 0x08: { // Chunked photo upload: [0x08, startY, numRows, ...pixels]
      if (length < 3) return;
      uint8_t startY  = payload[1];
      uint8_t numRows = payload[2];
      size_t expectedLen = 3 + (size_t)numRows * MATRIX_WIDTH * 2;
      if (length < expectedLen) return;

      const uint8_t* p = payload + 3;
      for (uint8_t r = 0; r < numRows; r++) {
        int y = startY + r;
        if (y >= MATRIX_HEIGHT) break;
        uint16_t* rowPtr = &canvasBuffer[y * MATRIX_WIDTH];
        for (int x = 0; x < MATRIX_WIDTH; x++) {
          uint16_t c565 = p[0] | (p[1] << 8);
          p += 2;
          dma_display->drawPixel(x, y, c565);
          rowPtr[x] = c565;
        }
      }
      break;
    }
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected\n", num);
      if (num < MAX_WS_CLIENTS) clientAuthenticated[num] = false;
      break;
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] Connected: %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      if (num < MAX_WS_CLIENTS) {
        clientAuthenticated[num] = !REQUIRE_AUTH;
      }
      if (!REQUIRE_AUTH) {
        sendFramebufferChunked(num);
      }
      break;
    }
    case WStype_BIN:
      handleBinaryCommand(num, payload, length);
      break;
    default:
      break;
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== HUB75 Web Drawing & Photo Tool ===");

  // 1. Connect WiFi FIRST at Maximum RF Power (Zero DMA Noise during handshake)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // Keep WiFi receiver awake for 1ms WebSocket latency
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Maximize RF transmit power to 19.5 dBm for 10m+ range
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to %s at max TX power", ssid);

  // Allow up to 30s for long-range negotiation through walls
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 60) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  IPAddress ip;
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP();
    Serial.println("\nWiFi connected successfully!");
    Serial.printf("Signal RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\nWiFi failed! Starting Fallback Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_pass);
    ip = WiFi.softAPIP();
    Serial.printf("AP SSID: %s (Password: %s)\n", ap_ssid, ap_pass);
  }

  Serial.printf("Web App URL: http://%s\n", ip.toString().c_str());

  if (MDNS.begin("esp32draw")) {
    Serial.println("mDNS: http://esp32draw.local");
  }

  // 2. NOW Initialize Matrix DMA Engine after WiFi is already established
  HUB75_I2S_CFG mxconfig;
  mxconfig.mx_height = PANEL_HEIGHT;      // 64 px
  mxconfig.chain_length = PANELS_NUMBER;  // 2 chained panels
  mxconfig.gpio.e = PIN_E;                // GPIO 21
  mxconfig.gpio.d = PIN_D;                // GPIO 2

  // Signal & Memory Stability Optimizations:
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  mxconfig.clkphase = MATRIX_CLKPHASE;
  mxconfig.latch_blanking = 1;
  mxconfig.double_buff = false;           // Saves ~40KB DMA RAM for WiFi stability

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(DEFAULT_BRIGHTNESS);
  dma_display->clearScreen();
  memset(canvasBuffer, 0, sizeof(canvasBuffer));

  // Display IP directly on the LED Matrix
  dma_display->setCursor(4, 8);
  dma_display->setTextColor(dma_display->color565(0, 255, 0));
  dma_display->print("READY TO DRAW!");
  dma_display->setCursor(4, 24);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->print("Open browser at:");
  dma_display->setCursor(4, 40);
  dma_display->setTextColor(dma_display->color565(0, 200, 255));
  dma_display->print(ip.toString());

  // 3. Web Server Routes
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.begin();

  // 4. WebSocket Server Setup
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  Serial.println("System initialized successfully.");
}

/* ================= CLEAN LEAN MAIN LOOP ================= */
void loop() {
  webSocket.loop();
  server.handleClient();
  delay(1);
}
