#include "app_cloud_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_network.h"

static const char *TAG = "app_cloud_context";

// Empty string Kconfig values may be omitted from sdkconfig.h. Keep the
// optional realtime-context feature buildable before the user configures it.
#ifndef CONFIG_APP_CLOUD_ASR_URL
#define CONFIG_APP_CLOUD_ASR_URL ""
#endif
#ifndef CONFIG_APP_CLOUD_ASR_API_KEY
#define CONFIG_APP_CLOUD_ASR_API_KEY ""
#endif

#define REALTIME_RX_BUFFER_SIZE 4096

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_rx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_rx_t *rx = (http_rx_t *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && rx != NULL && event->data_len > 0) {
        const size_t remain = rx->cap - rx->len - 1;
        const size_t copy_len = event->data_len < remain ? event->data_len : remain;
        if (copy_len > 0) {
            memcpy(rx->buf + rx->len, event->data, copy_len);
            rx->len += copy_len;
            rx->buf[rx->len] = '\0';
        }
    }

    return ESP_OK;
}
static bool contains_any(const char *text, const char *const *words, size_t count)
{
    if (text == NULL) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (strstr(text, words[i]) != NULL) {
            return true;
        }
    }
    return false;
}

bool app_cloud_context_needs_realtime(const char *query)
{
    static const char *const words[] = {
        "\xE6\x96\xB0\xE9\x97\xBB",
        "\xE6\x9C\x80\xE6\x96\xB0",
        "\xE7\x83\xAD\xE6\x90\x9C",
        "\xE4\xBB\x8A\xE6\x97\xA5\xE5\xA4\xB4\xE6\x9D\xA1",
        "\xE5\xAE\x9E\xE6\x97\xB6",
        "\xE5\x8F\x91\xE7\x94\x9F\xE4\xBA\x86\xE4\xBB\x80\xE4\xB9\x88",
        "news",
    };
    return contains_any(query, words, sizeof(words) / sizeof(words[0]));
}

static esp_err_t build_realtime_url(char *url, size_t url_size)
{
    if (url == NULL || url_size == 0 || strlen(CONFIG_APP_CLOUD_ASR_URL) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *marker = strstr(CONFIG_APP_CLOUD_ASR_URL, "/v1/");
    if (marker != NULL) {
        const size_t prefix_len = (size_t)(marker - CONFIG_APP_CLOUD_ASR_URL);
        int len = snprintf(url, url_size, "%.*s/v1/realtime",
                           (int)prefix_len, CONFIG_APP_CLOUD_ASR_URL);
        return (len > 0 && len < (int)url_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    return ESP_ERR_INVALID_STATE;
}

static esp_err_t build_payload(const char *query, char **out)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "query", query != NULL ? query : "");
    *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t parse_context(const char *json, char *context, size_t context_size)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *item = cJSON_GetObjectItem(root, "context");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(context, item->valuestring, context_size);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_cloud_context_fetch_realtime(const char *query, char *context, size_t context_size)
{
    if (context == NULL || context_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    context[0] = '\0';

    if (!app_network_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[192] = {0};
    esp_err_t ret = build_realtime_url(url, sizeof(url));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "build realtime URL failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char *payload = NULL;
    ret = build_payload(query, &payload);
    if (ret != ESP_OK) {
        return ret;
    }

    char *rx_buf = calloc(1, REALTIME_RX_BUFFER_SIZE);
    if (rx_buf == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    http_rx_t rx = {
        .buf = rx_buf,
        .len = 0,
        .cap = REALTIME_RX_BUFFER_SIZE,
    };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &rx,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(payload);
        free(rx_buf);
        return ESP_FAIL;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_APP_CLOUD_ASR_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    ESP_LOGI(TAG, "Realtime context requesting: %s", url);
    ret = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Realtime HTTP status=%d rx_len=%u", status_code, (unsigned)rx.len);

    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = parse_context(rx.buf, context, context_size);
    } else {
        ESP_LOGW(TAG, "Realtime request failed status=%d err=%s body=%s",
                 status_code, esp_err_to_name(ret), rx.buf);
        ret = ret == ESP_OK ? ESP_FAIL : ret;
    }

    esp_http_client_cleanup(client);
    free(payload);
    free(rx_buf);
    return ret;
}
