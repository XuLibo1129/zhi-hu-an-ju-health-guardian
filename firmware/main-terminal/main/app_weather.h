#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    char city[48];
    char condition[24];
    int temperature_c;
    int humidity;
    int weather_code;
    int64_t updated_ms;
} app_weather_info_t;

esp_err_t app_weather_start(void);
void app_weather_get_latest(app_weather_info_t *out);
const char *app_weather_status_text(void);

#ifdef __cplusplus
}
#endif
