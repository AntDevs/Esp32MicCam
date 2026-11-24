#include "DoorAudio.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include <Preferences.h>
// #include "pcm.h"

DoorAudio doorAudio;
volatile bool DoorAudio::isRecording = false;
volatile bool DoorAudio::isSaving = false;
bool DoorAudio::micReady = false;
bool DoorAudio::speakerReady = false;
int DoorAudio::micGain = MIC_DEFAULT_GAIN;
int DoorAudio::speakerVolume = SPK_DEFAULT_VOLUME;

WiFiClient micClient;
TaskHandle_t micTaskHandle = NULL;

// Новый драйвер I2S (legacy driver/i2s.h в core 3.x не отдавал данные PDM микрофона)
static i2s_chan_handle_t micChan = NULL;
static i2s_chan_handle_t spkChan = NULL;
static bool speakerRunning = false;

static File fsUploadFile; // Глобальный объект для сохранения файлов на диск

// Время, за которое DMA проигрывает весь буфер динамика, плюс запас
static const uint32_t SPK_DRAIN_MS = (AUDIO_DMA_BUF_COUNT * AUDIO_DMA_BUF_LEN * 1000UL) / AUDIO_SAMPLE_RATE + 20;

// Громкость: буфер для масштабированных сэмплов и байт, оставшийся от нечетного чанка
// (HTTP upload может разрезать 16-битный сэмпл между чанками)
static int16_t spkBuf[512];
static uint8_t spkCarry = 0;
static bool spkHasCarry = false;

static void speakerStart() {
    if (!DoorAudio::speakerReady || speakerRunning) return;
    spkHasCarry = false;
    // auto_clear: DMA буферы обнуляются после отправки, старый хвост не прозвучит
    i2s_channel_enable(spkChan);
    speakerRunning = true;
}

// Пишет PCM 16 бит в динамик, умножая на громкость.
// Кривая квадратичная (на слух ближе к линейной): 50% = -12 дБ, 25% = -24 дБ, 10% = -40 дБ.
static esp_err_t speakerWrite(const void *data, size_t len, size_t *written) {
    const int v = DoorAudio::speakerVolume;
    const int32_t q = (int32_t)v * v * 32768 / 10000; // Q15, при 100% = 32768 (без изменений)
    const uint8_t *p = (const uint8_t *)data;
    size_t total = 0;
    esp_err_t err = ESP_OK;
    while (len > 0) {
        size_t n = 0;
        if (spkHasCarry) {
            spkBuf[n++] = (int16_t)(spkCarry | (p[0] << 8));
            spkHasCarry = false;
            p++; len--; total++;
        }
        while (n < 512 && len >= 2) {
            spkBuf[n++] = (int16_t)(p[0] | (p[1] << 8));
            p += 2; len -= 2; total += 2;
        }
        if (len == 1 && n < 512) {
            spkCarry = p[0];
            spkHasCarry = true;
            p++; len--; total++;
        }
        if (q != 32768) {
            for (size_t i = 0; i < n; i++) spkBuf[i] = (int16_t)(((int32_t)spkBuf[i] * q) >> 15);
        }
        if (n) {
            size_t w = 0;
            err = i2s_channel_write(spkChan, spkBuf, n * 2, &w, portMAX_DELAY);
            if (err != ESP_OK) break;
        }
    }
    if (written) *written = total;
    return err;
}

// Дожидаемся, пока DMA доиграет хвост, и останавливаем тактирование I2S
static void speakerStop(bool drain) {
    if (!speakerRunning) return;
    if (drain) delay(SPK_DRAIN_MS);
    i2s_channel_disable(spkChan);
    speakerRunning = false;
}

