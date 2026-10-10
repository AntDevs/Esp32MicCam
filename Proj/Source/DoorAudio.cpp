#include "DoorAudio.h"

DoorAudio doorAudio;

// Простейший логгер, соответствующий стандартам проекта Intelligence Door
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
        .sample_rate = 16000, // Требование Whisper-small
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
        .sample_rate = 24000, // Частота Silero TTS
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

void DoorAudio::recordAndSendToSTT(const char* serverUrl, int durationSec) {
    Serial.printf("[ENTER] recordAndSendToSTT | params: serverUrl=%s, durationSec=%d\n", serverUrl, durationSec);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[EXIT ERROR] recordAndSendToSTT | error: WiFi not connected");
        return;
    }

    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/octet-stream"); // Отправляем сырые байты

    int bytesToRead = 16000 * 2 * durationSec; // 16kHz * 16-bit(2 bytes) * seconds
    uint8_t* audioBuffer = (uint8_t*)malloc(bytesToRead);
    
    if (!audioBuffer) {
        Serial.println("[EXIT ERROR] recordAndSendToSTT | error: Failed to allocate memory");
        return;
    }

    size_t bytesRead = 0;
    logInfo("Начало записи...");
    
    // Чтение данных с I2S микрофона
    esp_err_t err = i2s_read(I2S_MIC_PORT, audioBuffer, bytesToRead, &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] recordAndSendToSTT | error: i2s_read failed %d\n", err);
        free(audioBuffer);
        return;
    }

    logInfo("Запись завершена, отправка на STT...");

    int httpResponseCode = http.POST(audioBuffer, bytesRead);

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("[EXIT] recordAndSendToSTT | return: HTTP %d, %s\n", httpResponseCode, response.c_str());
    } else {
        Serial.printf("[EXIT ERROR] recordAndSendToSTT | error: HTTP POST failed, code: %d\n", httpResponseCode);
    }

    free(audioBuffer);
    http.end();
}

// Статический метод-обработчик для WebServer (эндпоинт /play)
void DoorAudio::handlePlayTTS(WebServer& server) {
    Serial.println("[ENTER] handlePlayTTS | params: WebServer");

    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Bad Request: No audio body");
        Serial.println("[EXIT ERROR] handlePlayTTS | error: Empty payload");
        return;
    }

    String audioData = server.arg("plain");
    const uint8_t* payload = (const uint8_t*)audioData.c_str();
    size_t payloadLength = audioData.length();

    // Пропускаем WAV-заголовок (первые 44 байта) для воспроизведения сырого PCM
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