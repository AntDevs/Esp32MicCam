#include "DoorAudio.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
// #include "pcm.h"

DoorAudio doorAudio;
volatile bool DoorAudio::isRecording = false;
volatile bool DoorAudio::isSaving = false;
bool DoorAudio::micReady = false;
bool DoorAudio::speakerReady = false;

WiFiClient micClient;
TaskHandle_t micTaskHandle = NULL;

// Новый драйвер I2S (legacy driver/i2s.h в core 3.x не отдавал данные PDM микрофона)
static i2s_chan_handle_t micChan = NULL;
static i2s_chan_handle_t spkChan = NULL;
static bool speakerRunning = false;

static File fsUploadFile; // Глобальный объект для сохранения файлов на диск

// Время, за которое DMA проигрывает весь буфер динамика, плюс запас
static const uint32_t SPK_DRAIN_MS = (AUDIO_DMA_BUF_COUNT * AUDIO_DMA_BUF_LEN * 1000UL) / AUDIO_SAMPLE_RATE + 20;

static void speakerStart() {
    if (!DoorAudio::speakerReady || speakerRunning) return;
    // auto_clear: DMA буферы обнуляются после отправки, старый хвост не прозвучит
    i2s_channel_enable(spkChan);
    speakerRunning = true;
}

static esp_err_t speakerWrite(const void *data, size_t len, size_t *written) {
    return i2s_channel_write(spkChan, data, len, written, portMAX_DELAY);
}

// Дожидаемся, пока DMA доиграет хвост, и останавливаем тактирование I2S
static void speakerStop(bool drain) {
    if (!speakerRunning) return;
    if (drain) delay(SPK_DRAIN_MS);
    i2s_channel_disable(spkChan);
    speakerRunning = false;
}

static void writeWavHeader(File &f, uint32_t dataLen) {
    uint8_t h[44];
    auto u32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
    auto u16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
    memcpy(h, "RIFF", 4); u32(4, 36 + dataLen); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); u32(16, 16); u16(20, 1); u16(22, 1);
    u32(24, AUDIO_SAMPLE_RATE); u32(28, AUDIO_SAMPLE_RATE * 2); u16(32, 2); u16(34, 16);
    memcpy(h + 36, "data", 4); u32(40, dataLen);
    f.write(h, sizeof(h));
}

// Пишем во временный файл и переименовываем, чтобы при сбое не остался обрезанный mic.wav
static bool saveMicFile(const uint8_t *data, size_t len) {
    const char *tmp = MIC_FILE_PATH ".tmp";
    unsigned long t0 = millis();
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    writeWavHeader(f, len);
    size_t written = 0;
    while (written < len) {
        size_t n = f.write(data + written, min((size_t)4096, len - written));
        if (n == 0) break;
        written += n;
        vTaskDelay(1); // Отдаем ядро loop(): WebServer отвечает на /api/status во время записи
    }
    unsigned long tWrite = millis();
    f.close();
    if (written != len) {
        LittleFS.remove(tmp);
        return false;
    }
    unsigned long tClose = millis();
    LittleFS.remove(MIC_FILE_PATH);
    bool ok = LittleFS.rename(tmp, MIC_FILE_PATH);
    Serial.printf("[TRACE] saveMicFile | %u bytes: write %lu ms, close %lu ms, rename %lu ms\n",
                  len, tWrite - t0, tClose - tWrite, millis() - tClose);
    return ok;
}

