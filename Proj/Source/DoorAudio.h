#ifndef DOOR_AUDIO_H
#define DOOR_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include "esp_camera.h"

// Встроенный PDM микрофон XIAO ESP32S3 Sense (PDM RX поддерживается только на I2S0)
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_CLK  42 // PDM CLK (в legacy-драйвере выводится через WS)
#define I2S_MIC_DATA 41 // PDM DATA

#define I2S_SPK_PORT I2S_NUM_1
#define I2S_SPK_BCLK 1  // BCLK orang Аппаратный GPIO1 (Пин D0)
#define I2S_SPK_LRC  4  // LRCK yellow WS Аппаратный GPIO4 (Пин D3)
#define I2S_SPK_DIN  5  // DIN / SDATA broun Аппаратный GPIO5 (Пин D4)

#define AUDIO_SAMPLE_RATE 16000

// Маленькие DMA буферы: 4 x 512 кадров x 2 байта = 4 КБ на порт (~128 мс при 16 кГц)
#define AUDIO_DMA_BUF_COUNT 4
#define AUDIO_DMA_BUF_LEN   512


class DoorAudio {
public:
    // Устанавливает оба I2S драйвера один раз. Вызывать в setup() ДО WiFi.begin()
    void start();
    void playBeep();
    static void handleStartMic(WebServer& server);
    static void handleStopMic(WebServer& server);

    // Новые методы стриминга и загрузки
    static void handlePlayStreamReq(WebServer& server);
    static void handlePlayStreamUpload(WebServer& server);

    static void handleFileUploadReq(WebServer& server);
    static void handleFileUploadData(WebServer& server);
    static void handleListFiles(WebServer& server);
    static void handlePlayFile(WebServer& server);
    static void handleDeleteFile(WebServer& server);

    static volatile bool isRecording;
    static bool micReady;
    static bool speakerReady;
private:
    void initI2SMic();
    void initI2SSpeaker();
    static void logDmaHeap(const char* where);

};

extern DoorAudio doorAudio;
#endif
