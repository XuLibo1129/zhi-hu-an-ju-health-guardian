#include "app_csi_receiver.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "app_device_agent.h"

#ifndef CONFIG_APP_CSI_HTTP_ENABLE
#define CONFIG_APP_CSI_HTTP_ENABLE 1
#endif

#ifndef CONFIG_APP_CSI_HTTP_PORT
#define CONFIG_APP_CSI_HTTP_PORT 8090
#endif

#ifndef CONFIG_APP_CSI_HTTP_MAX_BODY
#define CONFIG_APP_CSI_HTTP_MAX_BODY 1024
#endif

#define CSI_HTTP_PATH "/api/sensor/csi"
#define LD6002C_HTTP_PATH "/api/sensor/ld6002c"
#define DEVICE_STATUS_HTTP_PATH "/api/device/status"
#define DEVICE_HISTORY_HTTP_PATH "/api/device/history"
#define LEGACY_STATUS_HTTP_PATH "/api/agent/status"
#define LEGACY_HISTORY_HTTP_PATH "/api/agent/history"
#define DEVICE_HTTP_MAX_JSON 4096

static const char *TAG = "app_csi_receiver";

typedef enum {
    SENSOR_KIND_CSI = 0,
    SENSOR_KIND_LD6002C,
} sensor_kind_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_csi_receiver_sample_t s_latest;
static app_csi_receiver_sample_t s_csi_latest;
static app_csi_receiver_sample_t s_ld6002c_latest;
static httpd_handle_t s_server;
static bool s_started;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool json_bool(const cJSON *root, const char *name, bool default_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static float json_float(const cJSON *root, const char *name, float default_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return default_value;
}

static uint32_t json_u32(const cJSON *root, const char *name, uint32_t default_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
        return (uint32_t)item->valuedouble;
    }
    return default_value;
}

static void json_string_copy(const cJSON *root, const char *name, char *out, size_t out_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_size, "%s", item->valuestring);
    } else if (out_size > 0) {
        out[0] = '\0';
    }
}

static bool state_is_fall_alarm(const char *state)
{
    if (state == NULL) {
        return false;
    }

    return strstr(state, "\xE7\x96\x91\xE4\xBC\xBC\xE8\xB7\x8C\xE5\x80\x92") != NULL ||
           strstr(state, "\xE8\xB7\x8C\xE5\x80\x92") != NULL;
}

static sensor_kind_t sensor_kind_from_uri(const char *uri)
{
    if (uri != NULL && strcmp(uri, LD6002C_HTTP_PATH) == 0) {
        return SENSOR_KIND_LD6002C;
    }
    return SENSOR_KIND_CSI;
}

static void set_json_response_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static void copy_sample_locked(app_csi_receiver_sample_t *dst,
                               const app_csi_receiver_sample_t *sample)
{
    uint32_t frame_count = dst->frame_count + 1;
    uint32_t error_count = dst->error_count;

    *dst = *sample;
    dst->frame_count = frame_count;
    dst->error_count = error_count;
}

static void save_sample(const app_csi_receiver_sample_t *sample, sensor_kind_t kind)
{
    portENTER_CRITICAL(&s_lock);
    copy_sample_locked(&s_latest, sample);
    if (kind == SENSOR_KIND_LD6002C) {
        copy_sample_locked(&s_ld6002c_latest, sample);
    } else {
        copy_sample_locked(&s_csi_latest, sample);
    }
    portEXIT_CRITICAL(&s_lock);
}

