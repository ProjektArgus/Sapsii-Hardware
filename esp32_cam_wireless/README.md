# ESP32-CAM wireless camera

This sketch turns an AI-Thinker ESP32-CAM into an MJPEG camera suitable for a phone running YOLO11n with LiteRT/TFLite GPU acceleration. Inference stays on the phone; the ESP32 only captures and sends JPEG frames.

## Endpoints

| URL | Purpose |
| --- | --- |
| `http://<camera-ip>/` | JSON status and endpoint discovery |
| `http://<camera-ip>/stream` | Continuous `multipart/x-mixed-replace` MJPEG stream |
| `http://<camera-ip>/capture` | One JPEG frame, useful for testing |

The firmware also advertises `_sapseedcam._tcp` over mDNS so the Sapseed app can discover it automatically. Its hostname is `sapseed-cam.local` when the network supports mDNS.

## Hardware

- AI-Thinker ESP32-CAM with OV2640 camera
- Stable 5 V supply (weak 3.3 V/USB-serial power commonly causes brownouts)
- USB-to-TTL adapter capable of 3.3 V logic

### Flash wiring

| USB-to-TTL | ESP32-CAM |
| --- | --- |
| 5 V | 5V |
| GND | GND |
| TX | U0R / GPIO3 |
| RX | U0T / GPIO1 |
| GND | GPIO0 (flash mode only) |

Do not feed 5 V logic into the serial RX pin. Disconnect GPIO0 from GND and reset/power-cycle after uploading.

## Arduino IDE setup

1. Install Arduino IDE 2.x.
2. Add Espressif's board URL in **Preferences > Additional Boards Manager URLs**:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Install **esp32 by Espressif Systems** in Boards Manager.
4. Open `esp32_cam_wireless.ino` (the sketch and folder names intentionally match).
5. Select **AI Thinker ESP32-CAM**.
6. Enable **PSRAM** if that option is shown, use **Huge APP (3MB No OTA/1MB SPIFFS)**, and start with a 115200 upload speed.
7. Wire GPIO0 to GND, upload, disconnect GPIO0, reset, then open Serial Monitor at 115200 baud.

## Choose the network mode

Edit these constants near the top of the sketch.

### Direct phone-to-camera access point (default)

Leave the station credentials empty:

```cpp
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
```

On the phone, join **ESP32-CAM-YOLO** with password **12345678**. Android/iOS may warn that the network has no internet; choose to stay connected. The stream URL is:

```text
http://192.168.4.1/stream
```

This is portable and usually has the least network variability, but the phone may lose normal Wi-Fi internet while connected.

### Existing Wi-Fi network

Set the credentials:

```cpp
constexpr char WIFI_SSID[] = "your-2.4GHz-network";
constexpr char WIFI_PASSWORD[] = "your-password";
```

The ESP32 supports 2.4 GHz Wi-Fi, not a 5 GHz-only SSID. The phone must be on the same LAN. Read the assigned stream URL from Serial Monitor. Give the ESP32 a DHCP reservation in the router so the app can store a stable IP. If connection fails for 20 seconds, firmware falls back to access-point mode.

## Test before integrating

1. Open `http://<camera-ip>/` in the phone browser; it should return JSON.
2. Open `http://<camera-ip>/capture`; it should show one image.
3. Open `http://<camera-ip>/stream`; it should show moving video.

If capture fails or the board resets, use a better 5 V supply and shorter wires. If latency grows, ensure the app discards old frames rather than queues them.

If Serial Monitor reports `Camera probe failed with error 0x106`, confirm that **AI Thinker ESP32-CAM** is selected and that the ribbon cable is fully seated with its contacts facing the correct direction. This sketch explicitly power-cycles the OV2640 before probing (important after `SW_CPU_RESET`) and uses the same camera initialization settings as Espressif's CameraWebServer example.

## Use with the Sapseed Android app

1. Flash this sketch and connect the phone using either AP mode or the same 2.4 GHz LAN.
2. Open Sapseed. **Camera: Mobile** is the default source.
3. Tap **Camera: Mobile** at the top of the app.
4. Choose **Discover wireless camera**. The firmware advertises itself over mDNS and the app connects to the first Sapseed camera it finds.
5. If discovery is unavailable on the router, choose **Enter camera IP or URL** and enter either `192.168.4.1`, the LAN IP printed in Serial Monitor, or a complete URL such as `http://192.168.4.1/stream`.
6. Wait for **Wireless camera connected**, confirm that its preview is moving, and tap **LiteRT GPU ★**.
7. Tap the camera selector again and choose **Camera: Mobile** to switch back.