// =========================================================================
// ПРОИГРЫВАТЕЛЬ ДИНАМИКА
// Играет в отдельной задаче, поэтому WebServer свободен и принимает
// /spk/ctl?cmd=pause|resume|stop|restart прямо во время воспроизведения.
// Источники: файл из LittleFS, трансляция (принимается в PSRAM и остается
// там для повтора), тестовый бип. Все поля ниже меняются только под plMux.
// =========================================================================
enum PlayerSrc : uint8_t { SRC_NONE, SRC_FILE, SRC_STREAM, SRC_BEEP };
enum PlayerState : uint8_t { PL_IDLE, PL_PLAYING, PL_PAUSED };

static SemaphoreHandle_t plMux = NULL;
static TaskHandle_t plTask = NULL;
static PlayerState plState = PL_IDLE;
static PlayerSrc plSrc = SRC_NONE;
static String plName;            // путь файла или подпись источника
static File plFile;
static uint32_t plDataOff = 0;   // начало PCM в файле
static uint32_t plLen = 0;       // длина PCM в байтах (для трансляции растет по мере приема)
static uint32_t plPos = 0;       // позиция в байтах
static bool plSeek = false;      // файл надо перемотать на plPos перед чтением

static uint8_t *strBuf = NULL;   // трансляция в PSRAM
static uint32_t strCap = 0;
static bool strDone = true;      // прием закончен
static uint32_t strHdrLeft = 0;  // сколько байт WAV заголовка еще отрезать
static bool strOverflow = false;
static const uint32_t STREAM_MAX_BYTES = 4UL * 1024 * 1024; // ~131 с
static const uint32_t BEEP_BYTES = AUDIO_SAMPLE_RATE / 4 * 2; // 250 мс
static const uint32_t SPK_DMA_BYTES = AUDIO_DMA_BUF_COUNT * AUDIO_DMA_BUF_LEN * 2;

static void plLock() { xSemaphoreTake(plMux, portMAX_DELAY); }
static void plUnlock() { xSemaphoreGive(plMux); }
static void plWake() { if (plTask) xTaskNotifyGive(plTask); }

static void plFreeStreamLocked() {
    if (strBuf) heap_caps_free(strBuf);
    strBuf = NULL; strCap = 0; strDone = true;
}

// Остановка с возвратом в начало. Источник запоминается: "Сначала" сыграет его снова
static void plStopLocked() {
    plState = PL_IDLE;
    plPos = 0;
    plSeek = true;
    if (plFile) plFile.close();
}

// Новый источник вместо старого (старый файл закрывается, буфер трансляции освобождается)
static void plReplaceLocked(PlayerSrc src, const String &name) {
    plStopLocked();
    if (src != SRC_STREAM) plFreeStreamLocked();
    plSrc = src;
    plName = name;
}

// Файл удаляют или перезаписывают: если он играет, останавливаемся и забываем его
static void playerReleaseFile(const String &path) {
    if (!plMux) return;
    plLock();
    if (plSrc == SRC_FILE && plName == path) {
        plStopLocked();
        plSrc = SRC_NONE;
        plName = "";
    }
    plUnlock();
}

// Открывает файл заново (после стопа или окончания) для "Сначала"/"Продолжить"
static bool plReopenLocked() {
    if (plSrc != SRC_FILE) return true;
    if (plFile) return true;
    plFile = LittleFS.open(plName, "r");
    if (!plFile) { plSrc = SRC_NONE; plName = ""; return false; }
    plSeek = true;
    return true;
}

