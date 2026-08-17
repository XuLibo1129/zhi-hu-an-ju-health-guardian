#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t app_cloud_asr_transcribe_pcm(const int16_t *pcm,
                                       size_t samples,
                                       uint32_t sample_rate,
                                       char *text,
                                       size_t text_size);
const char *app_cloud_asr_status_text(void);
