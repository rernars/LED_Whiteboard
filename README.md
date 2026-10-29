# ESP32 Real-Time HUB75 Web Drawing Board & Photo Frame (128×64)

Turn an ESP32 and two daisy-chained 64×64 HUB75 LED panels into a live, interactive drawing canvas and digital photo display. Anyone on your local network (or over the internet) can open a webpage on their phone, tablet, or laptop and draw pixels, sketch in real time, or crop and upload photos directly to the wall display.

> **Hardware Note:** If you built my previous **ESP32 Bus Departure Display** project on Hackster.io, this project uses the **exact same hardware and wiring**. No soldering, no rewiring, and no hardware changes needed—just flash this firmware to give your display a second life.

---

## What It Does

- **Real-Time Live Drawing:** Web canvas syncing at ~60 FPS over WebSockets. Fast mouse and finger strokes stay smooth and connected with zero dropped pixels.
- **Client-Side Photo Cropper:** Upload any photo directly from your device. Includes smooth zoom (0.1x to 4x, with mouse wheel zoom support), drag-to-position framing, and instant client-side resizing down to 128×64 RGB565. Zero image-processing burden on the ESP32.
- **Mobile-Adaptive Interface:** Fluid responsive UI with touch gestures that fits smartphone screens without horizontal scrolling or accidental page bounce.
- **Live Brightness Control:** Smooth on-screen slider (0 to 150) that adjusts panel brightness on the fly.
- **Built-in Security & Anti-Spam:** PIN-protected access so unauthorized users or bots can't overwrite your canvas. Includes a firmware-level upload rate limiter to prevent people from spamming video streams.
- **Self-Hosted:** The ESP32 hosts the entire web application from its internal flash—no external web hosting, cloud subscription, or companion apps required.

---

## Hardware Requirements

| Component | Description | Quantity |
| :--- | :--- | :--- |
| **ESP32 Dev Board** | Standard 30-pin or 38-pin ESP32-WROOM-32 | 1 |
| **64×64 HUB75 LED Panels** | 1/32 scan (P2.5, P3, or P4) | 2 |
| **5V Power Supply** | 5V 5A to 10A power supply (panels draw high current at high brightness) | 1 |
| **16-pin Ribbon Cables** | Standard HUB75 IDC cables (usually come with the panels) | 2 |
| **Jumper Wires** | Female-to-female or custom breakout board | As needed |
| **Electrolytic Capacitor** | 100µF–470µF 10V/16V across ESP32 5V & GND (recommended) | 1 |

---

## Wiring & Pinout

Two 64×64 panels are connected in series (**daisy-chained**):
`ESP32` $\rightarrow$ `Panel 1 DATA IN` $\rightarrow$ `Panel 1 DATA OUT` $\rightarrow$ `Panel 2 DATA IN`

### ESP32 to Panel 1 (DATA IN) Header

| HUB75 Pin | ESP32 GPIO | Description |
| :--- | :--- | :--- |
| **R1** | GPIO 25 | Upper half Red |
| **G1** | GPIO 26 | Upper half Green |
| **B1** | GPIO 27 | Upper half Blue |
| **R2** | GPIO 14 | Lower half Red |
| **G2** | GPIO 12 | Lower half Green |
| **B2** | GPIO 13 | Lower half Blue |
| **A** | GPIO 23 | Row address A |
| **B** | GPIO 19 | Row address B |
| **C** | GPIO 5 | Row address C |
| **D** | **GPIO 2** | Row address D |
| **E** | **GPIO 21** | Row address E (Required for 64-row panels) |
| **CLK** | GPIO 17 | Shift clock |
| **LAT / STB** | GPIO 4 | Data latch |
| **OE** | GPIO 15 | Output enable (active LOW) |
| **GND** | GND | Common ground (connect to power supply GND) |

> ⚠️ **Important Power Tips:**
> 1. Never power the LED panels from the ESP32's 5V/VIN pin. Run 5V and GND directly from your 5V power supply to both panels using the heavy-gauge screw terminal power cables.
> 2. Connect the ESP32 GND to the power supply GND.
> 3. Add a 100µF–470µF capacitor across the ESP32 5V and GND pins. WiFi transmissions cause ~300mA RF spikes; the capacitor prevents brownout restarts.

---

## Software Setup

