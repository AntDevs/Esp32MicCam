#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "esp_camera.h"

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