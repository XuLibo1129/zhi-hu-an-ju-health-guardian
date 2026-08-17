#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_sim800_start(void);
bool app_sim800_is_ready(void);
const char *app_sim800_status_text(void);

esp_err_t app_sim800_call(const char *number);
esp_err_t app_sim800_hangup(void);
esp_err_t app_sim800_send_sms(const char *number, const char *text);

esp_err_t app_sim800_emergency_call(void);
esp_err_t app_sim800_emergency_sms(const char *text);
esp_err_t app_sim800_auto_alert(const char *text);

#ifdef __cplusplus
}
#endif