static void playerTask(void *) {
    static uint8_t buf[1024];
    for (;;) {
        size_t n = 0;
        bool wait = false, finished = false;
        plLock();
        if (plState == PL_PLAYING) {
            uint32_t left = plLen > plPos ? plLen - plPos : 0;
            switch (plSrc) {
            case SRC_FILE:
                if (left && plFile) {
                    if (plSeek) { plFile.seek(plDataOff + plPos); plSeek = false; }
                    n = plFile.read(buf, min((uint32_t)sizeof(buf), left));
                }
                if (n == 0) finished = true;
                break;
            case SRC_STREAM:
                n = min((uint32_t)sizeof(buf), left);
                if (!strDone) n &= ~1u; // нечетный хвост дождется следующего чанка
                if (n) memcpy(buf, strBuf + plPos, n);
                else if (strDone) finished = true;
                else wait = true;   // данные еще идут по сети
                break;
            case SRC_BEEP: {
                // 1000 Гц меандр: 16 сэмплов на период при 16 кГц
                n = min((uint32_t)sizeof(buf), left);
                int16_t *smp = (int16_t *)buf;
                uint32_t first = plPos / 2;
                for (size_t i = 0; i < n / 2; i++) smp[i] = (((first + i) % 16) < 8) ? 8000 : -8000;
                if (n == 0) finished = true;
                break;
            }
            default:
                finished = true;
            }
            plPos += n;
            if (finished) plStopLocked();
        }
        plUnlock();

        if (n) {
            speakerStart();
            size_t w;
            speakerWrite(buf, n, &w);
            continue;
        }
        if (wait) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        // Пауза, стоп или конец: при естественном конце даем DMA доиграть хвост
        speakerStop(finished);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static const char *plStateName(PlayerState s) {
    return s == PL_PLAYING ? "playing" : s == PL_PAUSED ? "paused" : "idle";
}
static const char *plSrcName(PlayerSrc s) {
    return s == SRC_FILE ? "file" : s == SRC_STREAM ? "stream" : s == SRC_BEEP ? "beep" : "none";
}

String DoorAudio::playerJson() {
    String j = "{\"state\":\"";
    if (!plMux) return j + "idle\",\"src\":\"none\",\"name\":\"\",\"pos_ms\":0,\"len_ms\":0,\"loading\":false,\"volume\":" + speakerVolume + "}";
    plLock();
    j += plStateName(plState);
    j += "\",\"src\":\"";   j += plSrcName(plSrc);
    String nm = plName;
    nm.replace("\\", "\\\\"); nm.replace("\"", "\\\"");
    j += "\",\"name\":\"";  j += nm;
    j += "\",\"pos_ms\":";  j += (uint32_t)((uint64_t)plPos * 1000 / (AUDIO_SAMPLE_RATE * 2));
    j += ",\"len_ms\":";    j += (uint32_t)((uint64_t)plLen * 1000 / (AUDIO_SAMPLE_RATE * 2));
    j += ",\"loading\":";   j += (plSrc == SRC_STREAM && !strDone) ? "true" : "false";
    plUnlock();
    j += ",\"volume\":";    j += speakerVolume;
    j += "}";
    return j;
}

static void sendPlayerState(WebServer &server, int code = 200) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", DoorAudio::playerJson());
}

// GET /spk/ctl?cmd=pause|resume|stop|restart. Без cmd — только состояние
void DoorAudio::handlePlayerCtl(WebServer &server) {
    if (!speakerReady || !plMux) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(500, "text/plain", "Speaker not initialized");
        return;
    }
    String cmd = server.arg("cmd");
    bool ok = true;
    plLock();
    if (cmd == "pause") {
        if (plState == PL_PLAYING) {
            plState = PL_PAUSED;
            // То, что уже ушло в DMA, при остановке пропадет: отматываем назад, чтобы не потерять
            plPos = plPos > SPK_DMA_BYTES ? (plPos - SPK_DMA_BYTES) & ~1u : 0;
            plSeek = true;
        }
    } else if (cmd == "resume") {
        if (plState == PL_PAUSED) plState = PL_PLAYING;
        else if (plState == PL_IDLE && plSrc != SRC_NONE) {
            ok = plReopenLocked();
            if (ok) { plPos = 0; plSeek = true; plState = PL_PLAYING; }
        }
    } else if (cmd == "stop") {
        plStopLocked();
    } else if (cmd == "restart") {
        if (plSrc == SRC_NONE) ok = false;
        else {
            ok = plReopenLocked();
            if (ok) { plPos = 0; plSeek = true; plState = PL_PLAYING; }
        }
    } else if (cmd.length()) {
        plUnlock();
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(400, "text/plain", "Bad Request: cmd must be pause|resume|stop|restart");
        return;
    }
    plUnlock();
    plWake();
    if (cmd.length()) Serial.printf("[INFO] handlePlayerCtl | cmd=%s ok=%d\n", cmd.c_str(), ok);
    sendPlayerState(server, ok ? 200 : 409);
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
    playerReleaseFile(MIC_FILE_PATH);
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
    // Усиление с мягким ограничителем: выше knee сигнал плавно сжимается к 32767,
    // поэтому громкий звук рядом с микрофоном не дает жесткого клиппинга
    const float gain = DoorAudio::micGain;
    const float knee = 24000.0f, room = 32767.0f - knee;
    uint32_t limited = 0;

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
            float y = hpY * gain;
            float a = fabsf(y);
            if (a > knee) {
                a = knee + room * tanhf((a - knee) / room);
                y = copysignf(a, y);
                limited++;
            }
            pcm[i] = (int16_t)lrintf(y);
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
    Serial.printf("[EXIT] audioStreamTask | return: sent %u bytes, empty reads %u, gain x%d, peak %ld, limited %u, %s %u bytes: %s\n",
                  totalBytes, emptyReads, (int)gain, (long)peak, limited, MIC_FILE_PATH, fileLen, saved ? "saved" : "NOT saved");
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

    Preferences prefs;
    prefs.begin("audio", true);
    micGain = constrain(prefs.getInt("mic_gain", MIC_DEFAULT_GAIN), 1, MIC_MAX_GAIN);
    prefs.end();

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

    Preferences prefs;
    if (prefs.begin("audio", true)) {
        speakerVolume = constrain(prefs.getInt("spk_vol", SPK_DEFAULT_VOLUME), 0, 100);
        prefs.end();
    }
    Serial.printf("[INFO] initI2SSpeaker | volume: %d%%\n", speakerVolume);

    // Канал выключен до начала воспроизведения
    plMux = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(playerTask, "SpkPlayer", 4096, NULL, 4, &plTask, 1);
    speakerReady = true;
    Serial.println("[EXIT] initI2SSpeaker | return: success");
}

