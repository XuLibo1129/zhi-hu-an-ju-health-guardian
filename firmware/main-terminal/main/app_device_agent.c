#include "app_device_agent.h"

#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"

#include "app_csi_receiver.h"
#include "app_k230_vision.h"
#include "app_network.h"
#include "app_radar.h"
#include "app_vitals_history.h"

#define NODE_STATUS_STALE_MS 10000
#define VITAL_STATUS_STALE_MS 15000
#define FALL_FUSION_PRIMARY_SCORE 45
#define FALL_FUSION_AUX_SCORE 20
#define FALL_FUSION_THRESHOLD 65

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool is_recent(int64_t timestamp_ms, int64_t now, int64_t stale_ms)
{
    return timestamp_ms > 0 && now >= timestamp_ms && now - timestamp_ms <= stale_ms;
}

void app_device_agent_get_status(app_device_agent_status_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    const int64_t now = now_ms();

    app_radar_sample_t radar = {0};
    app_k230_vision_sample_t vision = {0};
    app_csi_receiver_sample_t csi = {0};
    app_csi_receiver_sample_t ld6002c = {0};

    app_radar_get_latest(&radar);
    app_k230_vision_get_latest(&vision);
    app_csi_receiver_get_csi_latest(&csi);
    app_csi_receiver_get_ld6002c_latest(&ld6002c);

    out->network_ready = app_network_is_ready();
    out->vital_radar_online = is_recent(radar.last_frame_ms, now, VITAL_STATUS_STALE_MS);
    out->human_present = radar.has_human && radar.human_present;
    out->heart_valid = radar.has_heart_bpm && is_recent(radar.last_heart_ms, now, VITAL_STATUS_STALE_MS);
    out->heart_bpm = radar.heart_bpm;
    out->breath_valid = radar.has_breath_bpm && is_recent(radar.last_breath_ms, now, VITAL_STATUS_STALE_MS);
    out->breath_bpm = radar.breath_bpm;

    out->k230_online = vision.valid && is_recent(vision.last_update_ms, now, NODE_STATUS_STALE_MS);
    out->k230_fall = out->k230_online && vision.fall_detected;
    out->k230_valid = vision.valid;
    out->k230_frame_count = vision.frame_count;
    out->k230_error_count = vision.error_count;
    out->k230_age_ms = (vision.last_update_ms > 0 && now >= vision.last_update_ms) ?
                       (now - vision.last_update_ms) : -1;
    strlcpy(out->k230_last_line, vision.last_line, sizeof(out->k230_last_line));

    out->csi_online = csi.valid && is_recent(csi.last_update_ms, now, NODE_STATUS_STALE_MS);
    out->csi_fall = out->csi_online && csi.alarm;
    out->csi_probability = csi.smooth_probability;

    out->ld6002c_online = ld6002c.valid && is_recent(ld6002c.last_update_ms, now, NODE_STATUS_STALE_MS);
    out->ld6002c_fall = out->ld6002c_online && ld6002c.alarm;
    out->ld6002c_probability = ld6002c.smooth_probability;

    if (out->k230_fall) {
        out->fusion_score += FALL_FUSION_PRIMARY_SCORE;
    }
    if (out->ld6002c_fall) {
        out->fusion_score += FALL_FUSION_PRIMARY_SCORE;
    }
    if (out->csi_fall) {
        out->fusion_score += FALL_FUSION_AUX_SCORE;
    }
    out->fusion_alarm = out->fusion_score >= FALL_FUSION_THRESHOLD;

}

