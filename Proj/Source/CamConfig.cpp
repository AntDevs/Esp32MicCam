#include "CamConfig.h"

// Создание экземпляра
CamConfig camConfig;

// Маппинг параметров через C++11 лямбды
typedef int (*ApplyFunc)(sensor_t *s, int val);

struct CameraSetting {
    const char* key;
    int default_val;
    ApplyFunc apply_func;
};

const CameraSetting SETTINGS[] = {
    {"framesize",  8,  [](sensor_t *s, int val){ return s->set_framesize(s, (framesize_t)val); }}, // 8 = FRAMESIZE_VGA
    {"quality",    20, [](sensor_t *s, int val){ return s->set_quality(s, val); }},
    {"brightness", 0,  [](sensor_t *s, int val){ return s->set_brightness(s, val); }},
    {"contrast",   0,  [](sensor_t *s, int val){ return s->set_contrast(s, val); }},
    {"vflip",      0,  [](sensor_t *s, int val){ return s->set_vflip(s, val); }},
    {"hmirror",    0,  [](sensor_t *s, int val){ return s->set_hmirror(s, val); }}
};
const int SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);

// === Реализация методов ===

void CamConfig::loadCameraSettings(sensor_t *s) {
    Serial.println("CamConfig loadCameraSettings");
    if (!s) return;
    prefs.begin("camera", true);
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        int val = prefs.getInt(SETTINGS[i].key, SETTINGS[i].default_val);
        SETTINGS[i].apply_func(s, val);
    }
    prefs.end();
}

void CamConfig::saveCameraSetting(const char* key, int value) {
    Serial.println("CamConfig saveCameraSetting");
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        if (strcmp(SETTINGS[i].key, key) == 0) {
            prefs.begin("camera", false);
            prefs.putInt(key, value);
            prefs.end();
            return;
        }
    }
}

void CamConfig::saveWiFiCredentials(String ssid, String password) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);    
    prefs.end();
    Serial.printf("CamConfig saveWiFiCredentials new ssid:%s \n", ssid);
}

String CamConfig::getWiFiSSID(String default_ssid) {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", default_ssid);
    prefs.end();
    return ssid;
}

String CamConfig::getWiFiPassword(String default_pass) {
    prefs.begin("wifi", true);
    String pass = prefs.getString("password", default_pass);
    prefs.end();
    return pass;
}