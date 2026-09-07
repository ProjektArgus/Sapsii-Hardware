#include "esp_camera.h"
#include "esp_http_server.h"
#include <ESPmDNS.h>
#include <WiFi.h>

// AI-Thinker ESP32-CAM only.
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Leave WIFI_SSID empty to create the ESP32-CAM-YOLO access point.
// If station connection times out, the camera also falls back to this AP.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char AP_SSID[] = "ESP32-CAM-YOLO";
constexpr char AP_PASSWORD[] = "12345678"; // WPA2 requires at least 8 characters.
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;

static const char STREAM_CONTENT_TYPE[] =
    "multipart/x-mixed-replace;boundary=frame-boundary-7MA4YWxkTrZu0gW";
static const char STREAM_BOUNDARY[] =
    "\r\n--frame-boundary-7MA4YWxkTrZu0gW\r\n";
static const char STREAM_PART[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t cameraHttpd = nullptr;

void setCommonHeaders(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(request, "Pragma", "no-cache");
}

esp_err_t statusHandler(httpd_req_t *request) {
  setCommonHeaders(request);
  httpd_resp_set_type(request, "application/json");

  char response[160];
  const IPAddress address = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(response, sizeof(response),
           "{\"status\":\"ok\",\"ip\":\"%s\",\"stream\":\"/stream\",\"capture\":\"/capture\"}",
           address.toString().c_str());
  return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

esp_err_t captureHandler(httpd_req_t *request) {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    return ESP_FAIL;
  }

  setCommonHeaders(request);
  httpd_resp_set_type(request, "image/jpeg");
  httpd_resp_set_hdr(request, "Content-Disposition", "inline; filename=capture.jpg");
  const esp_err_t result =
      httpd_resp_send(request, reinterpret_cast<const char *>(frame->buf), frame->len);
  esp_camera_fb_return(frame);
  return result;
}

esp_err_t streamHandler(httpd_req_t *request) {
  esp_err_t result = httpd_resp_set_type(request, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) {
    return result;
  }
  setCommonHeaders(request);

  char header[96];
  while (true) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr) {
      Serial.println("Camera capture failed");
      return ESP_FAIL;
    }

    result = httpd_resp_send_chunk(request, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK) {
      const int headerLength =
          snprintf(header, sizeof(header), STREAM_PART, static_cast<unsigned int>(frame->len));
      if (headerLength <= 0 || headerLength >= static_cast<int>(sizeof(header))) {
        result = ESP_FAIL;
      } else {
        result = httpd_resp_send_chunk(request, header, headerLength);
      }
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(frame->buf), frame->len);
    }

    esp_camera_fb_return(frame);
    if (result != ESP_OK) {
      break; // Normal when the phone closes or replaces the stream.
    }
  }
  return result;
}

bool startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;

  if (httpd_start(&cameraHttpd, &config) != ESP_OK) {
    return false;
  }

  httpd_uri_t statusUri = {};
  statusUri.uri = "/";
  statusUri.method = HTTP_GET;
  statusUri.handler = statusHandler;

  httpd_uri_t captureUri = {};
  captureUri.uri = "/capture";
  captureUri.method = HTTP_GET;
  captureUri.handler = captureHandler;

  httpd_uri_t streamUri = {};
  streamUri.uri = "/stream";
  streamUri.method = HTTP_GET;
  streamUri.handler = streamHandler;

  if (httpd_register_uri_handler(cameraHttpd, &statusUri) != ESP_OK ||
      httpd_register_uri_handler(cameraHttpd, &captureUri) != ESP_OK ||
      httpd_register_uri_handler(cameraHttpd, &streamUri) != ESP_OK) {
    httpd_stop(cameraHttpd);
    cameraHttpd = nullptr;
    return false;
  }
  return true;
}

void startAccessPoint() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("Failed to start access point");
    return;
  }
  Serial.printf("Access point: %s\n", AP_SSID);
  Serial.printf("Stream URL: http://%s/stream\n", WiFi.softAPIP().toString().c_str());
}

void connectNetwork() {
  WiFi.setSleep(false);

  if (WIFI_SSID[0] == '\0') {
    startAccessPoint();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s", WIFI_SSID);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected. Stream URL: http://%s/stream\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Wi-Fi connection timed out; starting access point instead.");
    startAccessPoint();
  }
}

void startDiscoveryService() {
  if (!MDNS.begin("sapseed-cam")) {
    Serial.println("mDNS discovery failed; connect by IP instead");
    return;
  }
  MDNS.addService("sapseedcam", "tcp", 80);
  MDNS.addServiceTxt("sapseedcam", "tcp", "path", "/stream");
  MDNS.addServiceTxt("sapseedcam", "tcp", "model", "AI-Thinker ESP32-CAM");
  Serial.println("Discovery name: sapseed-cam.local");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA; // 320x240; resize/crop to the model input on phone.
  config.jpeg_quality = psramFound() ? 12 : 14;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t cameraResult = esp_camera_init(&config);
  if (cameraResult != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", cameraResult);
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
  }

  connectNetwork();
  startDiscoveryService();
  if (!startCameraServer()) {
    Serial.println("HTTP server failed to start");
    return;
  }
  Serial.println("Camera server ready");
}

void loop() {
  delay(10000); // The HTTP server runs in its own FreeRTOS task.
}
