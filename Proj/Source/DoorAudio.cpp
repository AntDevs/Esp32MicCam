#include "DoorAudio.h"
#include "esp_http_server.h"

DoorAudio doorAudio;
volatile bool DoorAudio::isRecording = false;

void DoorAudio::logInfo(const char* msg) {
    Serial.printf("[INFO] DoorAudio | %s\n", msg);
}

void DoorAudio::logError(const char* msg) {
    Serial.printf("[ERROR] DoorAudio | %s\n", msg);
}

void DoorAudio::initI2SMic() {
    Serial.println("[ENTER] initI2SMic | params: none");
    
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
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
        .bck_io_num = 42,               // PDM CLK
        .ws_io_num = I2S_PIN_NO_CHANGE, 
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = 41               // PDM DATA
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_driver_install failed with code %d\n", err);
        return;
    }
    
    i2s_set_pin(I2S_NUM_0, &pin_config);

    // КРИТИЧЕСКИ ВАЖНО: принудительный запуск тактового генератора PDM шины
    err = i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_set_clk failed with code %d\n", err);
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.println("[EXIT] initI2SMic | return: success (PDM mode with explicit clock initialization)");
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


// Эндпоинт запуска непрерывной потоковой записи: /mic/start
void DoorAudio::handleStartMic(WebServer& server) {
    Serial.println("[ENTER] handleStartMic | params: WebServer");

    if (isRecording) {
        server.send(400, "text/plain", "Recording already in progress");
        Serial.println("[EXIT ERROR] handleStartMic | error: Already recording");
        return;
    }

    isRecording = true;
    logInfo("Старт непрерывной потоковой записи с микрофона...");

    // Прямая запись HTTP-заголовков для чанк-потока через сокет клиента
    WiFiClient client = server.client();
    client.println("HTTP/1.1 200 OK");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Content-Type: application/octet-stream");
    client.println("Transfer-Encoding: chunked");
    client.println("Connection: keep-alive");
    client.println();
    client.flush();

    const size_t bufferSize = 1024;
    uint8_t readBuffer[bufferSize];
    size_t bytesRead = 0;

    unsigned long startTime = millis();
    unsigned long maxDurationMs = 10000; // Защитный лимит: максимум 10 секунд

    // Цикл непрерывной передачи данных до сигнала остановки или таймаута
    while (isRecording && client.connected() && (millis() - startTime < maxDurationMs)) {
        esp_err_t err = i2s_read(I2S_MIC_PORT, readBuffer, bufferSize, &bytesRead, pdMS_TO_TICKS(100));
        if (err == ESP_OK && bytesRead > 0) {
            // Форматируем чанк по стандартам HTTP Chunked Transfer Encoding: [HEX_SIZE]\r\n[DATA]\r\n
            char chunkHeader[16];
            int headerLen = snprintf(chunkHeader, sizeof(chunkHeader), "%x\r\n", bytesRead);
            
            client.write((uint8_t*)chunkHeader, headerLen);
            client.write(readBuffer, bytesRead);
            client.write((uint8_t*)"\r\n", 2);
            client.flush();
        }
        yield();
    }

    // Отправляем финальный нулевой чанк для завершения потока в браузере
    client.write((uint8_t*)"0\r\n\r\n", 5);
    client.flush();

    isRecording = false;
    Serial.println("[EXIT] handleStartMic | return: Recording stopped and stream closed");
}

// Эндпоинт остановки записи: /mic/stop
void DoorAudio::handleStopMic(WebServer& server) {
    Serial.println("[ENTER] handleStopMic | params: WebServer");

    server.sendHeader("Access-Control-Allow-Origin", "*");
    
    if (!isRecording) {
        server.send(200, "text/plain", "Not recording");
        Serial.println("[EXIT] handleStopMic | return: No active recording");
        return;
    }

    isRecording = false; // Сбрасываем флаг, что прервет цикл в handleStartMic
    logInfo("Получен сигнал остановки записи с микрофона.");
    
    server.send(200, "text/plain", "Recording stopped successfully");
    Serial.println("[EXIT] handleStopMic | return: Stop signal processed");
}

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