#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include <PubSubClient.h>
#include "esp_camera.h"
#include <base64.h>
#include "time.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WiFiManager.h>
#include <TelnetStream.h>

// ============================================================================
// Console: Serial + Telnet
// ============================================================================
class TeePrint : public Print {
  Print &out1;
  Print &out2;

public:
  TeePrint(Print &_out1, Print &_out2) : out1(_out1), out2(_out2) {}

  virtual size_t write(uint8_t b) override {
    size_t n1 = out1.write(b);

    if (b == '\n') {
      out2.write('\r');
      out2.write('\n');
    } else if (b != '\r') {
      out2.write(b);
    }

    return n1;
  }
};

TeePrint Console(Serial, TelnetStream);

// ============================================================================
// Cámara AI Thinker
// ============================================================================
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ── Red / servidor ──────────────────────────────────────────────────
WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ── Estado ──────────────────────────────────────────────────────────
bool sd_ok = false;
bool timeSynced = false;
bool mqttAttrsSubscribed = false;
volatile int waitingForBatchId = -1;
SemaphoreHandle_t camMutex;
SemaphoreHandle_t stateMutex;
uint8_t *prev_frame = NULL;

// ── WiFiManager ─────────────────────────────────────────────────────
WiFiManager wm;

// ── Handle tarea upload ─────────────────────────────────────────────
TaskHandle_t uploadTaskHandle = NULL;

// ── Estado captura/envío HR ─────────────────────────────────────────
volatile bool highResBusy = false;
volatile bool highResPending = false;
unsigned long highResCooldownUntil = 0;
const unsigned long HIGHRES_COOLDOWN_MS = 10000;
bool cooldownEndReported = true;

// ── Buffer único HR (sin cola) ──────────────────────────────────────
uint8_t* pendingImageBuf = NULL;
size_t pendingImageLen = 0;

// ── Flash manual ────────────────────────────────────────────────────
bool flashManualOn = false;

// ── Modo cámara actual ──────────────────────────────────────────────
enum CameraMode {
  CAM_MODE_STREAM_GRAY,
  CAM_MODE_CAPTURE_COLOR
};

CameraMode currentCamMode = CAM_MODE_STREAM_GRAY;

// ── IP fija global ──────────────────────────────────────────────────
IPAddress fixed_local_IP(192, 168, 1, 220);
IPAddress fixed_gateway(192, 168, 1, 1);
IPAddress fixed_subnet(255, 255, 255, 0);
IPAddress fixed_primaryDNS(8, 8, 8, 8);
IPAddress fixed_secondaryDNS(8, 8, 4, 4);

// ── MQTT dinámico según SSID ────────────────────────────────────────
const char* mqtt_server_office = "192.168.1.250";
const int   mqtt_port_office   = 1883;

const char* mqtt_server_remote = "biosfera.fortidyndns.com";
const int   mqtt_port_remote   = 11883;

const char* mqtt_user          = "6XpCbb2M216hBBLWYUC4";
const char* mqtt_password      = "";
const char* mqtt_topic_tele    = "v1/devices/me/telemetry";
const char* mqtt_topic_attrs   = "v1/devices/me/attributes";

// ── NTP ─────────────────────────────────────────────────────────────
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

// ── Stream / detección ──────────────────────────────────────────────
const int CAM_WIDTH  = 320;
const int CAM_HEIGHT = 240;
const uint32_t STREAM_INTERVAL_MS = 250;
const int STREAM_JPEG_QUALITY = 10;

// ── Detección ───────────────────────────────────────────────────────
const long MOTION_THRESHOLD = 100000;
const uint8_t PIXEL_DIFF_THRESHOLD = 5;

// ── Captura alarma ──────────────────────────────────────────────────
const framesize_t ALARM_FRAME_SIZE = FRAMESIZE_XGA;
const int ALARM_JPEG_QUALITY = 10;

// ── Envío por lotes ─────────────────────────────────────────────────
#define MAX_CHUNK_SIZE         240
#define BATCH_SIZE_CHUNKS      10
#define BATCH_BUFFER_SIZE      (MAX_CHUNK_SIZE * BATCH_SIZE_CHUNKS)
#define MQTT_ACK_TIMEOUT_MS    15000
#define MQTT_MAX_BATCH_RETRIES 3
#define MQTT_SETTLE_MS         500

