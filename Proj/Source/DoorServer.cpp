#include "DoorServer.h"
#include <LittleFS.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "DoorAudio.h"
#include "DoorVideo.h"

DoorServer doorServer;

static WebServer server(80);
static httpd_handle_t camera_httpd = NULL;
// Флаг остановки стрима: httpd_stop() ждет выхода из stream_handler,
// поэтому бесконечный цикл должен сам увидеть запрос на остановку
static volatile bool stream_stop_requested = false;

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

// Одиночный JPEG снимок: GET http://host/capture (порт 80).
// Не на порту 81: у httpd одна задача, и пока stream_handler крутит цикл, второй запрос туда не попадет.
static void handleCapture() {
    Serial.println("[ENTER] handleCapture | params: none");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!doorVideo.isInitialized()) {
        server.send(500, "text/plain", "Camera not initialized");
        Serial.println("[EXIT ERROR] handleCapture | error: Camera not initialized");
        return;
    }
    // Камера спит (XCLK на паузе): будим на один кадр и после снимка снова усыпляем
    bool wake = !doorServer.isCameraServerRunning();
    if (wake) doorVideo.start();

    // Стрим (httpd, приоритет 5) тоже ждет в esp_camera_fb_get(), а FreeRTOS отдает новый кадр
    // самой приоритетной ждущей задаче: loopTask (приоритет 1) получал только таймаут 4 с.
    // На время ожидания кадра поднимаемся выше стрима
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 6);
    camera_fb_t *fb = esp_camera_fb_get();
    vTaskPrioritySet(NULL, prio);
    if (!fb) {
        if (wake) doorVideo.stop();
        server.send(500, "text/plain", "Camera capture failed");
        Serial.println("[EXIT ERROR] handleCapture | error: Camera capture failed");
        return;
    }
    server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    size_t sent = server.client().write(fb->buf, fb->len);
    size_t len = fb->len;
    esp_camera_fb_return(fb);
    if (wake) doorVideo.stop();
    Serial.printf("[EXIT] handleCapture | return: sent %u/%u bytes%s\n", sent, len, wake ? " (camera woken)" : "");
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
    json += ",\"saving\":";         json += DoorAudio::isSaving ? "true" : "false";
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

// =========================================================
// ПОДКЛЮЧЕНИЕ: Wi-Fi (STA, при неудаче AP "Door-Setup") + mDNS
// =========================================================
void DoorServer::initWiFi() {
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
}

// =========================================================
// МАРШРУТЫ WebServer (порт 80)
// =========================================================
void DoorServer::registerRoutes() {
    // 1. Эндпоинт прямого потокового воспроизведения на I2S ЦАП
    server.on("/api/stream_play", HTTP_POST, []() {
        DoorAudio::handlePlayStreamReq(server);
    }, []() {
        DoorAudio::handlePlayStreamUpload(server);
    });

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/cam/set", HTTP_GET, handleCamSet);
    server.on("/capture", HTTP_GET, handleCapture);

    // Файлы в LittleFS: список, скачивание, проигрывание на динамик, удаление
    server.on("/api/files", HTTP_GET, []() {
        DoorAudio::handleListFiles(server);
    });
    server.on("/api/play_file", HTTP_GET, []() {
        DoorAudio::handlePlayFile(server);
    });
    server.on("/api/delete", HTTP_GET, []() {
        DoorAudio::handleDeleteFile(server);
    });
    server.on("/api/download", HTTP_GET, []() {
        DoorAudio::handleDownloadFile(server);
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
        if (doorServer.startCameraServer()) {
            server.send(200, "text/plain", "Camera started");
        } else {
            server.send(500, "text/plain", "Camera start failed");
        }
    });

    server.on("/cam/stop", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        // Сначала гасим сервер (stream_handler выходит из цикла), потом замораживаем сенсор
        doorServer.stopCameraServer();
        doorVideo.stop();
        server.send(200, "text/plain", "Camera stopped");
    });
}

void DoorServer::start() {
    Serial.println("[ENTER] DoorServer::start | params: none");
    initWiFi();
    registerRoutes();
    server.begin();

    Serial.println("==================================================");
    Serial.printf("[TRACE] Admin Panel: http://%s.local/\n", dns_cam);
    Serial.printf("[TRACE] Streaming:   http://%s.local:81/stream\n", dns_cam);
    Serial.println("==================================================");
    Serial.println("[EXIT] DoorServer::start | return: void");
}

void DoorServer::handleClient() {
    server.handleClient();
}

// =========================================================
// httpd КАМЕРЫ (порт 81): поднимается по /cam/start, гасится по /cam/stop
// =========================================================
bool DoorServer::startCameraServer() {
    Serial.println("[ENTER] startCameraServer | params: none");

// ЗАЩИТА: Если сервер уже работает, ничего не делаем
    if (camera_httpd != NULL) {
        Serial.println("[TRACE] Camera server is already running");
        return true;
    }

    if ( !doorVideo.initCamera() ) {
        Serial.println("[EXIT] startCameraServer initCamera false");
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.core_id = 0;            // Видео на Core 0, аудио и loop() на Core 1
    config.max_open_sockets = 2;   // Один зритель + запасной (вытесняет зависший), экономим сокеты lwIP
    config.lru_purge_enable = true; // Новый клиент вытесняет зависшее соединение

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    stream_stop_requested = false;
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        Serial.println("[INFO] Camera streaming server started on port 81");
    } else {
        Serial.println("[EXIT ERROR] startCameraServer | error: Failed to start httpd");
        camera_httpd = NULL;
        return false;
    }

    Serial.println("[EXIT] startCameraServer | return: true");
    return true;
}

void DoorServer::stopCameraServer() {
    if (camera_httpd == NULL) return;
    // Сначала просим stream_handler выйти из цикла, иначе httpd_stop() ждет вечно
    stream_stop_requested = true;
    httpd_stop(camera_httpd);
    camera_httpd = NULL;
    Serial.println("[TRACE] Streaming server stopped (Port 81 closed)");
}

bool DoorServer::isCameraServerRunning() const {
    return camera_httpd != NULL;
}
