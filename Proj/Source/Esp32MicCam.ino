#include <Arduino.h>
#include <LittleFS.h>
#include "DoorAudio.h"
#include "DoorVideo.h"
#include "DoorServer.h"

void setup() {
    Serial.begin(115200);
    Serial.println("\n[ENTER] setup | params: none");

// 1. СТРОГО ПЕРЕД WI-FI И FS: Захватываем DMA память камеры
    doorVideo.start();
    doorVideo.stop(); // Сразу переводим в сон (XCLK на паузе) до вызова /cam/start

// 2. Тоже ДО Wi-Fi: ставим оба I2S драйвера, пока DMA память не раздроблена.
    // Микрофон и динамик сразу засыпают (i2s_stop) до запроса
    doorAudio.start();

    // Инициализация файловой системы для загрузки файлов
    if (!LittleFS.begin(true)) {
        Serial.println("[TRACE] setup | error: LittleFS Mount Failed");
    }

// 3. Wi-Fi, mDNS и все HTTP маршруты
    doorServer.start();

    Serial.println("[EXIT] setup | return: void");
}

void loop() {
    doorServer.handleClient();
    delay(2);
}
