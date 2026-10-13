#ifndef DOOR_AUDIO_H
#define DOOR_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include "esp_camera.h"

#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_BCLK 42
#define I2S_MIC_LRC  41
#define I2S_MIC_DOUT 40

#define I2S_SPK_PORT I2S_NUM_1
#define I2S_SPK_BCLK 1
#define I2S_SPK_LRC  2
#define I2S_SPK_DIN  3

class DoorAudio {
public:
    void begin();
    static void handlePlayTTS(WebServer& server);
    static void handleStartMic(WebServer& server); // Старт непрерывной записи
    static void handleStopMic(WebServer& server);  // Остановка записи

    static volatile bool isRecording; // Флаг активности записи

private:
    void initI2SMic();
    void initI2SSpeaker();
    static void logInfo(const char* msg);
    static void logError(const char* msg);
};

extern DoorAudio doorAudio;

#endif