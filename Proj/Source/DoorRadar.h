#ifndef DOOR_RADAR_H
#define DOOR_RADAR_H

#include <Arduino.h>

// OUT - green D1
// TX -yllow - D6
// RX - bloe - D7


class DoorRadar {
private:
    int outPin;
    bool lastPresenceState;
    unsigned long lastChangeTime;

public:
    DoorRadar(int pin);
    void begin();
    
    // Возвращает текущее состояние пина (HIGH = человек обнаружен)
    bool isPresenceDetected();
    
    // Возвращает true, если состояние изменилось с прошлого вызова
    bool hasStateChanged();
    
    // Возвращает последнее сохраненное состояние
    bool getLastState();
};

extern DoorRadar doorRadar;

#endif // DOOR_RADAR_H
