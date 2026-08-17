#pragma once

#include "esp_err.h"

esp_err_t app_cloud_tts_speak(const char *text);
const char *app_cloud_tts_status_text(void);
