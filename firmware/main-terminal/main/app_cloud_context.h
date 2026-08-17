#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

bool app_cloud_context_needs_realtime(const char *query);
esp_err_t app_cloud_context_fetch_realtime(const char *query, char *context, size_t context_size);