// ── HTTP MJPEG ──────────────────────────────────────────────────────
const char HEADER[] =
  "HTTP/1.1 200 OK\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n";

const char BOUNDARY[] = "\r\n--123456789000000000000987654321\r\n";
const char CTNTTYPE[] = "Content-Type: image/jpeg\r\nContent-Length: ";

// ────────────────────────────────────────────────────────────────────
// Flash LED
// ────────────────────────────────────────────────────────────────────
void forceFlashOff() {
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  flashManualOn = false;
}

void setFlashManual(bool on) {
  pinMode(4, OUTPUT);
  digitalWrite(4, on ? HIGH : LOW);
  flashManualOn = on;
  Console.printf("Flash manual: %s\n", flashManualOn ? "ON" : "OFF");
}

void toggleFlashManual() {
  setFlashManual(!flashManualOn);
}

// ────────────────────────────────────────────────────────────────────
// Helpers estado HR
// ────────────────────────────────────────────────────────────────────
bool tryLockHighResSlot() {
  bool locked = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    unsigned long now = millis();
    if (!highResBusy && !highResPending && pendingImageBuf == NULL && now >= highResCooldownUntil) {
      highResBusy = true;
      locked = true;
    }
    xSemaphoreGive(stateMutex);
  }
  return locked;
}

bool setPendingHighResImage(uint8_t* buf, size_t len) {
  bool ok = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    if (highResBusy && !highResPending && pendingImageBuf == NULL && buf != NULL && len > 0) {
      pendingImageBuf = buf;
      pendingImageLen = len;
      highResPending = true;
      ok = true;
    }
    xSemaphoreGive(stateMutex);
  }
  return ok;
}

void clearHighResState(bool uploadSuccess) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    highResBusy = false;
    highResPending = false;
    pendingImageBuf = NULL;
    pendingImageLen = 0;

    if (uploadSuccess) {
      highResCooldownUntil = millis() + HIGHRES_COOLDOWN_MS;
      cooldownEndReported = false;
      Console.println("-> Cooldown HR activado: 10 s");
    }

    xSemaphoreGive(stateMutex);
  }
}

bool takePendingHighResImage(uint8_t** buf, size_t* len) {
  bool hasImage = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    if (highResBusy && highResPending && pendingImageBuf != NULL && pendingImageLen > 0) {
      *buf = pendingImageBuf;
      *len = pendingImageLen;
      hasImage = true;
    }
    xSemaphoreGive(stateMutex);
  }
  return hasImage;
}

void checkCooldownFinished() {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    if (!cooldownEndReported &&
        highResCooldownUntil != 0 &&
        millis() >= highResCooldownUntil) {
      Console.println("-> Cooldown HR finalizado. Sistema listo para nueva captura.");
      cooldownEndReported = true;
      highResCooldownUntil = 0;
    }
    xSemaphoreGive(stateMutex);
  }
}

// ────────────────────────────────────────────────────────────────────
// Configuraciones de cámara
// ────────────────────────────────────────────────────────────────────
camera_config_t buildCameraConfigGrayStream() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  return config;
}

camera_config_t buildCameraConfigColorCapture() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = psramFound() ? ALARM_FRAME_SIZE : FRAMESIZE_SVGA;
  config.jpeg_quality = ALARM_JPEG_QUALITY;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  return config;
}

void applyGraySensorTuning() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_vflip(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 3);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 0);
  s->set_ae_level(s, 0);
  s->set_gain_ctrl(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
}

void applyColorSensorTuning() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);

  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 3);

  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, 1);

  s->set_gain_ctrl(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
}

bool initCamera(CameraMode mode) {
  camera_config_t cfg = (mode == CAM_MODE_STREAM_GRAY)
    ? buildCameraConfigGrayStream()
    : buildCameraConfigColorCapture();

  esp_camera_deinit();
  delay(150);

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Console.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  currentCamMode = mode;

  if (mode == CAM_MODE_STREAM_GRAY) {
    applyGraySensorTuning();
  } else {
    applyColorSensorTuning();
  }

  delay(100);
  return true;
}

