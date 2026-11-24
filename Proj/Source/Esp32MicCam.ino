#include <Arduino.h>
#include <LittleFS.h>
#include "DoorAudio.h"
#include "DoorVideo.h"
#include "DoorServer.h"
#include "DoorRadar.h"

void setup() {
    Serial.begin(115200);
    // USB CDC: если плата подключена к ПК, но монитор порта закрыт, каждый print ждал бы
    // освобождения буфера до 100 мс и тормозил WebServer. Без читателя логи просто теряются
    Serial.setTxTimeoutMs(0);
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

// Инициализация радара (по умолчанию настроен на GPIO2 / Пин D1)
    doorRadar.begin();

    Serial.println("[EXIT] setup | return: void");
}

void loop() {
    doorServer.handleClient();

// Проверяем, изменилось ли состояние радара с прошлого цикла
    if (doorRadar.hasStateChanged()) {
        
        if (doorRadar.isPresenceDetected()) {
            Serial.println("[INFO] Радар: Человек обнаружен! Пробуждаем камеру.");
            doorVideo.start();
        } else {
            Serial.println("[INFO] Радар: Никого нет. Камера переведена в спящий режим.");
            doorVideo.stop();
        }
    }

    delay(2);
}
