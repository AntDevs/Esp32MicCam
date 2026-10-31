#ifndef DOOR_AUDIO_H
#define DOOR_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include "esp_camera.h"

#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_BCLK 42
#define I2S_MIC_LRC  41
#define I2S_MIC_DOUT 40

#define I2S_SPK_PORT I2S_NUM_1
#define I2S_SPK_BCLK 1  // BCLK orang Аппаратный GPIO1 (Пин D0)
#define I2S_SPK_LRC  4  // LRCK yellow WS Аппаратный GPIO4 (Пин D3)
#define I2S_SPK_DIN  5  // DIN / SDATA broun Аппаратный GPIO5 (Пин D4)


class DoorAudio {
public:
    void begin();
    void initI2SSpeaker();    
    // void sendSound();
    static void handlePlayTTS(WebServer& server);
    static void handleStartMic(WebServer& server);
    static void handleStopMic(WebServer& server);
    
    // Новые методы стриминга и загрузки
    static void handlePlayStreamReq(WebServer& server);
    static void handlePlayStreamUpload(WebServer& server);
    
    static void handleFileUploadReq(WebServer& server);
    static void handleFileUploadData(WebServer& server);    

    static volatile bool isRecording;
private:
    void initI2SMic();

    static void logInfo(const char* msg);
    static void logError(const char* msg);

};

extern DoorAudio doorAudio;
#endif