### 1. Arduino IDE Setup
1. Install the latest [Arduino IDE](https://www.arduino.cc/en/software).
2. Go to **File > Preferences** and add the ESP32 board manager URL:
   ```text
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools > Board > Boards Manager...**, search for `esp32`, and install the **esp32 by Espressif** platform.

### 2. Install Required Libraries
Open **Tools > Manage Libraries...** and install:
- **`ESP32-HUB75-MatrixPanel-I2S-DMA`** by *mrfaptastic*
- **`WebSockets`** by *Markus Sattler* (version 2.3.5 or newer)

### 3. Configure the Code
Open `esp32_hub75_web_draw.ino`:

1. **Enter your Wi-Fi credentials:**
   ```cpp
   const char* ssid     = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```
2. **Set your custom security PIN (default is `1234`):**
   ```cpp
   const char* ACCESS_PIN = "1234"; // Change to your preferred 4-8 digit code
   ```
3. Select your board (**ESP32 Dev Module**) under **Tools > Board**.
4. Set **Partition Scheme** to **Huge APP (3MB No OTA/1MB SPIFFS)** or **Default 4MB with ffat**.
5. Hit **Upload**.

---

## How to Use It

1. When the ESP32 boots up, it connects to your Wi-Fi and displays its assigned IP address directly on the LED screen (e.g. `192.168.1.150`).
2. Open a browser on your phone, tablet, or PC and navigate to:
   ```text
   http://<ESP32_IP>
   ```
   *(or `http://esp32draw.local` if your network supports mDNS).*
3. **Unlock:** A lock prompt will ask for your PIN. Enter `1234` (or whatever PIN you set in the code). The browser saves this token in local storage so you won't need to re-enter it every time.
4. **Draw:** Pick a color, brush size (1px, 2px, 3px), or the eraser tool, and draw across the canvas.
5. **Upload Photos:** Tap **"📷 Choose & Crop Photo"**, select an image, zoom in or out, drag to center your subject, and hit **"✂️ Apply & Send to Matrix"**.
6. **Adjust Brightness:** Drag the brightness slider (0–150) to match your room lighting.

---

## Under the Hood

### Smooth Drawing (Pointer Events & Coalesced Stream)
Traditional browser `touchmove` and `mousemove` events get throttled to the screen's refresh rate (60 Hz) and drop intermediate points during quick flicks. This web UI uses the modern **Pointer Events API** with `e.getCoalescedEvents()`, capturing high-frequency micro-movements. It packs them into a lightweight binary polyline packet (`0x05`) every 16ms, which the ESP32 renders using Bresenham's line algorithm. Fast strokes stay completely solid without disconnected gaps.

### Zero-Overhead Client-Side Cropping
The ESP32 does not have enough free RAM to decode JPEG/PNG images or resize them. The web UI handles 100% of the image decoding and cropping inside an HTML5 Canvas on the client device. It downsamples the selected frame directly to 128×64 pixels, packs them into 16-bit RGB565 color bytes, and streams them in 4-row chunks over the WebSocket.

### RF Noise & DMA Isolation
HUB75 ribbon cables carry high-speed 10 MHz square waves that emit noticeable 2.4 GHz radio-frequency noise. In this firmware, the Wi-Fi connection and WPA2 handshake are completed **before** the matrix DMA engine is started, with Wi-Fi power locked at maximum transmission power (`WIFI_POWER_19_5dBm`). This ensures rock-solid connections through walls even 10+ meters away from the router.

---

## Remote Access (Optional: Cloudflare Tunnel / Nginx)

If you want your friends or roommates to draw on the matrix from outside your home network, you can expose it securely via **Cloudflare Tunnel** or an **Nginx reverse proxy** (e.g. on CasaOS or a home server).

The web interface automatically detects HTTPS and domain names, switching from standard `ws://<ip>:81` to secure `wss://<domain>/ws` over port 443.

### Cloudflare Tunnel Setup (No Port Forwarding Required)
In the Cloudflare Zero Trust Dashboard under **Tunnels > Public Hostnames**:
1. **Hostname:** `led.yourdomain.com` | **Path:** `/ws` | **Service:** `HTTP://<ESP32_IP>:81` *(Enable WebSockets in HTTP Settings)*
2. **Hostname:** `led.yourdomain.com` | **Path:** `(empty)` | **Service:** `HTTP://<ESP32_IP>:80`

### Nginx Reverse Proxy Config
```nginx
server {
    listen 80;
    server_name led.yourdomain.com;

    location / {
        proxy_pass http://<ESP32_IP>:80;
        proxy_set_header Host $host;
    }

    location /ws {
        proxy_pass http://<ESP32_IP>:81/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "Upgrade";
        proxy_read_timeout 86400s;
    }
}
```

---

## License

MIT License. Free to use, modify, and build upon!
