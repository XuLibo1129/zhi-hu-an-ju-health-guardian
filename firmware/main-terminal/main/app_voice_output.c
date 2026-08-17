#include "app_voice_output.h"

#include <stdbool.h>
#include <string.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "voice_output";

#define VOICE_SAMPLE_RATE CONFIG_APP_AUDIO_SAMPLE_RATE
#define VOICE_CHANNELS 1
#define VOICE_BITS 16
#define VOICE_BEEP_FREQ 880
#define VOICE_BEEP_MS 140
#define VOICE_ALARM_FREQ 1180
#define VOICE_ALARM_AMPLITUDE 10500
#define VOICE_OUTPUT_VOLUME 85
#define VOICE_CHUNK_SAMPLES 256
#define VOICE_PLAY_CHUNK_SAMPLES 1024
#define VOICE_TAIL_SILENCE_MS 60
#define VOICE_OUTPUT_LOCK_TIMEOUT_MS 30000
#define VOICE_BEEP_ENABLE 1

static esp_codec_dev_handle_t s_spk;
static bool s_open;
static uint32_t s_open_sample_rate;
static StaticSemaphore_t s_output_mutex_storage;
static SemaphoreHandle_t s_output_mutex;

static esp_err_t codec_ret_to_err(int ret)
{
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t open_speaker(uint32_t sample_rate)
{
    if (s_spk == NULL) {
        s_spk = bsp_audio_codec_speaker_init();
        if (s_spk == NULL) {
            ESP_LOGE(TAG, "bsp_audio_codec_speaker_init failed");
            return ESP_FAIL;
        }

        int ret = esp_codec_dev_set_out_vol(s_spk, VOICE_OUTPUT_VOLUME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "set speaker volume failed: %d", ret);
        }
    }

    if (s_open && s_open_sample_rate != sample_rate) {
        int ret = esp_codec_dev_close(s_spk);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_close failed before rate switch: %d", ret);
        }
        s_open = false;
        s_open_sample_rate = 0;
    }

    if (!s_open) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = sample_rate,
            .channel = VOICE_CHANNELS,
            .bits_per_sample = VOICE_BITS,
        };

        int ret = esp_codec_dev_open(s_spk, &fs);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
            return codec_ret_to_err(ret);
        }
        s_open = true;
        s_open_sample_rate = sample_rate;
    }

    return ESP_OK;
}

static esp_err_t lock_output(void)
{
    if (s_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_output_mutex, pdMS_TO_TICKS(VOICE_OUTPUT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "speaker busy timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_output(void)
{
    if (s_output_mutex != NULL) {
        xSemaphoreGive(s_output_mutex);
    }
}

esp_err_t app_voice_output_init(void)
{
    if (s_output_mutex == NULL) {
        s_output_mutex = xSemaphoreCreateMutexStatic(&s_output_mutex_storage);
        if (s_output_mutex == NULL) {
            ESP_LOGE(TAG, "speaker mutex init failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = lock_output();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = open_speaker(VOICE_SAMPLE_RATE);
    unlock_output();
    return ret;
}

esp_err_t app_voice_output_play_pcm(const int16_t *pcm, size_t samples, uint32_t sample_rate)
{
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = lock_output();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = open_speaker(sample_rate);
    if (ret != ESP_OK) {
        unlock_output();
        return ret;
    }

    size_t sent = 0;
    while (sent < samples) {
        const size_t remain = samples - sent;
        const size_t todo = remain > VOICE_PLAY_CHUNK_SAMPLES ? VOICE_PLAY_CHUNK_SAMPLES : remain;

        int wr = esp_codec_dev_write(s_spk, (void *)&pcm[sent], todo * sizeof(int16_t));
        if (wr != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", wr);
            ret = codec_ret_to_err(wr);
            goto out;
        }

        sent += todo;
    }

    int16_t silence[VOICE_PLAY_CHUNK_SAMPLES] = {0};
    size_t tail_samples = ((size_t)sample_rate * VOICE_TAIL_SILENCE_MS) / 1000;
    while (tail_samples > 0) {
        const size_t todo = tail_samples > VOICE_PLAY_CHUNK_SAMPLES ? VOICE_PLAY_CHUNK_SAMPLES : tail_samples;
        int wr = esp_codec_dev_write(s_spk, silence, todo * sizeof(int16_t));
        if (wr != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "tail silence write failed: %d", wr);
            break;
        }
        tail_samples -= todo;
    }

    ret = ESP_OK;

out:
    unlock_output();
    return ret;
}

esp_err_t app_voice_output_play_alarm_tone(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = lock_output();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = open_speaker(VOICE_SAMPLE_RATE);
    if (ret != ESP_OK) {
        unlock_output();
        return ret;
    }

    int16_t chunk[VOICE_CHUNK_SAMPLES];
    const uint32_t total_samples = (VOICE_SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t period = VOICE_SAMPLE_RATE / VOICE_ALARM_FREQ;
    uint32_t sent = 0;

    while (sent < total_samples) {
        const uint32_t todo = (total_samples - sent) > VOICE_CHUNK_SAMPLES ?
                              VOICE_CHUNK_SAMPLES : (total_samples - sent);

        for (uint32_t i = 0; i < todo; i++) {
            const uint32_t pos = (sent + i) % period;
            chunk[i] = pos < (period / 2) ? VOICE_ALARM_AMPLITUDE : -VOICE_ALARM_AMPLITUDE;
        }

        int wr = esp_codec_dev_write(s_spk, chunk, todo * sizeof(int16_t));
        if (wr != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "alarm tone write failed: %d", wr);
            ret = codec_ret_to_err(wr);
            goto out;
        }
        sent += todo;
    }

    memset(chunk, 0, sizeof(chunk));
    (void)esp_codec_dev_write(s_spk, chunk, sizeof(chunk));
    ret = ESP_OK;

out:
    unlock_output();
    return ret;
}

esp_err_t app_voice_output_speak_text(const char *text)
{
    if (text == NULL) {
        text = "";
    }

    ESP_LOGI(TAG, "TTS placeholder text=\"%s\"", text);

#if !VOICE_BEEP_ENABLE
    return ESP_OK;
#else

    esp_err_t ret = lock_output();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = open_speaker(VOICE_SAMPLE_RATE);
    if (ret != ESP_OK) {
        unlock_output();
        return ret;
    }

    int16_t chunk[VOICE_CHUNK_SAMPLES];
    const uint32_t total_samples = (VOICE_SAMPLE_RATE * VOICE_BEEP_MS) / 1000;
    const uint32_t period = VOICE_SAMPLE_RATE / VOICE_BEEP_FREQ;
    uint32_t sent = 0;

    while (sent < total_samples) {
        const uint32_t todo = (total_samples - sent) > VOICE_CHUNK_SAMPLES ?
                              VOICE_CHUNK_SAMPLES : (total_samples - sent);

        for (uint32_t i = 0; i < todo; i++) {
            const uint32_t pos = (sent + i) % period;
            chunk[i] = pos < (period / 2) ? 2600 : -2600;
        }

        int wr = esp_codec_dev_write(s_spk, chunk, todo * sizeof(int16_t));
        if (wr != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", wr);
            ret = codec_ret_to_err(wr);
            goto out;
        }
        sent += todo;
    }

    memset(chunk, 0, sizeof(chunk));
    (void)esp_codec_dev_write(s_spk, chunk, sizeof(chunk));
    ret = ESP_OK;

out:
    unlock_output();
    return ret;
#endif
}
