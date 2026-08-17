#include "app_audio_input.h"

#include <string.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_input";

#define RECORD_CHUNK_SAMPLES 512
#define RECORD_CHANNELS 1
#define RECORD_BITS 16
#define RECORD_MIN_MS 800
#define RECORD_TAIL_MS 250

static esp_codec_dev_handle_t s_mic;
static bool s_open;

static esp_err_t codec_ret_to_err(int ret)
{
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

esp_err_t app_audio_input_init(void)
{
    if (s_mic == NULL) {
        s_mic = bsp_audio_codec_microphone_init();
        if (s_mic == NULL) {
            ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
            return ESP_FAIL;
        }
    }

    if (!s_open) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = CONFIG_APP_AUDIO_SAMPLE_RATE,
            .channel = RECORD_CHANNELS,
            .bits_per_sample = RECORD_BITS,
        };

        int ret = esp_codec_dev_open(s_mic, &fs);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
            return codec_ret_to_err(ret);
        }
        s_open = true;

        ret = esp_codec_dev_set_in_gain(s_mic, 42.0f);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "set microphone gain failed: %d", ret);
        }
    }

    return ESP_OK;
}

esp_err_t app_audio_input_record_until_stopped(uint32_t max_ms,
                                               volatile bool *stop_requested,
                                               app_audio_recording_t *out)
{
    if (stop_requested == NULL || out == NULL || max_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    esp_err_t ret = app_audio_input_init();
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t max_samples = ((size_t)CONFIG_APP_AUDIO_SAMPLE_RATE * max_ms) / 1000;
    const size_t min_samples_config = ((size_t)CONFIG_APP_AUDIO_SAMPLE_RATE * RECORD_MIN_MS) / 1000;
    const size_t min_samples = min_samples_config < max_samples ? min_samples_config : max_samples;
    const size_t tail_samples_config = ((size_t)CONFIG_APP_AUDIO_SAMPLE_RATE * RECORD_TAIL_MS) / 1000;
    const size_t max_bytes = max_samples * sizeof(int16_t);
    int16_t *pcm = heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        pcm = heap_caps_malloc(max_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (pcm == NULL) {
        ESP_LOGE(TAG, "record buffer alloc failed: %u bytes", (unsigned)max_bytes);
        return ESP_ERR_NO_MEM;
    }

    int16_t chunk[RECORD_CHUNK_SAMPLES];
    size_t samples = 0;
    int32_t peak = 0;
    uint64_t sum_sq = 0;
    bool tail_started = false;
    size_t tail_samples_left = 0;

    while (samples < max_samples) {
        if (*stop_requested && samples >= min_samples && !tail_started) {
            tail_started = true;
            tail_samples_left = tail_samples_config;
        }
        if (tail_started && tail_samples_left == 0) {
            break;
        }

        size_t remain = max_samples - samples;
        size_t want_samples = remain > RECORD_CHUNK_SAMPLES ? RECORD_CHUNK_SAMPLES : remain;
        if (tail_started && want_samples > tail_samples_left) {
            want_samples = tail_samples_left;
        }
        int want_bytes = (int)(want_samples * sizeof(int16_t));

        int read_ret = esp_codec_dev_read(s_mic, chunk, want_bytes);
        if (read_ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", read_ret);
            heap_caps_free(pcm);
            return codec_ret_to_err(read_ret);
        }

        memcpy(&pcm[samples], chunk, want_bytes);

        for (size_t i = 0; i < want_samples; i++) {
            int32_t v = chunk[i];
            int32_t a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
            sum_sq += (uint64_t)((int64_t)v * (int64_t)v);
        }

        samples += want_samples;
        if (tail_started) {
            tail_samples_left -= want_samples;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (samples == 0) {
        heap_caps_free(pcm);
        return ESP_ERR_INVALID_SIZE;
    }

    out->pcm = pcm;
    out->samples = samples;
    out->sample_rate = CONFIG_APP_AUDIO_SAMPLE_RATE;
    out->duration_ms = (uint32_t)((samples * 1000U) / CONFIG_APP_AUDIO_SAMPLE_RATE);
    out->peak = peak;
    out->rms = isqrt_u64(sum_sq / samples);

    ESP_LOGI(TAG, "recorded %u ms, samples=%u, peak=%ld, rms=%u",
             (unsigned)out->duration_ms,
             (unsigned)out->samples,
             (long)out->peak,
             (unsigned)out->rms);
    return ESP_OK;
}

void app_audio_recording_free(app_audio_recording_t *recording)
{
    if (recording == NULL) {
        return;
    }

    if (recording->pcm != NULL) {
        heap_caps_free(recording->pcm);
    }
    memset(recording, 0, sizeof(*recording));
}
