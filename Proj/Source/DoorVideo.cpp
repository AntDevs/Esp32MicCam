#include "driver/ledc.h"
#include "DoorVideo.h"

DoorVideo doorVideo;

//bool camera_initialized = false;

// Делаем переменные из .ino файла видимыми здесь
extern httpd_handle_t camera_httpd;
extern volatile bool stream_stop_requested;
extern void startCameraServer();

void DoorVideo::start() {
    if ( !initCamera() ) {
        Serial.println("[EXIT ERROR] DoorVideo::start | error: initCamera false");
        return;
    }

// 1. Подаем тактовый сигнал, пробуждая матрицу
    ledc_timer_resume(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    delay(200); // Даем сенсору 200 мс на стабилизацию напряжения и света
    
    // 2. Очищаем DMA от старого или битого переходного кадра
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);

    // Явно глобальная функция из .ino (раньше вызывался пустой метод класса)
    ::startCameraServer();
}

void DoorVideo::stop() {
    Serial.println("[ENTER] DoorVideo::stop");
// 1. Освобождаем сетевые ресурсы и сокеты
    if (camera_httpd != NULL) {
        // Сначала просим stream_handler выйти из цикла, иначе httpd_stop() ждет вечно
        stream_stop_requested = true;
        httpd_stop(camera_httpd);
        camera_httpd = NULL;
        Serial.println("[TRACE] Streaming server stopped (Port 81 closed)");
    }
    
    // // 2. Уводим камеру в энергосберегающий режим (останавливает генерацию кадров)
    // sensor_t *s = esp_camera_sensor_get();
    // if (s != NULL) {
    //     s->set_pwdn(s, 1); 
    // }
    // Serial.println("[EXIT] DoorVideo::stop - Camera sleeping, CPU idle");    

    // 2. Аппаратно замораживаем сенсор (остановка XCLK)
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);    
    Serial.println("[EXIT] DoorVideo::stop - Camera XCLK paused, CPU free");
    
}


void DoorVideo::checkMem(){
    Serial.println("=== Memory Diagnostics ===");    
    // Serial.printf("Внутренняя память (Heap): %d байт свободно\n", ESP.getFreeHeap());
    // if (psramFound()) {
    //     Serial.printf("Внешняя память (PSRAM): %d байт свободно\n", ESP.getFreePsram());
    // } else {
    //     Serial.println("ВНИМАНИЕ: PSRAM НЕ НАЙДЕНА ИЛИ ОТКЛЮЧЕНА В НАСТРОЙКАХ!");
    // }
    Serial.printf("Total heap: %u\n", ESP.getHeapSize());
    Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
    Serial.printf("Total PSRAM: %u\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %u\n", ESP.getFreePsram());
    if (psramFound()) {
        Serial.println("PSRAM is successfully initialized!");
    } else {
        Serial.println("WARNING: PSRAM is NOT found! Camera will fail.");
    }
    Serial.println("==========================");    
}


bool DoorVideo::initCamera(){
    if (camera_initialized) {
        Serial.println("[ENTER] initCamera yeat");
        return camera_initialized;
    }
    Serial.println("[ENTER] initCamera");
    checkMem();

    camera_config_t config = {}; // Обнуляем, чтобы в незаданных полях не было мусора
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
    config.xclk_freq_hz = 10000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST; 
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 1;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 10;
        config.fb_count = 3;
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

