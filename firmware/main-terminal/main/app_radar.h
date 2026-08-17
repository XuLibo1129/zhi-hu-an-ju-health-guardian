#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_human;
    bool human_present;
    bool has_breath_bpm;
    float breath_bpm;
    int64_t last_breath_ms;
    bool has_heart_bpm;
    float heart_bpm;
    int64_t last_heart_ms;
    bool has_range_m;
    float range_m;
    bool has_fall;
    bool fall_detected;
    uint32_t frame_count;
    uint32_t error_count;
    uint32_t bytes_total;
    int64_t last_frame_ms;
} app_radar_sample_t;

esp_err_t app_radar_start(void);
void app_radar_get_latest(app_radar_sample_t *out);
const char *app_radar_status_text(void);

#ifdef __cplusplus
}
#endif