On the ESP32's direct access point, the manual address is always `192.168.4.1`. Android may report that this Wi-Fi network has no internet; choose to remain connected.

The app keeps only the newest MJPEG frame, decodes it into the same A/R/G/B representation used by CameraX, and feeds it through the existing YOLO11n LiteRT GPU path. It reconnects automatically after a temporary stream interruption.

### App data path

```text
HTTP MJPEG stream
  -> multipart boundary parser
  -> latest complete JPEG byte array
  -> JPEG decode to RGB/ARGB image
  -> YOLO11n letterbox/resize + normalization
  -> LiteRT GPU inference
  -> YOLO output decode + NMS
  -> draw detections using the inverse letterbox transform
```

Important implementation rules:

- Keep only the **latest** decoded frame. Use a one-element conflated channel/atomic slot; never build a FIFO backlog.
- Run networking/JPEG decoding off the UI thread.
- Allow only one inference at a time. If inference is busy, replace or drop the pending frame.
- Reuse image, tensor, and output buffers where the platform API permits.
- Close the HTTP response and inference resources when the screen stops.
- Add reconnect with bounded backoff when Wi-Fi or the ESP32 stream drops.
- Treat the camera URL as local-network user configuration rather than hard-coding it.
- The stream is plain HTTP and unauthenticated. Use it only on a trusted LAN or the password-protected direct AP; do not port-forward it to the internet.

### Android-specific requirements

Add network permissions:

```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
<uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
```

Because the ESP32 serves HTTP rather than HTTPS, allow cleartext traffic. Prefer a narrow network security configuration for the camera host; for initial development, the application flag is simpler:

```xml
<application
    android:usesCleartextTraffic="true"
    ... />
```

No Android `CAMERA` permission is required merely to consume this network stream. If the app also uses the phone camera, retain its existing permission.

Use an HTTP client that exposes the response body as a streaming byte source. Do not use an API that buffers the complete response—the MJPEG response never ends. Parse the boundary from the `Content-Type` header and each part's `Content-Length`, rather than searching JPEG marker bytes alone.

### iOS-specific requirements

Add a local-network usage description to `Info.plist`:

```xml
<key>NSLocalNetworkUsageDescription</key>
<string>Connect to the ESP32 wireless camera for on-device object detection.</string>
```

App Transport Security blocks arbitrary HTTP by default. Add the narrowest ATS exception compatible with the selected IP/hostname for development, and review it before distribution. A raw IP address cannot use the same domain exception behavior as a DNS hostname, so direct-AP deployments often require an appropriate local-network/arbitrary-load exception.

## YOLO11n + LiteRT GPU notes

- Use the exact input size and tensor layout exported with the `.tflite` model; do not assume it from the camera's 320x240 resolution.
- QVGA is 4:3, while common YOLO exports use a square input. **Letterbox** the frame and retain scale/padding values so detection boxes map back correctly.
- Follow the model metadata/export settings for pixel order and normalization (for example RGB float `[0,1]`, or quantized scale/zero-point). Do not guess.
- Confirm whether the exported YOLO11 model includes NMS. If not, decode predictions and run NMS in app code.
- Create the LiteRT interpreter and GPU delegate once, warm them up once, and reuse them for every frame.
- Keep CPU fallback available because LiteRT GPU operator support varies by model/export and phone.
- Start at QVGA with JPEG quality 12. If inference is slower than incoming video, frame dropping is correct. Increase resolution only after measuring end-to-end latency.

The firmware's `CAMERA_GRAB_LATEST` plus two PSRAM frame buffers reduces stale-camera frames, but the phone must also use a latest-frame policy to prevent latency accumulation.

## What was corrected from the original sketch

- The placeholder SSID no longer accidentally selects station mode; empty credentials now intentionally select AP mode.
- Station connection has a timeout and AP fallback instead of blocking forever.
- `camera_config_t` is zero-initialized, including predictable non-PSRAM grab behavior.
- HTTP responses disable caching and include CORS for clients that need it.
- `/capture` and `/` health/status endpoints were added for troubleshooting.
- HTTP startup and URI registration failures are checked.
- `_sapseedcam._tcp` mDNS advertising was added for in-app discovery.
- Stale sockets can be purged, and all frame buffers are returned on stream exit paths.