// ────────────────────────────────────────────────────────────────────
// WiFi + WiFiManager + IP fija global
// ────────────────────────────────────────────────────────────────────
void applyGlobalStaticIP() {
  if (!WiFi.config(fixed_local_IP, fixed_gateway, fixed_subnet, fixed_primaryDNS, fixed_secondaryDNS)) {
    Console.println("No se pudo configurar la IP fija");
  } else {
    Console.printf("IP fija configurada: %s\n", fixed_local_IP.toString().c_str());
  }
}

bool connectWiFiWithManager(bool forcePortal = false) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);

  wm.setHostname("esp32cam");
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  wm.setSTAStaticIPConfig(fixed_local_IP, fixed_gateway, fixed_subnet, fixed_primaryDNS);

  applyGlobalStaticIP();

  bool ok = false;

  if (forcePortal) {
    Console.println("Abriendo portal WiFiManager...");
    ok = wm.startConfigPortal("ESP32CAM-Setup");
    Console.print("IP portal AP: ");
    Console.println(WiFi.softAPIP());
  } else {
    ok = wm.autoConnect("ESP32CAM-Setup");
  }

  if (!ok) {
    Console.println("WiFiManager no pudo conectar/configurar WiFi");
    return false;
  }

  Console.printf("WiFi conectada: %s\n", WiFi.SSID().c_str());
  Console.printf("IP local: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Console.println("WiFi desconectada. Intentando reconectar...");
  applyGlobalStaticIP();
  WiFi.reconnect();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Console.print(".");
  }
  Console.println();

  if (WiFi.status() == WL_CONNECTED) {
    Console.printf("WiFi reconectada: %s  IP: %s\n",
      WiFi.SSID().c_str(),
      WiFi.localIP().toString().c_str());
    return;
  }

  Console.println("Reconexión fallida. Lanzando WiFiManager...");
  connectWiFiWithManager(false);
}

void configureMqttByCurrentSSID() {
  String currentSSID = WiFi.SSID();
  if (currentSSID == "Wifioficina") {
    mqttClient.setServer(mqtt_server_office, mqtt_port_office);
    Console.println("MQTT modo oficina");
  } else {
    mqttClient.setServer(mqtt_server_remote, mqtt_port_remote);
    Console.println("MQTT modo remoto");
  }
}

// ────────────────────────────────────────────────────────────────────
// NTP
// ────────────────────────────────────────────────────────────────────
void syncTimeFromNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Console.println("No se pudo obtener la hora NTP");
    timeSynced = false;
  } else {
    Console.println("Hora sincronizada por NTP");
    Console.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
    timeSynced = true;
  }
}

String getTimestampFileName() {
  struct tm timeinfo;

  if (timeSynced && getLocalTime(&timeinfo, 1000)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &timeinfo);
    return String("/") + String(buf) + ".jpg";
  }

  static uint32_t fallbackCounter = 0;
  fallbackCounter++;
  return "/no_time_" + String(fallbackCounter) + ".jpg";
}

// ────────────────────────────────────────────────────────────────────
// SD
// ────────────────────────────────────────────────────────────────────
bool initSDCard() {
  pinMode(2,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
  pinMode(15, INPUT_PULLUP);

  delay(50);

  if (!SD_MMC.begin("/sdcard", true)) {
    Console.println("SD_MMC mount failed");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Console.println("No SD card attached");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  Console.printf("SD OK: %llu MB\n", cardSize);
  forceFlashOff();
  return true;
}

bool saveBufferToSDRoot(uint8_t* data, size_t len) {
  if (!sd_ok) return false;

  bool restoreFlash = flashManualOn;
  if (restoreFlash) {
    Console.println("Apagando flash antes de escribir en SD (GPIO4 compartido).");
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    delay(5);
  }

  String path = getTimestampFileName();

  if (SD_MMC.exists(path)) {
    SD_MMC.remove(path);
  }

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Console.println("Error al abrir archivo en SD: " + path);
    if (restoreFlash) {
      pinMode(4, OUTPUT);
      digitalWrite(4, HIGH);
    }
    return false;
  }

  size_t written = file.write(data, len);
  file.close();

  if (restoreFlash) {
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH);
  }

  if (written != len) {
    Console.println("Escritura incompleta en SD: " + path);
    return false;
  }

  Console.println("SD: " + path);
  return true;
}

