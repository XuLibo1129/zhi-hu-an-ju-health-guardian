#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VITALS_HISTORY_DAYS 5

typedef struct {
    char day[16];
    bool has_heart_bpm;
    uint16_t heart_bpm;
    uint32_t heart_samples;
    bool has_breath_bpm;
    uint16_t breath_bpm;
    uint32_t breath_samples;
} app_vitals_history_day_t;

esp_err_t app_vitals_history_start(void);
size_t app_vitals_history_get_recent(app_vitals_history_day_t *out, size_t max_count);
bool app_vitals_history_sd_ready(void);
const char *app_vitals_history_status_text(void);

#ifdef __cplusplus
}
#endif
