#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t app_cloud_llm_chat(const char *query, char *reply, size_t reply_size);
const char *app_cloud_llm_status_text(void);