// ────────────────────────────────────────────────────────────────────
// MQTT callback
// ────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  if (!topicStr.startsWith(mqtt_topic_attrs)) return;

  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  int keyIndex = msg.indexOf("ack_batch_id");
  if (keyIndex >= 0) {
    int colonIndex = msg.indexOf(":", keyIndex);
    int commaIndex = msg.indexOf(",", colonIndex);
    int braceIndex = msg.indexOf("}", colonIndex);
    int endIndex = (commaIndex == -1) ? braceIndex : (braceIndex == -1 ? commaIndex : min(commaIndex, braceIndex));

    if (colonIndex != -1 && endIndex != -1 && endIndex > colonIndex) {
      int confirmedBatch = msg.substring(colonIndex + 1, endIndex).toInt();
      if (confirmedBatch == waitingForBatchId) {
        Console.printf("ACK batch %d OK\n", confirmedBatch);
        waitingForBatchId = -1;
      }
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// MQTT envío en batches
// ────────────────────────────────────────────────────────────────────
bool publishBatchOnce(const uint8_t* data, size_t len, int batchId, int totalBatches) {
  if (!mqttClient.connected() || !mqttAttrsSubscribed) return false;

  String b64 = base64::encode((uint8_t*)data, len);
  String header = "{\"img_idx\":" + String(batchId) +
                  ",\"img_total\":" + String(totalBatches) +
                  ",\"img_b64\":\"";
  String footer = "\"}";
  size_t totalPayloadLen = header.length() + b64.length() + footer.length();

  Console.printf("Batch %d/%d raw=%u b64=%u\n",
                batchId, totalBatches - 1, (unsigned)len, (unsigned)b64.length());

  if (!mqttClient.beginPublish(mqtt_topic_tele, totalPayloadLen, false)) {
    Console.println("beginPublish failed");
    return false;
  }

  mqttClient.print(header);

  int b64Len = b64.length();
  for (int i = 0; i < b64Len; i += 128) {
    if (!mqttClient.connected()) {
      Console.println("MQTT lost during publish");
      return false;
    }

    int end = min(i + 128, b64Len);
    mqttClient.print(b64.substring(i, end));
    mqttClient.loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  mqttClient.print(footer);

  if (!mqttClient.endPublish()) {
    Console.println("endPublish failed");
    return false;
  }

  waitingForBatchId = batchId;
  unsigned long waitStart = millis();

  while (millis() - waitStart < MQTT_ACK_TIMEOUT_MS) {
    if (!mqttClient.connected()) {
      Console.println("MQTT lost while waiting ACK");
      waitingForBatchId = -1;
      mqttAttrsSubscribed = false;
      return false;
    }

    mqttClient.loop();

    if (waitingForBatchId == -1) {
      return true;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }

  Console.printf("Timeout ACK batch %d\n", batchId);
  waitingForBatchId = -1;
  return false;
}

bool publishBatchRetry(const uint8_t* data, size_t len, int batchId, int totalBatches) {
  for (int attempt = 1; attempt <= MQTT_MAX_BATCH_RETRIES; attempt++) {
    Console.printf("Batch %d intento %d/%d\n", batchId, attempt, MQTT_MAX_BATCH_RETRIES);

    if (publishBatchOnce(data, len, batchId, totalBatches)) {
      return true;
    }

    mqttClient.disconnect();
    mqttAttrsSubscribed = false;
    vTaskDelay(pdMS_TO_TICKS(300));

    configureMqttByCurrentSSID();
    Console.print("Conectando MQTT...");
    if (mqttClient.connect("ESP32CAM", mqtt_user, mqtt_password)) {
      Console.println(" OK");
      mqttAttrsSubscribed = mqttClient.subscribe(mqtt_topic_attrs);
      vTaskDelay(pdMS_TO_TICKS(MQTT_SETTLE_MS));
    } else {
      Console.print(" fallo, rc=");
      Console.println(mqttClient.state());
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  return false;
}

bool sendImageInBatches(uint8_t* buf, size_t len) {
  size_t offset = 0;
  int batchId = 0;
  int totalChunks  = (len + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
  int totalBatches = (totalChunks + BATCH_SIZE_CHUNKS - 1) / BATCH_SIZE_CHUNKS;

  Console.printf("Enviando imagen: %u bytes, %d chunks, %d batches\n",
                (unsigned)len, totalChunks, totalBatches);

  while (offset < len) {
    size_t batchLen = min((size_t)BATCH_BUFFER_SIZE, len - offset);

    if (!publishBatchRetry(buf + offset, batchLen, batchId, totalBatches)) {
      Console.printf("Fallo definitivo en batch %d\n", batchId);
      return false;
    }

    offset += batchLen;
    batchId++;

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  return true;
}

// ────────────────────────────────────────────────────────────────────
// MQTT reconnect
// ────────────────────────────────────────────────────────────────────
void reconnectMQTT() {
  if (mqttClient.connected()) return;

  configureMqttByCurrentSSID();

  Console.print("Conectando MQTT...");
  if (mqttClient.connect("ESP32CAM", mqtt_user, mqtt_password)) {
    Console.println(" OK");
    mqttAttrsSubscribed = mqttClient.subscribe(mqtt_topic_attrs);
    delay(MQTT_SETTLE_MS);
  } else {
    Console.print(" fallo, rc=");
    Console.println(mqttClient.state());
  }
}

// ────────────────────────────────────────────────────────────────────
// Stream MJPEG
// ────────────────────────────────────────────────────────────────────
bool captureGrayFrameToJpeg(uint8_t **jpg_buf, size_t *jpg_len) {
  *jpg_buf = NULL;
  *jpg_len = 0;

  if (currentCamMode != CAM_MODE_STREAM_GRAY) return false;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;

  bool ok = fmt2jpg(
    fb->buf,
    fb->len,
    fb->width,
    fb->height,
    fb->format,
    STREAM_JPEG_QUALITY,
    jpg_buf,
    jpg_len
  );

  esp_camera_fb_return(fb);
  return ok;
}

void streamTask(void *pvParameters) {
  WiFiClient client = *((WiFiClient *)pvParameters);
  free(pvParameters);

  char buf[32];
  client.write(HEADER, strlen(HEADER));
  client.write(BOUNDARY, strlen(BOUNDARY));

  const TickType_t xPeriod = pdMS_TO_TICKS(STREAM_INTERVAL_MS);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  while (client.connected()) {
    if (xSemaphoreTake(camMutex, portMAX_DELAY)) {
      uint8_t *out_jpg = NULL;
      size_t out_jpg_len = 0;

      bool converted = captureGrayFrameToJpeg(&out_jpg, &out_jpg_len);

      xSemaphoreGive(camMutex);

      if (converted && client.connected()) {
        client.write(CTNTTYPE, strlen(CTNTTYPE));
        sprintf(buf, "%u\r\n\r\n", (unsigned int)out_jpg_len);
        client.write(buf, strlen(buf));
        client.write((char *)out_jpg, out_jpg_len);
        client.write(BOUNDARY, strlen(BOUNDARY));
        free(out_jpg);
      } else if (out_jpg) {
        free(out_jpg);
      }
    }

    vTaskDelayUntil(&xLastWakeTime, xPeriod);
  }

  client.stop();
  vTaskDelete(NULL);
}

void handle_jpg_stream() {
  WiFiClient *client = new WiFiClient(server.client());
  xTaskCreatePinnedToCore(
    streamTask,
    "streamTask",
    12288,
    (void *)client,
    1,
    NULL,
    1
  );
}

// ────────────────────────────────────────────────────────────────────
// Captura alta calidad color
// ────────────────────────────────────────────────────────────────────
bool captureHighResColor(uint8_t **outBuf, size_t *outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  bool restoreFlashAfterCapture = flashManualOn;

  if (!initCamera(CAM_MODE_CAPTURE_COLOR)) {
    Console.println("No se pudo pasar a modo color");
    return false;
  }

  if (restoreFlashAfterCapture) {
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH);
    Console.println("Flash mantenido ON para captura HR");
    delay(150);
  }

  delay(250);

  camera_fb_t *fbDiscard = esp_camera_fb_get();
  if (fbDiscard) {
    esp_camera_fb_return(fbDiscard);
  } else {
    Console.println("Primer frame de descarte fallido");
  }

  delay(200);

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Console.println("Capture failed");
    if (restoreFlashAfterCapture) {
      pinMode(4, OUTPUT);
      digitalWrite(4, LOW);
    }
    initCamera(CAM_MODE_STREAM_GRAY);
    if (restoreFlashAfterCapture) {
      pinMode(4, OUTPUT);
      digitalWrite(4, HIGH);
    }
    return false;
  }

  uint8_t *buf = (uint8_t*) ps_malloc(fb->len);
  if (!buf) {
    Console.println("ps_malloc failed");
    esp_camera_fb_return(fb);
    if (restoreFlashAfterCapture) {
      pinMode(4, OUTPUT);
      digitalWrite(4, LOW);
    }
    initCamera(CAM_MODE_STREAM_GRAY);
    if (restoreFlashAfterCapture) {
      pinMode(4, OUTPUT);
      digitalWrite(4, HIGH);
    }
    return false;
  }

  memcpy(buf, fb->buf, fb->len);
  *outBuf = buf;
  *outLen = fb->len;

  Console.printf("Captura color valida: %u bytes\n", (unsigned)*outLen);

  esp_camera_fb_return(fb);

  if (restoreFlashAfterCapture) {
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    Console.println("Flash apagado tras captura HR, antes de SD");
  }

  if (!initCamera(CAM_MODE_STREAM_GRAY)) {
    Console.println("ERROR: no se pudo restaurar modo stream");
  }

  if (restoreFlashAfterCapture) {
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH);
    Console.println("Flash restaurado tras volver a modo stream");
  }

  return true;
}

// ────────────────────────────────────────────────────────────────────
// Upload task: guarda y envía fuera de motionTask
// ────────────────────────────────────────────────────────────────────
void uploadTask(void *pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint8_t* localBuf = NULL;
    size_t localLen = 0;

    if (!takePendingHighResImage(&localBuf, &localLen)) {
      continue;
    }

    bool uploadSuccess = false;

    if (!saveBufferToSDRoot(localBuf, localLen)) {
      Console.println("-> No se pudo guardar la imagen en SD");
    }

    if (mqttClient.connected()) {
      uploadSuccess = sendImageInBatches(localBuf, localLen);
      if (uploadSuccess) {
        Console.println("-> Foto enviada a ThingsBoard correctamente");
      } else {
        Console.println("-> Error enviando foto a ThingsBoard");
      }
    } else {
      Console.println("-> MQTT no conectado. No se envió la foto.");
    }

    free(localBuf);
    clearHighResState(uploadSuccess);
  }
}

// ────────────────────────────────────────────────────────────────────
// Detección de movimiento
// ────────────────────────────────────────────────────────────────────
void motionTask(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(camMutex, portMAX_DELAY)) {
      if (currentCamMode != CAM_MODE_STREAM_GRAY) {
        xSemaphoreGive(camMutex);
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        xSemaphoreGive(camMutex);
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      uint8_t *current_frame = fb->buf;
      int frame_size = fb->len;

      if (prev_frame == NULL) {
        prev_frame = (uint8_t *) ps_malloc(frame_size);
        if (prev_frame != NULL) {
          memcpy(prev_frame, current_frame, frame_size);
        } else {
          Console.println("Error reservando memoria para prev_frame");
        }

        esp_camera_fb_return(fb);
        xSemaphoreGive(camMutex);
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      long total_diff = 0;
      for (int i = 0; i < frame_size; i += 2) {
        int diff = abs(current_frame[i] - prev_frame[i]);
        if (diff >= PIXEL_DIFF_THRESHOLD) {
          total_diff += diff;
        }
      }

      memcpy(prev_frame, current_frame, frame_size);
      esp_camera_fb_return(fb);

      Console.printf("total_diff=%ld\n", total_diff);

      if (total_diff > MOTION_THRESHOLD) {
        bool locked = tryLockHighResSlot();

        if (!locked) {
          xSemaphoreGive(camMutex);
          vTaskDelay(pdMS_TO_TICKS(500));
          continue;
        }

        Console.println("\n⚠️ ¡Movimiento detectado!");

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;

        bool ok = captureHighResColor(&jpg_buf, &jpg_len);

        xSemaphoreGive(camMutex);

        if (ok) {
          bool stored = setPendingHighResImage(jpg_buf, jpg_len);
          if (stored) {
            Console.println("-> Imagen HR preparada para SD/MQTT");
            if (uploadTaskHandle != NULL) {
              xTaskNotifyGive(uploadTaskHandle);
            }
            jpg_buf = NULL;
          } else {
            Console.println("-> No se pudo registrar la imagen HR pendiente");
          }
        } else {
          Console.println("-> Error capturando imagen color");
        }

        if (jpg_buf) {
          free(jpg_buf);
          clearHighResState(false);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
      } else {
        xSemaphoreGive(camMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ────────────────────────────────────────────────────────────────────
// Comandos debug Serial/Telnet
// ────────────────────────────────────────────────────────────────────
void processDebugCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.equalsIgnoreCase("F")) {
    toggleFlashManual();
    return;
  }

  if (cmd.equalsIgnoreCase("FOFF")) {
    setFlashManual(false);
    return;
  }

  if (cmd.equalsIgnoreCase("FON")) {
    setFlashManual(true);
    return;
  }

  if (cmd.equalsIgnoreCase("ESTADO")) {
    Console.println("=== ESTADO ===");
    Console.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "conectada" : "desconectada");
    if (WiFi.status() == WL_CONNECTED) {
      Console.printf("SSID: %s\n", WiFi.SSID().c_str());
      Console.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Console.printf("RSSI: %d\n", WiFi.RSSI());
    }
    Console.printf("MQTT: %s\n", mqttClient.connected() ? "conectado" : "desconectado");
    Console.printf("Attrs subscribed: %s\n", mqttAttrsSubscribed ? "si" : "no");
    Console.printf("SD: %s\n", sd_ok ? "montada" : "no disponible");
    Console.printf("Hora NTP: %s\n", timeSynced ? "sincronizada" : "no sincronizada");
    Console.printf("Flash manual: %s\n", flashManualOn ? "ON" : "OFF");
    return;
  }

  Console.print("Comando desconocido: ");
  Console.println(cmd);
}

void checkSerialCommand() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  processDebugCommand(cmd);
}

void checkTelnetCommand() {
  if (!TelnetStream.available()) return;
  String cmd = TelnetStream.readStringUntil('\n');
  processDebugCommand(cmd);
}

// ────────────────────────────────────────────────────────────────────
// Setup
// ────────────────────────────────────────────────────────────────────
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(1000);

  Console.println("\nBooting ESP32-CAM intrusión...");

  forceFlashOff();

  if (!initCamera(CAM_MODE_STREAM_GRAY)) {
    Console.println("Fallo inicializando cámara en modo stream");
    delay(3000);
    ESP.restart();
  }

  if (!connectWiFiWithManager(false)) {
    Console.println("No se pudo conectar/configurar WiFi, reiniciando...");
    delay(3000);
    ESP.restart();
  }

  TelnetStream.begin();
  Console.println("Telnet iniciado, puerto 23.");

  syncTimeFromNTP();

  sd_ok = initSDCard();
  if (!sd_ok) {
    Console.println("Continuando sin SD.");
  }

  forceFlashOff();

  Console.print("Stream: http://");
  Console.print(WiFi.localIP());
  Console.println("/mjpeg/1");

  Console.println("Comandos disponibles:");
  Console.println("  F     -> alterna flash");
  Console.println("  FON   -> enciende flash");
  Console.println("  FOFF  -> apaga flash");
  Console.println("  ESTADO -> muestra estado general");

  configureMqttByCurrentSSID();
  mqttClient.setBufferSize(6144);
  mqttClient.setKeepAlive(60);
  mqttClient.setCallback(mqttCallback);

  camMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
    motionTask,
    "motionTask",
    16384,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    uploadTask,
    "uploadTask",
    12288,
    NULL,
    1,
    &uploadTaskHandle,
    1
  );

  server.on("/mjpeg/1", HTTP_GET, handle_jpg_stream);
  server.begin();
  Console.println("Servidor HTTP iniciado.");
}

// ────────────────────────────────────────────────────────────────────
// Loop
// ────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFiConnected();
    if (WiFi.status() == WL_CONNECTED) {
      syncTimeFromNTP();
      configureMqttByCurrentSSID();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
    mqttClient.loop();
  }

  checkSerialCommand();
  checkTelnetCommand();
  checkCooldownFinished();

  delay(1);
}
