#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t app_voice_output_init(void);
esp_err_t app_voice_output_speak_text(const char *text);
esp_err_t app_voice_output_play_alarm_tone(uint32_t duration_ms);
esp_err_t app_voice_output_play_pcm(const int16_t *pcm, size_t samples, uint32_t sample_rate);
