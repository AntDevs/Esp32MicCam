#include "DoorAudio.h"
#include "esp_http_server.h" // <--- ЭТОТ ИМПОРТ РЕШАЕТ ОШИБКУ КОМПИЛЯЦИИ

DoorAudio doorAudio;

// Логгер, соответствующий стандартам проекта Intelligence Door
void DoorAudio::logInfo(const char* msg) {
    Serial.printf("[INFO] DoorAudio | %s\n", msg);
}

void DoorAudio::logError(const char* msg) {
    Serial.printf("[ERROR] DoorAudio | %s\n", msg);
}

void DoorAudio::initI2SMic() {
    Serial.println("[ENTER] initI2SMic | params: none");
    
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000, 
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_LRC,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DOUT
    };

    esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_driver_install failed with code %d\n", err);
        return;
    }
    
    i2s_set_pin(I2S_MIC_PORT, &pin_config);
    Serial.println("[EXIT] initI2SMic | return: success");
}

void DoorAudio::initI2SSpeaker() {
    Serial.println("[ENTER] initI2SSpeaker | params: none");

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 24000, 
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_LRC,
        .data_out_num = I2S_SPK_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SSpeaker | error: i2s_driver_install failed with code %d\n", err);
        return;
    }
    
    i2s_set_pin(I2S_SPK_PORT, &pin_config);
    Serial.println("[EXIT] initI2SSpeaker | return: success");
}

void DoorAudio::begin() {
    Serial.println("[ENTER] DoorAudio::begin | params: none");
    initI2SMic();
    initI2SSpeaker();
    Serial.println("[EXIT] DoorAudio::begin | return: initialized");
}

// REST API эндпоинт для прослушивания звука с микрофона с защитой от Watchdog Reset
void DoorAudio::handleGetMicAudio(WebServer& server) {
    Serial.println("[ENTER] handleGetMicAudio | params: WebServer");

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "*");

    int durationSec = 3;
    if (server.hasArg("duration")) {
        durationSec = server.arg("duration").toInt();
        if (durationSec <= 0 || durationSec > 10) durationSec = 3;
    }

    int sampleRate = 16000;
    int bytesToRead = sampleRate * 2 * durationSec;
    uint8_t* audioBuffer = (uint8_t*)malloc(bytesToRead);

    if (!audioBuffer) {
        server.send(500, "text/plain", "Memory allocation failed");
        Serial.println("[EXIT ERROR] handleGetMicAudio | error: Out of memory");
        return;
    }

    size_t bytesRead = 0;
    logInfo("Запись аудио с микрофона по REST запросу...");
    
    TickType_t timeoutTicks = pdMS_TO_TICKS((durationSec * 1000) + 500);
    esp_err_t err = i2s_read(I2S_MIC_PORT, audioBuffer, bytesToRead, &bytesRead, timeoutTicks);
    
    if (err != ESP_OK || bytesRead == 0) {
        free(audioBuffer);
        server.send(500, "text/plain", "I2S read timeout or failed");
        Serial.printf("[EXIT ERROR] handleGetMicAudio | error: i2s_read failed with code %d, read bytes: %u\n", err, bytesRead);
        return;
    }

    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "inline; filename=mic_recording.pcm");
    server.setContentLength(bytesRead);
    server.send(200, "application/octet-stream", "");
    server.client().write(audioBuffer, bytesRead);

    free(audioBuffer);
    Serial.println("[EXIT] handleGetMicAudio | return: Audio stream sent successfully");
}

// Статический метод-обработчик для воспроизведения TTS
void DoorAudio::handlePlayTTS(WebServer& server) {
    Serial.println("[ENTER] handlePlayTTS | params: WebServer");

    server.sendHeader("Access-Control-Allow-Origin", "*");

    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Bad Request: No audio body");
        Serial.println("[EXIT ERROR] handlePlayTTS | error: Empty payload");
        return;
    }

    String audioData = server.arg("plain");
    const uint8_t* payload = (const uint8_t*)audioData.c_str();
    size_t payloadLength = audioData.length();

    int headerOffset = 44; 
    if (payloadLength <= headerOffset) {
         server.send(400, "text/plain", "Bad Request: Payload too short");
         Serial.println("[EXIT ERROR] handlePlayTTS | error: Payload too short");
         return;
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT, payload + headerOffset, payloadLength - headerOffset, &bytesWritten, portMAX_DELAY);

    if (err != ESP_OK) {
        server.send(500, "text/plain", "Internal Server Error: I2S write failed");
        Serial.printf("[EXIT ERROR] handlePlayTTS | error: i2s_write failed %d\n", err);
        return;
    }

    server.send(200, "text/plain", "OK: Audio played");
    Serial.println("[EXIT] handlePlayTTS | return: Playback success");
}