static void count_error(void)
{
    portENTER_CRITICAL(&s_lock);
    s_latest.error_count++;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len <= 0 || req->content_len >= buf_size ||
        req->content_len > CONFIG_APP_CSI_HTTP_MAX_BODY) {
        ESP_LOGW(TAG, "bad body size: %d", req->content_len);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGW(TAG, "recv failed: %d", ret);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t csi_post_handler(httpd_req_t *req)
{
    char body[CONFIG_APP_CSI_HTTP_MAX_BODY + 1];
    esp_err_t ret = recv_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        count_error();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        count_error();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    app_csi_receiver_sample_t sample = {0};
    sensor_kind_t kind = sensor_kind_from_uri(req->uri);
    sample.valid = true;
    sample.last_update_ms = now_ms();
    json_string_copy(root, "node_id", sample.node_id, sizeof(sample.node_id));
    json_string_copy(root, "state", sample.state, sizeof(sample.state));
    sample.fall_probability = json_float(root, "fall_probability", 0.0f);
    sample.smooth_probability = json_float(root, "smooth_probability", sample.fall_probability);
    sample.threshold = json_float(root, "threshold", 0.0f);
    sample.effective_threshold = json_float(root, "effective_threshold", sample.threshold);
    sample.motion_score = json_float(root, "motion_score", 0.0f);
    sample.motion_threshold = json_float(root, "motion_threshold", 0.0f);
    sample.motion_ok = json_bool(root, "motion_ok", true);
    sample.samples = json_u32(root, "samples", 0);
    sample.alarm = json_bool(root, "alarm", false) ||
                   json_bool(root, "fall_detected", false) ||
                   state_is_fall_alarm(sample.state);

    cJSON_Delete(root);
    save_sample(&sample, kind);

    ESP_LOGI(TAG,
             "fall sensor post path=%s node=%s state=%s alarm=%d p=%.3f smooth=%.3f samples=%" PRIu32,
             req->uri,
             sample.node_id[0] ? sample.node_id : "-",
             sample.state[0] ? sample.state : "-",
             sample.alarm,
             sample.fall_probability,
             sample.smooth_probability,
             sample.samples);

    set_json_response_headers(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t device_status_get_handler(httpd_req_t *req)
{
    char json[DEVICE_HTTP_MAX_JSON] = {0};
    esp_err_t ret = app_device_agent_build_status_json(json, sizeof(json));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device status failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "device status failed");
        return ESP_OK;
    }

    set_json_response_headers(req);
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t device_history_get_handler(httpd_req_t *req)
{
    char json[DEVICE_HTTP_MAX_JSON] = {0};
    esp_err_t ret = app_device_agent_build_history_json(json, sizeof(json));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device history failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "device history failed");
        return ESP_OK;
    }

    set_json_response_headers(req);
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t app_csi_receiver_start(void)
{
#if !CONFIG_APP_CSI_HTTP_ENABLE
    ESP_LOGI(TAG, "CSI HTTP receiver disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_started) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_APP_CSI_HTTP_PORT;
    config.ctrl_port = CONFIG_APP_CSI_HTTP_PORT + 1;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t csi_uri = {
        .uri = CSI_HTTP_PATH,
        .method = HTTP_POST,
        .handler = csi_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t ld6002c_uri = {
        .uri = LD6002C_HTTP_PATH,
        .method = HTTP_POST,
        .handler = csi_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t device_status_uri = {
        .uri = DEVICE_STATUS_HTTP_PATH,
        .method = HTTP_GET,
        .handler = device_status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t device_history_uri = {
        .uri = DEVICE_HISTORY_HTTP_PATH,
        .method = HTTP_GET,
        .handler = device_history_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t legacy_status_uri = {
        .uri = LEGACY_STATUS_HTTP_PATH,
        .method = HTTP_GET,
        .handler = device_status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t legacy_history_uri = {
        .uri = LEGACY_HISTORY_HTTP_PATH,
        .method = HTTP_GET,
        .handler = device_history_get_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(s_server, &csi_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &ld6002c_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register LD6002C URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &device_status_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register device status URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &device_history_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register device history URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &legacy_status_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register legacy status URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &legacy_history_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register legacy history URI failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    s_started = true;
    ESP_LOGI(TAG, "fall sensor HTTP receiver listening: http://0.0.0.0:%d%s and %s",
             CONFIG_APP_CSI_HTTP_PORT, CSI_HTTP_PATH, LD6002C_HTTP_PATH);
    ESP_LOGI(TAG, "device status API listening: %s and %s",
             DEVICE_STATUS_HTTP_PATH, DEVICE_HISTORY_HTTP_PATH);
    return ESP_OK;
#endif
}

void app_csi_receiver_get_latest(app_csi_receiver_sample_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_lock);
}

void app_csi_receiver_get_csi_latest(app_csi_receiver_sample_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_csi_latest;
    portEXIT_CRITICAL(&s_lock);
}

void app_csi_receiver_get_ld6002c_latest(app_csi_receiver_sample_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_ld6002c_latest;
    portEXIT_CRITICAL(&s_lock);
}

const char *app_csi_receiver_status_text(void)
{
    static char status[96];
    app_csi_receiver_sample_t sample = {0};
    app_csi_receiver_get_latest(&sample);

    if (!sample.valid) {
        snprintf(status, sizeof(status), "waiting");
    } else {
        snprintf(status, sizeof(status), "%s %.2f",
                 sample.alarm ? "alarm" : "normal",
                 sample.smooth_probability);
    }

    return status;
}
