#ifndef DOOR_VIDEO_H
#define DOOR_VIDEO_H

#include <Arduino.h>
#include <WebServer.h>
#include "CamConfig.h"
// #include <ESPmDNS.h>
// #include <LittleFS.h>
// #include "CamConfig.h"
// #include "DoorAudio.h"
#include "esp_http_server.h"

class DoorVideo {
    public:
        void start();
        void stop();
        bool initCamera();
    private:
        void checkMem();
        bool camera_initialized = false;
        void startCameraServer();
};

extern DoorVideo doorVideo;
#endif