// =========================================================================
// ДИАГНОСТИЧЕСКИЙ СИГНАЛ (БИП) ДЛЯ ПРОВЕРКИ ЦАП
// =========================================================================
void DoorAudio::playBeep() {
    Serial.println("[ENTER] playBeep | тестовый сигнал 1000 Гц, 250 мс");
    if (!speakerReady || !plMux) {
        Serial.println("[EXIT ERROR] playBeep | error: speaker not initialized");
        return;
    }
    plLock();
    plReplaceLocked(SRC_BEEP, "beep");
    plLen = BEEP_BYTES;
    plState = PL_PLAYING;
    plUnlock();
    plWake();
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

    // Усиление: /mic/start?gain=N (1..64), запоминается в NVS
    if (server.hasArg("gain")) {
        int g = constrain((int)server.arg("gain").toInt(), 1, MIC_MAX_GAIN);
        if (g != micGain) {
            micGain = g;
            Preferences prefs;
            prefs.begin("audio", false);
            prefs.putInt("mic_gain", g);
            prefs.end();
        }
    }

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
    if (!speakerReady) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(500, "text/plain", "Speaker not initialized");
        return;
    }
    // Прием закончен, воспроизведение продолжается в фоне
    sendPlayerState(server);
}

// Трансляция: данные складываются в PSRAM и сразу играют из задачи проигрывателя.
// Буфер остается после конца, поэтому трансляцию можно поставить на паузу и повторить сначала.
void DoorAudio::handlePlayStreamUpload(WebServer& server) {
    HTTPUpload& upload = server.upload();
    if (!speakerReady || !plMux) return;

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ENTER] handlePlayStreamUpload | UPLOAD_FILE_START, filename=%s\n", upload.filename.c_str());
        plLock();
        plReplaceLocked(SRC_STREAM, upload.filename);
        plFreeStreamLocked();
        strDone = false;
        strHdrLeft = 44; // канонный заголовок WAV, который делает панель
        strOverflow = false;
        plLen = 0;
        plState = PL_PLAYING;
        plUnlock();
        plWake();
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
        uint8_t* data = upload.buf;
        size_t len = upload.currentSize;
        plLock();
        size_t skip = min((size_t)strHdrLeft, len);
        strHdrLeft -= skip; data += skip; len -= skip;
        if (plSrc == SRC_STREAM && len) {
            if (plLen + len > strCap && strCap < STREAM_MAX_BYTES) {
                uint32_t cap = max(strCap ? strCap * 2 : 256UL * 1024, (uint32_t)(plLen + len));
                cap = min(cap, STREAM_MAX_BYTES);
                uint8_t *nb = (uint8_t *)heap_caps_realloc(strBuf, cap, MALLOC_CAP_SPIRAM);
                if (nb) { strBuf = nb; strCap = cap; }
            }
            size_t fit = min((uint32_t)len, strCap - plLen);
            if (fit) { memcpy(strBuf + plLen, data, fit); plLen += fit; }
            if (fit < len && !strOverflow) {
                strOverflow = true;
                Serial.printf("[WARN] handlePlayStreamUpload | буфер %u КБ заполнен, хвост отброшен\n", strCap / 1024);
            }
        }
        plUnlock();
    }
    else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        plLock();
        if (plSrc == SRC_STREAM) strDone = true;
        uint32_t got = plLen;
        plUnlock();
        plWake();
        Serial.printf("[EXIT] handlePlayStreamUpload | %s, %u байт в PSRAM\n",
                      upload.status == UPLOAD_FILE_END ? "UPLOAD_FILE_END" : "UPLOAD_FILE_ABORTED", got);
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
        playerReleaseFile(path); // перезапись играющего файла
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

    uint32_t dataOff = f.position();
    dataSize = min(dataSize, (uint32_t)(f.size() > dataOff ? f.size() - dataOff : 0));
    plLock();
    plReplaceLocked(SRC_FILE, path);
    plFile = f;
    plDataOff = dataOff;
    plLen = dataSize & ~1u;
    plSeek = false; // файл уже стоит на начале данных
    plState = PL_PLAYING;
    plUnlock();
    plWake();

    sendPlayerState(server);
    Serial.printf("[EXIT] handlePlayFile | return: playing %s, %u bytes\n", path.c_str(), dataSize);
}

// Громкость динамика с сохранением в NVS: GET /spk/volume?val=0..100 (без val — только прочитать)
void DoorAudio::handleSpeakerVolume(WebServer& server) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (server.hasArg("val")) {
        String a = server.arg("val");
        if (a.length() == 0 || a.toInt() < 0 || a.toInt() > 100 || (a.toInt() == 0 && a != "0")) {
            server.send(400, "text/plain", "Bad Request: val must be 0..100");
            return;
        }
        int v = a.toInt();
        if (v != speakerVolume) {
            speakerVolume = v;
            Preferences prefs;
            if (prefs.begin("audio", false)) {
                prefs.putInt("spk_vol", v);
                prefs.end();
            }
            Serial.printf("[INFO] handleSpeakerVolume | volume: %d%%\n", v);
        }
    }
    server.send(200, "application/json", String("{\"volume\":") + speakerVolume + "}");
}

void DoorAudio::handleDeleteFile(WebServer& server) {
    Serial.println("[ENTER] handleDeleteFile | params: WebServer");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String path;
    if (!getSafeFileName(server, path)) {
        server.send(400, "text/plain", "Bad Request: invalid name");
        return;
    }
    playerReleaseFile(path);
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