void audioStreamTask(void *pvParameters) {
    Serial.println("[ENTER] audioStreamTask | params: pvParameters");
    const size_t bufferSize = 1024;
    uint8_t readBuffer[bufferSize];
    size_t bytesRead = 0;
    unsigned long startTime = millis();
    unsigned long maxDurationMs = (unsigned long)(uintptr_t)pvParameters;
    // Диагностика: отличить "нет данных от I2S" от "данные есть, но нули"
    uint32_t totalBytes = 0, emptyReads = 0;
    int32_t peak = 0;
    // Первые ~100 мс после включения PDM фильтр выдает щелчок, их не отдаем
    uint32_t skipBytes = AUDIO_SAMPLE_RATE * 2 / 10;
    // ФВЧ ~20 Гц: у PDM фильтра постоянное смещение (~-1400) и медленный дрейф,
    // которые без фильтра дают RMS в сотни при тишине
    const float hpA = 0.992f;
    float hpX = 0, hpY = 0;
    bool hpInit = false;

    // Копия первых MIC_FILE_MAX_SEC секунд в PSRAM, на флеш пишем после записи,
    // чтобы стирание секторов LittleFS не приводило к переполнению DMA микрофона
    const size_t fileCap = MIC_FILE_MAX_SEC * AUDIO_SAMPLE_RATE * 2;
    uint8_t *fileBuf = (uint8_t *)heap_caps_malloc(fileCap, MALLOC_CAP_SPIRAM);
    size_t fileLen = 0;
    if (!fileBuf) Serial.println("[WARN] audioStreamTask | no PSRAM buffer, " MIC_FILE_PATH " will not be saved");

    while (DoorAudio::isRecording && micClient.connected() && (millis() - startTime < maxDurationMs)) {
        esp_err_t err = i2s_channel_read(micChan, readBuffer, bufferSize, &bytesRead, pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytesRead == 0) {
            emptyReads++;
            continue;
        }
        int16_t *pcm = (int16_t *)readBuffer;
        if (!hpInit) { hpX = pcm[0]; hpInit = true; }
        for (size_t i = 0; i < bytesRead / 2; i++) {
            float x = pcm[i];
            hpY = hpA * (hpY + x - hpX);
            hpX = x;
            pcm[i] = (int16_t)constrain(lrintf(hpY), -32768, 32767);
        }
        if (skipBytes) {
            size_t n = min((size_t)skipBytes, bytesRead);
            skipBytes -= n;
            if (n == bytesRead) continue;
            memmove(readBuffer, readBuffer + n, bytesRead - n);
            bytesRead -= n;
        }
        const int16_t *samples = (const int16_t *)readBuffer;
        for (size_t i = 0; i < bytesRead / 2; i++) {
            int32_t v = abs((int32_t)samples[i]); // int16 не вмещает |-32768|
            if (v > peak) peak = v;
        }
        totalBytes += bytesRead;
        if (fileBuf && fileLen < fileCap) {
            size_t n = min(bytesRead, fileCap - fileLen);
            memcpy(fileBuf + fileLen, readBuffer, n);
            fileLen += n;
        }
        char chunkHeader[16];
        int headerLen = snprintf(chunkHeader, sizeof(chunkHeader), "%x\r\n", bytesRead);
        micClient.write((uint8_t*)chunkHeader, headerLen);
        micClient.write(readBuffer, bytesRead);
        micClient.write((uint8_t*)"\r\n", 2);
    }
    // Микрофон засыпает, DMA буферы остаются выделенными до следующего /mic/start
    i2s_channel_disable(micChan);

    if (micClient.connected()) {
        micClient.write((uint8_t*)"0\r\n\r\n", 5);
        micClient.stop();
    }
    // Файл пишем уже после закрытия потока: 10 с (320 КБ) пишутся во флеш ~4 с.
    // Панель ждет, пока /api/status не покажет mic.saving = false
    bool saved = false;
    DoorAudio::isSaving = fileBuf && fileLen > 0;
    DoorAudio::isRecording = false;
    if (fileBuf) {
        // Приоритет как у loopTask, иначе WebServer на том же ядре стоит все ~4 с записи
        vTaskPrioritySet(NULL, 1);
        if (fileLen > 0) saved = saveMicFile(fileBuf, fileLen);
        heap_caps_free(fileBuf);
    }
    DoorAudio::isSaving = false;
    micTaskHandle = NULL;
    Serial.printf("[EXIT] audioStreamTask | return: sent %u bytes, empty reads %u, peak %ld, %s %u bytes: %s\n",
                  totalBytes, emptyReads, (long)peak, MIC_FILE_PATH, fileLen, saved ? "saved" : "NOT saved");
    vTaskDelete(NULL);
}

