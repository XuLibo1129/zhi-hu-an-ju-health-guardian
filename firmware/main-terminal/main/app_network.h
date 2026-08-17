#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t app_network_start(void);
bool app_network_is_ready(void);
const char *app_network_status_text(void);
