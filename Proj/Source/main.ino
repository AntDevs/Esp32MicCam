#include <WiFi.h>
#include <WebServer.h>
#include "DoorAudio.h"
#include "DoorConfig.h" // Ваш существующий модуль настроек

WebServer server(80);

void setup() {
    Serial.begin(115200);
    
    // 1. Подключение к WiFi
    WiFi.begin("ВАШ_SSID", "ВАШ_ПАРОЛЬ");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[INFO] WiFi connected");

    // 2. Инициализация I2S интерфейсов (микрофон и динамик)
    doorAudio.begin();

    // 3. Настройка эндпоинта /play (Для получения звука от TTS)
    server.on("/play", HTTP_POST, []() {
        DoorAudio::handlePlayTTS(server);
    });
    server.begin();
    
    Serial.println("[INFO] Server started");
}

void loop() {
    server.handleClient(); // Обработка входящих запросов от TTS сервера (на /play)

    // Пример вызова записи голоса при нажатии кнопки или детекции движения (STT)
    // if (digitalRead(BUTTON_PIN) == LOW) {
    //     // Сервер 192.168.x.x должен совпадать с IP, где запущен FastAPI
    //     doorAudio.recordAndSendToSTT("http://192.168.1.100:8000/listen", 3);
    // }
}