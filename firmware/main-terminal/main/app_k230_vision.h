#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool fall_detected;
    uint32_t frame_count;
    uint32_t error_count;
    int64_t last_update_ms;
    char last_line[48];
} app_k230_vision_sample_t;

esp_err_t app_k230_vision_start(void);
void app_k230_vision_get_latest(app_k230_vision_sample_t *out);
const char *app_k230_vision_status_text(void);

#ifdef __cplusplus
}
#endif