void DoorAudio::logDmaHeap(const char* where) {
    Serial.printf("[TRACE] %s | free DMA: %u, largest DMA block: %u\n", where,
                  heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

void DoorAudio::initI2SMic() {
    Serial.printf("[ENTER] initI2SMic | I2S_MIC_PORT: %d \n" , I2S_MIC_PORT);
    logDmaHeap("initI2SMic");

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chanCfg.dma_frame_num = AUDIO_DMA_BUF_LEN;
    esp_err_t err = i2s_new_channel(&chanCfg, NULL, &micChan);
    if (err != ESP_OK) {
        // 0x101 ESP_ERR_NO_MEM - нет DMA памяти, 0x105 ESP_ERR_NOT_FOUND - порт или прерывание заняты
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_new_channel failed with code 0x%x\n", err);
        return;
    }

    i2s_pdm_rx_config_t pdmCfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)I2S_MIC_CLK,
            .din = (gpio_num_t)I2S_MIC_DATA,
            .invert_flags = { .clk_inv = false },
        },
    };
    err = i2s_channel_init_pdm_rx_mode(micChan, &pdmCfg);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SMic | error: i2s_channel_init_pdm_rx_mode failed with code 0x%x\n", err);
        return;
    }

    // Канал остается выключенным (READY) до /mic/start
    micReady = true;
    Serial.println("[EXIT] initI2SMic | return: success");
}

void DoorAudio::initI2SSpeaker() {
    Serial.printf("[ENTER] initI2SSpeaker | I2S_SPK_PORT: %d \n" , I2S_SPK_PORT);
    logDmaHeap("initI2SSpeaker");

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_PORT, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chanCfg.dma_frame_num = AUDIO_DMA_BUF_LEN;
    chanCfg.auto_clear = true; // При нехватке данных уходят нули, а не повтор старого буфера
    esp_err_t err = i2s_new_channel(&chanCfg, &spkChan, NULL);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SSpeaker | error: i2s_new_channel failed with code 0x%x\n", err);
        return;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // Иначе MCLK может оказаться на GPIO0 (кнопка BOOT)
            .bclk = (gpio_num_t)I2S_SPK_BCLK,
            .ws = (gpio_num_t)I2S_SPK_LRC,
            .dout = (gpio_num_t)I2S_SPK_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // Моно в оба слота: усилитель играет при любом выборе канала L/R на его входе
    stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    err = i2s_channel_init_std_mode(spkChan, &stdCfg);
    if (err != ESP_OK) {
        Serial.printf("[EXIT ERROR] initI2SSpeaker | error: i2s_channel_init_std_mode failed with code 0x%x\n", err);
        return;
    }

    // Канал выключен до начала воспроизведения
    speakerReady = true;
    Serial.println("[EXIT] initI2SSpeaker | return: success");
}

// =========================================================================
// ДИАГНОСТИЧЕСКИЙ СИГНАЛ (БИП) ДЛЯ ПРОВЕРКИ ЦАП
// =========================================================================
void DoorAudio::playBeep() {
    Serial.println("[ENTER] playBeep | Генерация тестового сигнала (бип 1000 Гц)");
    if (!speakerReady) {
        Serial.println("[EXIT ERROR] playBeep | error: speaker not initialized");
        return;
    }
    speakerStart();

    // При 16000 Гц тон 1000 Гц = 16 сэмплов на период (8 вверх, 8 вниз).
    // Проигрываем 250 мс -> 16000 * 0.25 = 4000 сэмплов, порциями по 256.
    int16_t chunk[256];
    size_t bytes_written;
    for (int sent = 0; sent < 4000; sent += 256) {
        for (int i = 0; i < 256; i++) {
            chunk[i] = (((sent + i) % 16) < 8) ? 8000 : -8000;
        }
        esp_err_t err = speakerWrite(chunk, sizeof(chunk), &bytes_written);
        if (err != ESP_OK) {
            Serial.printf("[ERROR] playBeep | error: i2s_write failed with code 0x%x\n", err);
            break;
        }
    }

    speakerStop(true);
    Serial.println("[EXIT] playBeep | return: success");
}


