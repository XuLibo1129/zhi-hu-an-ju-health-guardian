#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int16_t *pcm;
    size_t samples;
    uint32_t sample_rate;
    uint32_t duration_ms;
    int32_t peak;
    uint32_t rms;
} app_audio_recording_t;

esp_err_t app_audio_input_init(void);
esp_err_t app_audio_input_record_until_stopped(uint32_t max_ms,
                                               volatile bool *stop_requested,
                                               app_audio_recording_t *out);
void app_audio_recording_free(app_audio_recording_t *recording);
