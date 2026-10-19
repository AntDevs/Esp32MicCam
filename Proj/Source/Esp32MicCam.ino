#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include "CamConfig.h"
#include "DoorAudio.h"
#include "esp_http_server.h"

// =========================================================
// ПИНЫ КАМЕРЫ ДЛЯ SEEED STUDIO XIAO ESP32S3 SENSE
// =========================================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

const char *ssid = "MyHome_555";
const char *password = "anton123";
const char *dns_cam = "esp32-door-cam";

WebServer server(80);
httpd_handle_t camera_httpd = NULL;
bool camera_initialized = false;

static esp_err_t stream_handler(httpd_req_t *req) {
    Serial.println("[ENTER] stream_handler | params: httpd_req_t *req");

    if (!camera_initialized) {
        Serial.println("[EXIT ERROR] stream_handler | error: Camera not initialized");
        httpd_resp_send_500(req); 
        return ESP_FAIL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[EXIT ERROR] stream_handler | error: Camera capture failed on initial request");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_camera_fb_return(fb);

    char *part_buf[64];
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
    static const char* _STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        Serial.printf("[EXIT ERROR] stream_handler | error: httpd_resp_set_type failed code %d\n", res);
        return res;
    }

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[TRACE] stream_handler | error: Failed to get frame, breaking stream");
            res = ESP_FAIL;
            break;
        }
        
        size_t _jpg_buf_len = fb->len;
        uint8_t *_jpg_buf = fb->buf;

        res = httpd_resp_send_chunk(req, (const char *)part_buf, snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));

        esp_camera_fb_return(fb);
        
        delay(1); // ПРЕДОТВРАЩЕНИЕ FB-OVF И БЛОКИРОВКИ ЯДРА WI-FI
        
        if (res != ESP_OK) {
            Serial.println("[TRACE] stream_handler | error: Failed to send chunk, breaking stream");
            break;
        }
    }
    
    Serial.printf("[EXIT] stream_handler | return: code %d\n", res);
    return res;
}

bool initCamera(){
    if (camera_initialized) {
        Serial.println("[ENTER] initCamera yeat");
        return camera_initialized;
    }
    Serial.println("[ENTER] initCamera");

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
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST; 
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 1;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_VGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
        camera_initialized = true;
        sensor_t *s = esp_camera_sensor_get();
        camConfig.loadCameraSettings(s);
        Serial.println("[TRACE] Camera initialized successfully");
    } else {
        Serial.printf("[EXIT ERROR] Camera init failed (0x%x). Running without camera stream.\n", err);
    }

    Serial.println("[EXIT] initCamera");
    return camera_initialized;
}

void startCameraServer() {
    Serial.println("[ENTER] startCameraServer | params: none");
    
    if ( !initCamera() ) {
        Serial.println("[EXIT] startCameraServer initCamera false");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.core_id = 0; // Строго по скиллу: тяжелый процесс камеры на Core 0

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        Serial.println("[INFO] Camera streaming server started on port 81");
    } else {
        Serial.println("[EXIT ERROR] startCameraServer | error: Failed to start httpd");
        return;
    }
    
    Serial.println("[EXIT] startCameraServer | return: void");
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n[ENTER] setup | params: none");

    // Инициализация файловой системы для загрузки файлов
    if (!LittleFS.begin(true)) {
        Serial.println("[TRACE] setup | error: LittleFS Mount Failed");
    }

    String current_ssid = camConfig.getWiFiSSID(ssid);
    String current_pass = camConfig.getWiFiPassword(password);

    Serial.printf("[TRACE] WiFi connecting | target_ssid: %s\n", current_ssid.c_str());
    WiFi.setHostname(dns_cam);
    WiFi.mode(WIFI_STA);
    WiFi.begin(current_ssid.c_str(), current_pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[TRACE] WiFi connected | IP: %s\n", dns_cam);
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Door-Setup", "12345678");
        Serial.printf("[TRACE] AP Mode Started | IP: %s\n", dns_cam);
    }

    if (MDNS.begin(dns_cam)) {
        Serial.printf("[TRACE] mDNS started: http://%s.local\n", dns_cam);
    }

    doorAudio.begin();

    server.on("/mic/start", HTTP_GET, []() {
        DoorAudio::handleStartMic(server);
    });

    server.on("/mic/stop", HTTP_GET, []() {
        DoorAudio::handleStopMic(server);
    });

    server.on("/play", HTTP_POST, []() {
        DoorAudio::handlePlayTTS(server);
    });

    // 1. Эндпоинт прямого потокового воспроизведения на I2S ЦАП
    server.on("/api/stream_play", HTTP_POST, []() {
        DoorAudio::handlePlayStreamReq(server);
    }, []() {
        DoorAudio::handlePlayStreamUpload(server);
    });

    // 2. Эндпоинт загрузки файла во внутреннюю память
    server.on("/api/upload", HTTP_POST, []() {
        DoorAudio::handleFileUploadReq(server);
    }, []() {
        DoorAudio::handleFileUploadData(server);
    });

    server.begin();
    startCameraServer();
    
    Serial.println("==================================================");
    Serial.printf("[TRACE] Admin Panel: http://%s.local/\n", dns_cam);
    Serial.printf("[TRACE] Streaming:   http://%s.local:81/stream\n", dns_cam);
    Serial.println("==================================================");
    Serial.println("[EXIT] setup | return: void");
}

void loop() {
    server.handleClient();
    delay(2);
}