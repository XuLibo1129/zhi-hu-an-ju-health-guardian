#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool network_ready;

    bool vital_radar_online;
    bool human_present;
    bool heart_valid;
    float heart_bpm;
    bool breath_valid;
    float breath_bpm;

    bool k230_online;
    bool k230_fall;
    bool k230_valid;
    uint32_t k230_frame_count;
    uint32_t k230_error_count;
    int64_t k230_age_ms;
    char k230_last_line[48];
    bool csi_online;
    bool csi_fall;
    float csi_probability;
    bool ld6002c_online;
    bool ld6002c_fall;
    float ld6002c_probability;

    int fusion_score;
    bool fusion_alarm;

} app_device_agent_status_t;

void app_device_agent_get_status(app_device_agent_status_t *out);
esp_err_t app_device_agent_build_status_json(char *json, size_t json_size);
esp_err_t app_device_agent_build_history_json(char *json, size_t json_size);
