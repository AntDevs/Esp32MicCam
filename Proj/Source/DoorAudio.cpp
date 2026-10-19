#include "DoorAudio.h"
#include "esp_http_server.h"

DoorAudio doorAudio;
volatile bool DoorAudio::isRecording = false;

WiFiClient micClient;
TaskHandle_t micTaskHandle = NULL;

static File fsUploadFile; // Глобальный объект для сохранения файлов на диск

void audioStreamTask(void *pvParameters) {
    const size_t bufferSize = 1024;
    uint8_t readBuffer[bufferSize];
    size_t bytesRead = 0;
    unsigned long startTime = millis();
    unsigned long maxDurationMs = 10000;

    while (DoorAudio::isRecording && micClient.connected() && (millis() - startTime < maxDurationMs)) {
        esp_err_t err = i2s_read(I2S_NUM_0, readBuffer, bufferSize, &bytesRead, pdMS_TO_TICKS(100));
        
        if (err == ESP_OK && bytesRead > 0) {
            char chunkHeader[16];
            int headerLen = snprintf(chunkHeader, sizeof(chunkHeader), "%x\r\n", bytesRead);
            micClient.write((uint8_t*)chunkHeader, headerLen);
            micClient.write(readBuffer, bytesRead);
            micClient.write((uint8_t*)"\r\n", 2);
            micClient.flush();
        }
        vTaskDelay(pdMS_TO_TICKS(2)); 
    }

    if (micClient.connected()) {
        micClient.write((uint8_t*)"0\r\n\r\n", 5);
        micClient.flush();
        micClient.stop();
    }
    DoorAudio::isRecording = false;
    vTaskDelete(NULL);
}

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
        .bck_io_num = 42,
        .ws_io_num = I2S_PIN_NO_CHANGE, 
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = 41
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_driver_install failed with code %d\n", err);
        return;
    }
    
    i2s_set_pin(I2S_NUM_0, &pin_config);
    err = i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_set_clk failed with code %d\n", err);
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
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

void DoorAudio::handleStartMic(WebServer& server) {
    Serial.println("[ENTER] handleStartMic | params: WebServer");
    if (isRecording) {
        server.send(400, "text/plain", "Recording already in progress");
        return;
    }
    isRecording = true;
    micClient = server.client(); 

    micClient.println("HTTP/1.1 200 OK");
    micClient.println("Access-Control-Allow-Origin: *");
    micClient.println("Content-Type: application/octet-stream");
    micClient.println("Transfer-Encoding: chunked");
    micClient.println("Connection: keep-alive");
    micClient.println();
    micClient.flush();

    xTaskCreatePinnedToCore(audioStreamTask, "AudioStream", 8192, NULL, 1, &micTaskHandle, 1);
}

void DoorAudio::handleStopMic(WebServer& server) {
    Serial.println("[ENTER] handleStopMic | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!isRecording) {
        server.send(200, "text/plain", "Not recording");
        Serial.println("[EXIT] handleStopMic | return: No active recording");
        return;
    }
    isRecording = false; 
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

// =========================================================================
// 1. ЛОГИКА ПРОИГРЫВАНИЯ АУДИОПОТОКА (НА ЛЕТУ НА I2S)
// =========================================================================
void DoorAudio::handlePlayStreamReq(WebServer& server) {
    Serial.println("[ENTER] handlePlayStreamReq | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "Stream successfully played on speaker");
    Serial.println("[EXIT] handlePlayStreamReq | return: OK");
}

void DoorAudio::handlePlayStreamUpload(WebServer& server) {
//    Serial.println("[ENTER] handlePlayStreamUpload");
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ENTER] handlePlayStreamUpload | params: UPLOAD_FILE_START, filename=%s\n", upload.filename.c_str());
        i2s_zero_dma_buffer(I2S_SPK_PORT);
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) {
        size_t bytesWritten = 0;
        uint8_t* data = upload.buf;
        size_t len = upload.currentSize;
        
        // Отсекаем 44-байтный заголовок WAV в самом начале
        if (upload.totalSize == 0 && len > 44) {
            data += 44;
            len -= 44;
        }
        
        esp_err_t err = i2s_write(I2S_SPK_PORT, data, len, &bytesWritten, portMAX_DELAY);
        if (err != ESP_OK) {
            Serial.printf("[EXIT ERROR] handlePlayStreamUpload | error: i2s_write failed %d\n", err);
        }
        // Избегаем блокировки потока WebServer'a
        delay(1);        
        Serial.printf("[EXIT] handlePlayStreamUpload | i2s_write: len=%u, bytesWritten=%u \n", len, bytesWritten);
    } 
    else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("[EXIT] handlePlayStreamUpload | return: UPLOAD_FILE_END, totalSize=%u\n", upload.totalSize);
    }

//    Serial.println("[EXIT] handlePlayStreamUpload");
}

// =========================================================================
// 2. ЛОГИКА СОХРАНЕНИЯ ФАЙЛОВ НА ДИСК (LittleFS)
// =========================================================================
void DoorAudio::handleFileUploadReq(WebServer& server) {
    Serial.println("[ENTER] handleFileUploadReq | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "File successfully saved to ESP32 Flash");
    Serial.println("[EXIT] handleFileUploadReq | return: OK");
}

void DoorAudio::handleFileUploadData(WebServer& server) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ENTER] handleFileUploadData | params: UPLOAD_FILE_START, filename=%s\n", upload.filename.c_str());
        
        String path = "/" + upload.filename;
        fsUploadFile = LittleFS.open(path, "w");
        if (!fsUploadFile) {
            Serial.printf("[EXIT ERROR] handleFileUploadData | error: Failed to open %s for writing\n", path.c_str());
        }
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
        // Избегаем Watchdog таймаута при записи длинных файлов
        delay(1);
    } 
    else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            fsUploadFile.close();
        }
        Serial.printf("[EXIT] handleFileUploadData | return: UPLOAD_FILE_END, totalSize=%u\n", upload.totalSize);
    }
}