// void DoorAudio::sendSound(){
//     Serial.printf("[ENTER] DoorAudio | sendSound: %d \n" , I2S_SPK_PORT);
//     //   CLOGI << "Writing PCM data, size: " << sizeof(_MERRY_CHRISTMAS_PCM) << " bytes";
//     delay(100);

//     size_t bytes_written = 0;
//     size_t total_bytes = sizeof(_MERRY_CHRISTMAS_PCM);
//     size_t bytes_to_write = total_bytes;
//     const uint8_t* pcm_data = (const uint8_t*)_MERRY_CHRISTMAS_PCM;
    
//     while (bytes_to_write > 0) {
//         size_t chunk_size = (bytes_to_write > 1024) ? 1024 : bytes_to_write;
//         size_t chunk_written = 0;
        
//         esp_err_t err = i2s_write(I2S_SPK_PORT, pcm_data + (total_bytes - bytes_to_write), 
//                         chunk_size, &chunk_written, pdMS_TO_TICKS(1000));
        
//         if (err != ESP_OK) {
//         //   CLOGE << "i2s_write failed at offset " << (total_bytes - bytes_to_write)             << ": " << err;
//         break;
//         }
        
//         bytes_written += chunk_written;
//         bytes_to_write -= chunk_written;
        
//         if (chunk_written == 0) {
//         //   CLOGW << "No bytes written, possible buffer full";
//         delay(10);
//         }
//     }
    
//     //   CLOGI << "Total bytes written: " << bytes_written << "/" << total_bytes;
//     delay(100);
//     //   CLOGI << "setup OK - Audio should be playing now";
//     Serial.printf("[EXIT] DoorAudio | sendSound: %d \n" , I2S_SPK_PORT);


void DoorAudio::start() {
    Serial.println("[ENTER] DoorAudio::begin | params: none");
    // Оба драйвера ставятся один раз, пока внутренняя DMA память не раздроблена Wi-Fi.
    // Дальше только i2s_start/i2s_stop, без install/uninstall.
    initI2SMic();
    initI2SSpeaker();
    logDmaHeap("DoorAudio::begin");
    Serial.println("[EXIT] DoorAudio::begin | return: initialized");
}

void DoorAudio::handleStartMic(WebServer& server) {
    Serial.println("[ENTER] handleStartMic | params: WebServer");
    if (!micReady) {
        server.send(500, "text/plain", "Microphone not initialized");
        Serial.println("[EXIT ERROR] handleStartMic | error: Microphone not initialized");
        return;
    }
    // micTaskHandle сбрасывается только после сохранения mic.wav, поэтому проверяем и его
    if (isRecording || micTaskHandle != NULL) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(409, "text/plain", isSaving ? "Busy: saving " MIC_FILE_PATH : "Recording already in progress");
        Serial.println("[EXIT ERROR] handleStartMic | error: Recording in progress");
        return;
    }
    // Длительность записи: /mic/start?sec=N (1..60, по умолчанию 10)
    long sec = server.hasArg("sec") ? server.arg("sec").toInt() : 10;
    if (sec < 1) sec = 1;
    if (sec > 60) sec = 60;

    isRecording = true;
    micClient = server.client();

    micClient.println("HTTP/1.1 200 OK");
    micClient.println("Access-Control-Allow-Origin: *");
    micClient.println("Content-Type: application/octet-stream");
    micClient.println("Transfer-Encoding: chunked");
    micClient.println("Connection: close");
    micClient.println();
    micClient.flush();

    i2s_channel_enable(micChan);
    // Звук важнее видео: приоритет выше, чем у httpd потока камеры (5), который живет на Core 0
    xTaskCreatePinnedToCore(audioStreamTask, "AudioStream", 8192, (void *)(uintptr_t)(sec * 1000), 6, &micTaskHandle, 1);
    Serial.println("[EXIT] handleStartMic | return: Task started");
}

