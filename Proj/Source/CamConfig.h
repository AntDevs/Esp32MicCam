#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "esp_camera.h"

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

class CamConfig {
private:
    Preferences prefs;
    
public:
    CamConfig() {}
    
    // Работа с настройками изображения
    void loadCameraSettings(sensor_t *s);
    void saveCameraSetting(const char* key, int value);
    
    // Работа с учетными данными Wi-Fi
    void saveWiFiCredentials(String ssid, String password);
    String getWiFiSSID(String default_ssid);
    String getWiFiPassword(String default_pass);
};

// Экспортируем единственный глобальный экземпляр объекта
extern CamConfig camConfig;