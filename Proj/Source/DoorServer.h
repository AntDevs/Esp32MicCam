#ifndef DOOR_SERVER_H
#define DOOR_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "CamConfig.h"
// #include <HTTPClient.h>
#include <WebServer.h>


class DoorServer {
    public:
        void init();
    private:
        const char *ssid = "MyHome_555";
        const char *password = "anton123";
        const char *dns_cam = "esp32-door-cam";

};

extern DoorServer doorServer;
#endif