void DoorAudio::handleStopMic(WebServer& server) {
    Serial.println("[ENTER] handleStopMic | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!isRecording) {
        server.send(200, "text/plain", "Not recording");
        Serial.println("[EXIT ERROR] handleStopMic | error: No active recording");
        return;
    }
    isRecording = false;
    Serial.println("[INFO] DoorAudio | Получен сигнал остановки записи с микрофона.");
    server.send(200, "text/plain", "Recording stopped successfully");
    Serial.println("[EXIT] handleStopMic | return: Stop signal processed");
}

// =========================================================================
// 1. ЛОГИКА ПРОИГРЫВАНИЯ АУДИОПОТОКА (НА ЛЕТУ НА I2S)
// =========================================================================
void DoorAudio::handlePlayStreamReq(WebServer& server) {
    Serial.println("[ENTER] handlePlayStreamReq | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!speakerReady) {
        server.send(500, "text/plain", "Speaker not initialized");
        Serial.println("[EXIT ERROR] handlePlayStreamReq | error: Speaker not initialized");
        return;
    }
    server.send(200, "text/plain", "Stream successfully played on speaker");
    Serial.println("[EXIT] handlePlayStreamReq | return: OK");
}

void DoorAudio::handlePlayStreamUpload(WebServer& server) {
    HTTPUpload& upload = server.upload();
    if (!speakerReady) return;

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ENTER] handlePlayStreamUpload | params: UPLOAD_FILE_START, filename=%s\n", upload.filename.c_str());
        speakerStart();
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

        esp_err_t err = speakerWrite(data, len, &bytesWritten);
        if (err != ESP_OK) {
            Serial.printf("[EXIT ERROR] handlePlayStreamUpload | error: i2s_write failed %d\n", err);
        }
        // Избегаем блокировки потока WebServer'a
        delay(1);
        Serial.printf("[EXIT] handlePlayStreamUpload | i2s_write: len=%u, bytesWritten=%u \n", len, bytesWritten);
    }
    else if (upload.status == UPLOAD_FILE_END) {
        speakerStop(true);
        Serial.printf("[EXIT] handlePlayStreamUpload | return: UPLOAD_FILE_END, totalSize=%u\n", upload.totalSize);
    }
    else if (upload.status == UPLOAD_FILE_ABORTED) {
        speakerStop(false);
        Serial.println("[EXIT ERROR] handlePlayStreamUpload | error: UPLOAD_FILE_ABORTED");
    }
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
        Serial.printf("[EXIT] handleFileUploadData | write_chunk: len=%u \n", upload.currentSize);
    } 
    else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            fsUploadFile.close();
        }
        Serial.printf("[EXIT] handleFileUploadData | return: UPLOAD_FILE_END, totalSize=%u\n", upload.totalSize);
    }
}

// =========================================================================
// 3. УПРАВЛЕНИЕ ФАЙЛАМИ В LittleFS
// =========================================================================

// Имя файла из ?name= без путей, чтобы не выйти за корень FS
static bool getSafeFileName(WebServer& server, String& path) {
    if (!server.hasArg("name")) return false;
    String name = server.arg("name");
    if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) return false;
    path = "/" + name;
    return true;
}

