#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include "driver/ledc.h"
#include "CamConfig.h"
#include "DoorAudio.h"
#include "DoorVideo.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

const char *ssid = "MyHome_555";
const char *password = "anton123";
const char *dns_cam = "esp32-door-cam";

WebServer server(80);
httpd_handle_t camera_httpd = NULL;
// Флаг остановки стрима: httpd_stop() ждет выхода из stream_handler,
// поэтому бесконечный цикл должен сам увидеть запрос на остановку
volatile bool stream_stop_requested = false;

static esp_err_t stream_handler(httpd_req_t *req) {
    Serial.println("[ENTER] stream_handler | params: httpd_req_t *req");

    if (!doorVideo.isInitialized()) {
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (!stream_stop_requested) {
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

// Одиночный JPEG снимок: http://host:81/capture
static esp_err_t capture_handler(httpd_req_t *req) {
    Serial.println("[ENTER] capture_handler | params: httpd_req_t *req");
    if (!doorVideo.isInitialized()) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[EXIT ERROR] capture_handler | error: Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    Serial.printf("[EXIT] capture_handler | return: code %d\n", res);
    return res;
}

// Состояние системы и ресурсов для тестовой панели: GET /api/status
static void handleStatus() {
    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();

    String json;
    json.reserve(640);
    json += "{\"uptime_s\":";      json += millis() / 1000;
    json += ",\"heap_free\":";     json += ESP.getFreeHeap();
    json += ",\"heap_min\":";      json += ESP.getMinFreeHeap();
    json += ",\"psram_free\":";    json += ESP.getFreePsram();
    json += ",\"psram_total\":";   json += ESP.getPsramSize();
    json += ",\"dma_free\":";      json += heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    json += ",\"dma_largest\":";   json += heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    json += ",\"wifi_mode\":\"";  json += (WiFi.getMode() == WIFI_AP) ? "AP" : "STA";
    json += "\",\"ip\":\"";      json += (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    json += "\",\"rssi\":";       json += (WiFi.getMode() == WIFI_AP) ? 0 : WiFi.RSSI();

    json += ",\"cam\":{\"init\":"; json += doorVideo.isInitialized() ? "true" : "false";
    json += ",\"streaming\":";      json += (camera_httpd != NULL) ? "true" : "false";
    sensor_t *s = doorVideo.isInitialized() ? esp_camera_sensor_get() : NULL;
    if (s) {
        json += ",\"framesize\":";  json += s->status.framesize;
        json += ",\"quality\":";    json += s->status.quality;
        json += ",\"brightness\":"; json += s->status.brightness;
        json += ",\"contrast\":";   json += s->status.contrast;
        json += ",\"vflip\":";      json += s->status.vflip;
        json += ",\"hmirror\":";    json += s->status.hmirror;
    }
    json += "},\"mic\":{\"ready\":"; json += DoorAudio::micReady ? "true" : "false";
    json += ",\"recording\":";      json += DoorAudio::isRecording ? "true" : "false";
    json += "},\"spk\":{\"ready\":"; json += DoorAudio::speakerReady ? "true" : "false";
    json += "},\"fs\":{\"total\":";  json += fsTotal;
    json += ",\"used\":";           json += fsUsed;
    json += "}}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// Настройка камеры с сохранением в NVS: GET /cam/set?var=quality&val=12
static void handleCamSet() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!server.hasArg("var") || !server.hasArg("val")) {
        server.send(400, "text/plain", "Bad Request: need var and val");
        return;
    }
    sensor_t *s = doorVideo.isInitialized() ? esp_camera_sensor_get() : NULL;
    if (!s) {
        server.send(500, "text/plain", "Camera not initialized");
        return;
    }
    String var = server.arg("var");
    int val = server.arg("val").toInt();
    if (!camConfig.applyCameraSetting(s, var.c_str(), val)) {
        server.send(400, "text/plain", "Unknown or rejected setting: " + var);
        return;
    }
    server.send(200, "text/plain", "OK: " + var + "=" + String(val));
}

// Главная страница: панель, загруженная в LittleFS через /api/upload
static void handleRoot() {
    const char *panelPath = "/esp32_panel.html";
    if (LittleFS.exists(panelPath)) {
        File f = LittleFS.open(panelPath, "r");
        server.streamFile(f, "text/html");
        f.close();
        return;
    }
    server.send(200, "text/plain",
        "ESP32 Door Cam\n"
        "Panel not found: upload esp32_panel.html via POST /api/upload\n");
}

// bool initCamera(){
//     if (camera_initialized) {
//         Serial.println("[ENTER] initCamera yeat");
//         return camera_initialized;
//     }
//     Serial.println("[ENTER] initCamera");

//     camera_config_t config;
//     config.ledc_channel = LEDC_CHANNEL_0;
//     config.ledc_timer = LEDC_TIMER_0;
//     config.pin_d0 = Y2_GPIO_NUM;
//     config.pin_d1 = Y3_GPIO_NUM;
//     config.pin_d2 = Y4_GPIO_NUM;
//     config.pin_d3 = Y5_GPIO_NUM;
//     config.pin_d4 = Y6_GPIO_NUM;
//     config.pin_d5 = Y7_GPIO_NUM;
//     config.pin_d6 = Y8_GPIO_NUM;
//     config.pin_d7 = Y9_GPIO_NUM;
//     config.pin_xclk = XCLK_GPIO_NUM;
//     config.pin_pclk = PCLK_GPIO_NUM;
//     config.pin_vsync = VSYNC_GPIO_NUM;
//     config.pin_href = HREF_GPIO_NUM;
//     config.pin_sccb_sda = SIOD_GPIO_NUM;
//     config.pin_sccb_scl = SIOC_GPIO_NUM;
//     config.pin_pwdn = PWDN_GPIO_NUM;
//     config.pin_reset = RESET_GPIO_NUM;
//     config.xclk_freq_hz = 20000000;
//     config.pixel_format = PIXFORMAT_JPEG;
//     config.grab_mode = CAMERA_GRAB_LATEST; 
//     config.fb_location = CAMERA_FB_IN_PSRAM;
//     config.jpeg_quality = 12;
//     config.fb_count = 1;

//     if (psramFound()) {
//         config.frame_size = FRAMESIZE_SVGA;
//         config.jpeg_quality = 10;
//         config.fb_count = 2;
//     } else {
//         config.frame_size = FRAMESIZE_VGA;
//         config.fb_location = CAMERA_FB_IN_DRAM;
//     }

//     esp_err_t err = esp_camera_init(&config);
//     if (err == ESP_OK) {
//         camera_initialized = true;
//         sensor_t *s = esp_camera_sensor_get();
//         camConfig.loadCameraSettings(s);
//         Serial.println("[TRACE] Camera initialized successfully");
//     } else {
//         Serial.printf("[EXIT ERROR] Camera init failed (0x%x). Running without camera stream.\n", err);
//     }

//     Serial.println("[EXIT] initCamera");
//     return camera_initialized;
// }

void startCameraServer() {
    Serial.println("[ENTER] startCameraServer | params: none");
    
// ЗАЩИТА: Если сервер уже работает, ничего не делаем
    if (camera_httpd != NULL) {
        Serial.println("[TRACE] Camera server is already running");
        return;
    }

    if ( !doorVideo.initCamera() ) {
        Serial.println("[EXIT] startCameraServer initCamera false");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.core_id = 0;            // Видео на Core 0, аудио и loop() на Core 1
    config.max_open_sockets = 2;   // Один зритель + запасной, экономим сокеты lwIP
    config.lru_purge_enable = true; // Новый клиент вытесняет зависшее соединение

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    stream_stop_requested = false;
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
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

// 1. СТРОГО ПЕРЕД WI-FI И FS: Захватываем DMA память
    doorVideo.initCamera();
    doorVideo.stop(); // Сразу переводим в сон до вызова /cam/start

// 2. СРАЗУ аппаратно отключаем тактирование (сенсор полностью замирает)
    // На архитектуре ESP32-S3 для камеры используется низкоскоростной таймер
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);

// 3. Тоже ДО Wi-Fi: ставим оба I2S драйвера, пока DMA память не раздроблена.
    // Микрофон и динамик сразу засыпают (i2s_stop) до запроса
    doorAudio.begin();

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

    // 1. Эндпоинт прямого потокового воспроизведения на I2S ЦАП
    server.on("/api/stream_play", HTTP_POST, []() {
        DoorAudio::handlePlayStreamReq(server);
    }, []() {
        DoorAudio::handlePlayStreamUpload(server);
    });

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/cam/set", HTTP_GET, handleCamSet);

    // Файлы в LittleFS: список, проигрывание на динамик, удаление
    server.on("/api/files", HTTP_GET, []() {
        DoorAudio::handleListFiles(server);
    });
    server.on("/api/play_file", HTTP_GET, []() {
        DoorAudio::handlePlayFile(server);
    });
    server.on("/api/delete", HTTP_GET, []() {
        DoorAudio::handleDeleteFile(server);
    });

    // 2. Эндпоинт загрузки файла во внутреннюю память
    server.on("/api/upload", HTTP_POST, []() {
        DoorAudio::handleFileUploadReq(server);
    }, []() {
        DoorAudio::handleFileUploadData(server);
    });

    server.on("/mic/start", HTTP_GET, []() {
        DoorAudio::handleStartMic(server);
    });

    server.on("/mic/stop", HTTP_GET, []() {
        DoorAudio::handleStopMic(server);
    });

    server.on("/spr/start", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        if (!DoorAudio::speakerReady) {
            server.send(500, "text/plain", "Speaker not initialized");
            return;
        }
        doorAudio.playBeep();
        server.send(200, "text/plain", "Beep played");
    });

    server.on("/cam/start", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        doorVideo.start();
        if (camera_httpd != NULL) {
            server.send(200, "text/plain", "Camera started");
        } else {
            server.send(500, "text/plain", "Camera start failed");
        }
    });

    server.on("/cam/stop", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        doorVideo.stop();
        server.send(200, "text/plain", "Camera stopped");
    });


    server.begin();
    
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