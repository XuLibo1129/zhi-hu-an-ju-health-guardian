#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool alarm;
    bool motion_ok;
    float fall_probability;
    float smooth_probability;
    float threshold;
    float effective_threshold;
    float motion_score;
    float motion_threshold;
    uint32_t samples;
    uint32_t frame_count;
    uint32_t error_count;
    int64_t last_update_ms;
    char node_id[32];
    char state[32];
} app_csi_receiver_sample_t;

esp_err_t app_csi_receiver_start(void);
void app_csi_receiver_get_latest(app_csi_receiver_sample_t *out);
void app_csi_receiver_get_csi_latest(app_csi_receiver_sample_t *out);
void app_csi_receiver_get_ld6002c_latest(app_csi_receiver_sample_t *out);
const char *app_csi_receiver_status_text(void);

#ifdef __cplusplus
}
#endif
