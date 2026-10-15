#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "CamConfig.h"
#include <stdexcept>
#include "esp_log.h" // ДОБАВЛЕНО: для управления системными логами драйвера

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Enter your default WiFi credentials
// http://esp32-pigeon-cam.local
// ===========================
const char *ssid = "MyHome_555";
const char *password = "anton123";
const char *dns_cam = "esp32-pigeon-cam";

void startCameraServer();
void setupLedFlash();

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  
  // [ENTER TRACE]
  Serial.println("[ENTER] setup | params: none");

  try {
    // =========================================================
    // 1. ИНИЦИАЛИЗАЦИЯ КАМЕРЫ
    // =========================================================
    camera_config_t config;
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
    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST; 
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 1;

    if (config.pixel_format == PIXFORMAT_JPEG) {
      if (psramFound()) {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
      } else {
        config.frame_size = FRAMESIZE_VGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
      }
    } else {
      config.frame_size = FRAMESIZE_UXGA;
#if CONFIG_IDF_TARGET_ESP32S3
      config.fb_count = 2;
#endif
    }

#if defined(CAMERA_MODEL_ESP_EYE)
    pinMode(13, INPUT_PULLUP);
    pinMode(14, INPUT_PULLUP);
#endif

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      throw std::runtime_error("esp_camera_init failed");
    }

    sensor_t *s = esp_camera_sensor_get();
    camConfig.loadCameraSettings(s);
    Serial.println("[TRACE] CamConfig loadCameraSettings completed");
    
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);        
      s->set_brightness(s, 1);   
      s->set_saturation(s, -2);  
    }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
    s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
    setupLedFlash();
#endif

    // =========================================================
    // 2. ОТКЛЮЧЕНИЕ СИСТЕМНОГО СПАМА (Аппаратная маскировка)
    // =========================================================
    // Глушим логи драйвера камеры, чтобы переполнение буфера 
    // обрабатывалось в фоне тихо и не вешало систему.
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);

    // =========================================================
    // 3. ИНИЦИАЛИЗАЦИЯ WI-FI С ТАЙМАУТОМ И ТОЧКОЙ ДОСТУПА
    // =========================================================
    String current_ssid = camConfig.getWiFiSSID(ssid);
    String current_pass = camConfig.getWiFiPassword(password);

    Serial.printf("[TRACE] WiFi connecting | target_ssid: %s:%s\n", current_ssid.c_str(), current_pass.c_str());

    WiFi.setHostname(dns_cam);
    // Начинаем только с WIFI_STA (как в вашем рабочем коде)
    WiFi.mode(WIFI_STA); 
    WiFi.begin(current_ssid.c_str(), current_pass.c_str());
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setSleep(false);

    int attempts = 0;
    // Используем классический надежный delay
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500); 
      Serial.print(".");
      attempts++;
    }
    Serial.println("");

    // IPAddress activeIP;

    if (WiFi.status() == WL_CONNECTED) {
      // activeIP = WiFi.localIP();
      Serial.printf("[TRACE] WiFi connected | connected_ssid: %s, IP: %s\n", WiFi.SSID().c_str(), "activeIP.toString().c_str()");
    } else {
      Serial.println("[TRACE] WiFi connection failed! Starting Access Point...");
      // Включаем точку доступа только если не удалось подключиться
      WiFi.mode(WIFI_AP);
      WiFi.softAP("Door-Setup", "12345678"); 
      // activeIP = WiFi.softAPIP();
      Serial.printf("[TRACE] AP Started | connected_ssid: Door-Setup (AP Mode), IP: %s\n", "activeIP.toString().c_str()");
    }

    // Возвращаем штатные логи камеры (если нужно видеть другие предупреждения)
    esp_log_level_set("cam_hal", ESP_LOG_WARN);

    // =========================================================
    // 4. ЗАПУСК СЕРВЕРОВ
    // =========================================================
    if (MDNS.begin(dns_cam)) {
      Serial.printf("[TRACE] mDNS responder started: http://%s.local\n", dns_cam);
    } else {
      Serial.println("[TRACE] Error setting up MDNS responder!");
    }

    startCameraServer();

    Serial.println("==================================================");
    Serial.printf("[TRACE] Admin Panel Link: http://%s/\n", "activeIP.toString().c_str()");
    Serial.printf("[TRACE] Admin Panel mDNS: http://%s.local/\n", dns_cam);
    Serial.printf("[TRACE] Streaming Link:   http://%s:81/stream\n", "activeIP.toString().c_str()");
    Serial.printf("[TRACE] Streaming mDNS:   http://%s.local:81/stream\n", dns_cam);
    Serial.println("==================================================");

  } 
  catch (const std::exception& e) {
    Serial.printf("[EXIT ERROR] setup | error: %s\n", e.what());
    return; 
  }

  // [EXIT TRACE]
  Serial.println("[EXIT] setup | return: void");
}

void loop() {
  delay(10000);
}