void DoorAudio::handleListFiles(WebServer& server) {
    Serial.println("[ENTER] handleListFiles | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String json = "{\"total\":";
    json += LittleFS.totalBytes();
    json += ",\"used\":";
    json += LittleFS.usedBytes();
    json += ",\"files\":[";
    File root = LittleFS.open("/");
    bool first = true;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory()) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"";
            json += f.name();
            json += "\",\"size\":";
            json += f.size();
            json += "}";
        }
        f.close();
    }
    root.close();
    json += "]}";
    server.send(200, "application/json", json);
    Serial.println("[EXIT] handleListFiles | return: OK");
}

// Проигрывает WAV из LittleFS: GET /api/play_file?name=door.wav
// Ожидается PCM 16 бит, моно, 16 кГц (панель конвертирует файлы перед загрузкой)
void DoorAudio::handlePlayFile(WebServer& server) {
    Serial.println("[ENTER] handlePlayFile | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String path;
    if (!getSafeFileName(server, path)) {
        server.send(400, "text/plain", "Bad Request: invalid name");
        return;
    }
    if (!speakerReady) {
        server.send(500, "text/plain", "Speaker not initialized");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(404, "text/plain", "File not found: " + path);
        return;
    }

    // Обходим чанки RIFF до "data", попутно проверяя формат в "fmt "
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        f.close();
        server.send(415, "text/plain", "Not a WAV file");
        return;
    }
    uint32_t dataSize = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    while (true) {
        uint8_t ch[8];
        if (f.read(ch, 8) != 8) break;
        uint32_t size = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16 || f.read(fmt, 16) != 16) break;
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            f.seek(f.position() + size - 16 + (size & 1));
        } else if (memcmp(ch, "data", 4) == 0) {
            dataSize = size;
            break;
        } else {
            f.seek(f.position() + size + (size & 1));
        }
    }
    if (dataSize == 0 || channels != 1 || bits != 16 || rate != AUDIO_SAMPLE_RATE) {
        f.close();
        server.send(415, "text/plain", "Unsupported WAV: need PCM 16-bit mono 16000 Hz, got " +
                    String(channels) + "ch " + String(bits) + "bit " + String(rate) + "Hz");
        return;
    }

    speakerStart();
    uint8_t buf[1024];
    uint32_t left = dataSize;
    while (left > 0) {
        size_t n = f.read(buf, left > sizeof(buf) ? sizeof(buf) : left);
        if (n == 0) break;
        size_t written = 0;
        speakerWrite(buf, n, &written);
        left -= n;
    }
    speakerStop(true);
    f.close();

    server.send(200, "text/plain", "Played " + path);
    Serial.printf("[EXIT] handlePlayFile | return: played %u bytes\n", dataSize - left);
}

void DoorAudio::handleDeleteFile(WebServer& server) {
    Serial.println("[ENTER] handleDeleteFile | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String path;
    if (!getSafeFileName(server, path)) {
        server.send(400, "text/plain", "Bad Request: invalid name");
        return;
    }
    if (!LittleFS.remove(path)) {
        server.send(404, "text/plain", "Delete failed: " + path);
        return;
    }
    server.send(200, "text/plain", "Deleted " + path);
    Serial.println("[EXIT] handleDeleteFile | return: OK");
}

// Отдает файл из LittleFS: GET /api/download?name=mic.wav (для прослушивания в браузере)
void DoorAudio::handleDownloadFile(WebServer& server) {
    Serial.println("[ENTER] handleDownloadFile | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String path;
    if (!getSafeFileName(server, path)) {
        server.send(400, "text/plain", "Bad Request: invalid name");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(404, "text/plain", "File not found: " + path);
        return;
    }
    String type = path.endsWith(".wav") ? "audio/wav" : path.endsWith(".html") ? "text/html" : "application/octet-stream";
    server.sendHeader("Cache-Control", "no-store"); // mic.wav перезаписывается при каждой записи
    size_t sent = server.streamFile(f, type);
    f.close();
    Serial.printf("[EXIT] handleDownloadFile | return: sent %u bytes\n", sent);
}
