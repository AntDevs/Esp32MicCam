#ifndef DOOR_SERVER_H
#define DOOR_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "esp_http_server.h"
#include "CamConfig.h"

// Application server: Wi-Fi, mDNS, WebServer (порт 80, API и панель)
// и httpd камеры (порт 81, /stream и /capture)
class DoorServer {
    public:
        // Wi-Fi + mDNS + регистрация маршрутов. Вызывать ПОСЛЕ инициализации камеры и I2S
        void begin();
        void handleClient();

        bool startCameraServer();
        void stopCameraServer();
        bool isCameraServerRunning() const;
    private:
        void initWiFi();
        void registerRoutes();

        const char *ssid = "MyHome_555";
        const char *password = "anton123";
        const char *dns_cam = "esp32-door-cam";
};

extern DoorServer doorServer;
#endif