static cJSON *add_node_json(cJSON *nodes, const char *name, bool online, bool fall, float probability)
{
    cJSON *node = cJSON_CreateObject();
    if (node == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(node, "online", online);
    cJSON_AddBoolToObject(node, "fall", fall);
    cJSON_AddNumberToObject(node, "probability", probability);
    cJSON_AddItemToObject(nodes, name, node);
    return node;
}

static void add_history_json(cJSON *root)
{
    app_vitals_history_day_t days[APP_VITALS_HISTORY_DAYS] = {0};
    size_t count = app_vitals_history_get_recent(days, APP_VITALS_HISTORY_DAYS);

    cJSON *history = cJSON_AddObjectToObject(root, "history");
    if (history == NULL) {
        return;
    }

    cJSON_AddBoolToObject(history, "sd_ready", app_vitals_history_sd_ready());
    cJSON_AddStringToObject(history, "status", app_vitals_history_status_text());
    cJSON_AddNumberToObject(history, "count", count);

    cJSON *items = cJSON_AddArrayToObject(history, "days");
    if (items == NULL) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }

        cJSON_AddStringToObject(item, "day", days[i].day);
        cJSON_AddBoolToObject(item, "heart_valid", days[i].has_heart_bpm);
        cJSON_AddNumberToObject(item, "heart_bpm", days[i].has_heart_bpm ? days[i].heart_bpm : 0);
        cJSON_AddNumberToObject(item, "heart_samples", days[i].heart_samples);
        cJSON_AddBoolToObject(item, "breath_valid", days[i].has_breath_bpm);
        cJSON_AddNumberToObject(item, "breath_bpm", days[i].has_breath_bpm ? days[i].breath_bpm : 0);
        cJSON_AddNumberToObject(item, "breath_samples", days[i].breath_samples);
        cJSON_AddItemToArray(items, item);
    }
}

esp_err_t app_device_agent_build_status_json(char *json, size_t json_size)
{
    if (json == NULL || json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    app_device_agent_status_t status = {0};
    app_device_agent_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "device_status");
    cJSON_AddBoolToObject(root, "network_ready", status.network_ready);

    cJSON *vitals = cJSON_AddObjectToObject(root, "vitals");
    if (vitals != NULL) {
        cJSON_AddBoolToObject(vitals, "radar_online", status.vital_radar_online);
        cJSON_AddBoolToObject(vitals, "human_present", status.human_present);
        cJSON_AddBoolToObject(vitals, "heart_valid", status.heart_valid);
        cJSON_AddNumberToObject(vitals, "heart_bpm", status.heart_bpm);
        cJSON_AddBoolToObject(vitals, "breath_valid", status.breath_valid);
        cJSON_AddNumberToObject(vitals, "breath_bpm", status.breath_bpm);
    }

    cJSON *nodes = cJSON_AddObjectToObject(root, "nodes");
    if (nodes != NULL) {
        cJSON *k230 = add_node_json(nodes, "k230_vision", status.k230_online, status.k230_fall, status.k230_fall ? 1.0f : 0.0f);
        if (k230 != NULL) {
            cJSON_AddBoolToObject(k230, "valid", status.k230_valid);
            cJSON_AddNumberToObject(k230, "frame_count", status.k230_frame_count);
            cJSON_AddNumberToObject(k230, "error_count", status.k230_error_count);
            cJSON_AddNumberToObject(k230, "age_ms", status.k230_age_ms);
            cJSON_AddStringToObject(k230, "last_line", status.k230_last_line);
        }
        add_node_json(nodes, "csi", status.csi_online, status.csi_fall, status.csi_probability);
        add_node_json(nodes, "ld6002c", status.ld6002c_online, status.ld6002c_fall, status.ld6002c_probability);
    }

    cJSON *fusion = cJSON_AddObjectToObject(root, "fusion");
    if (fusion != NULL) {
        cJSON_AddNumberToObject(fusion, "score", status.fusion_score);
        cJSON_AddNumberToObject(fusion, "threshold", FALL_FUSION_THRESHOLD);
        cJSON_AddBoolToObject(fusion, "fall_alarm", status.fusion_alarm);
    }

    add_history_json(root);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(json, printed, json_size);
    cJSON_free(printed);
    return ESP_OK;
}

esp_err_t app_device_agent_build_history_json(char *json, size_t json_size)
{
    if (json == NULL || json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    add_history_json(root);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(json, printed, json_size);
    cJSON_free(printed);
    return ESP_OK;
}
