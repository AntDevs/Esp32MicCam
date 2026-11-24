#include "DoorRadar.h"

// По умолчанию используем пин D1 (GPIO2 на XIAO ESP32S3) для чтения сигнала OUT радара
DoorRadar doorRadar(2);

DoorRadar::DoorRadar(int pin) {
    outPin = pin;
    lastPresenceState = false;
    lastChangeTime = 0;
}

void DoorRadar::begin() {
    Serial.println("[ENTER] DoorRadar::begin");
    
    // Радар выдает 3.3V (HIGH) при обнаружении человека и 0V (LOW) когда никого нет.
    // Используем INPUT_PULLDOWN, чтобы исключить наводки, если провод отпадет.
    pinMode(outPin, INPUT_PULLDOWN); 
    delay(10); // Даем 10мс буферу Serial на отправку рамки веб-сервер
    Serial.printf("[INFO] Radar OUT pin configured on GPIO %d\n", outPin);
    
    // Считываем начальное состояние
    lastPresenceState = isPresenceDetected();
    
    Serial.println("[EXIT] DoorRadar::begin");
}

bool DoorRadar::isPresenceDetected() {
    return digitalRead(outPin) == HIGH;
}

bool DoorRadar::hasStateChanged() {
    bool currentState = isPresenceDetected();
    
    // Простейшая защита от дребезга (debounce), радар сам по себе имеет задержки,
    // но защита на 50мс не помешает
    if (currentState != lastPresenceState && millis() - lastChangeTime > 50) {
        lastPresenceState = currentState;
        lastChangeTime = millis();
        return true;
    }
    return false;
}

bool DoorRadar::getLastState() {
    return lastPresenceState;
}
