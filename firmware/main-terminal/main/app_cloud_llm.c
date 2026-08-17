#include "app_cloud_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_network.h"

static const char *TAG = "app_cloud_llm";

#ifndef CONFIG_APP_CLOUD_LLM_URL
#define CONFIG_APP_CLOUD_LLM_URL ""
#endif
#ifndef CONFIG_APP_CLOUD_LLM_API_KEY
#define CONFIG_APP_CLOUD_LLM_API_KEY ""
#endif
#ifndef CONFIG_APP_CLOUD_LLM_MODEL
#define CONFIG_APP_CLOUD_LLM_MODEL ""
#endif
#ifndef CONFIG_APP_CLOUD_LLM_TIMEOUT_MS
#define CONFIG_APP_CLOUD_LLM_TIMEOUT_MS 15000
#endif

#define HTTP_RX_BUFFER_SIZE 8192

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_rx_t;

static char s_status[96] = "LLM: idle";

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

static void set_status(const char *status)
{
    strlcpy(s_status, status, sizeof(s_status));
    ESP_LOGI(TAG, "%s", s_status);
}

static bool is_blank_text(const char *text)
{
    if (text == NULL) {
        return true;
    }

    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text;
        if (ch > ' ') {
            return false;
        }
        text++;
    }

    return true;
}

static esp_err_t build_payload(const char *query, char **out)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system_msg = cJSON_CreateObject();
    cJSON *user_msg = cJSON_CreateObject();

    if (root == NULL || messages == NULL || system_msg == NULL || user_msg == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(system_msg);
        cJSON_Delete(user_msg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", CONFIG_APP_CLOUD_LLM_MODEL);
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddNumberToObject(root, "max_tokens", 320);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content",
                            "You are a helpful AI companion inside an elderly-care health guardian terminal. "
                            "Reply naturally in warm Simplified Chinese, like a real conversational assistant. "
                            "Only return the final answer; the message content must not be empty. "
                            "Do not use Markdown or emoji. "
                            "Do not give medical diagnosis and do not cancel alarms. "
                            "Keep the answer suitable for spoken output. "
                            "For ordinary chat, use 1 to 3 concise sentences. "
                            "For health or travel suggestions, be practical and gentle, usually within 120 Chinese characters.");
    cJSON_AddItemToArray(messages, system_msg);

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", query);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddItemToObject(root, "messages", messages);
    *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return *out == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t parse_reply(const char *json, char *reply, size_t reply_size)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = first != NULL ? cJSON_GetObjectItem(first, "message") : NULL;
    cJSON *content = message != NULL ? cJSON_GetObjectItem(message, "content") : NULL;

    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(reply, content->valuestring, reply_size);
    if (is_blank_text(reply)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_cloud_llm_chat(const char *query, char *reply, size_t reply_size)
{
#if CONFIG_APP_CLOUD_LLM_ENABLE
    if (reply == NULL || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(CONFIG_APP_CLOUD_LLM_URL) == 0 ||
        strlen(CONFIG_APP_CLOUD_LLM_API_KEY) == 0 ||
        strlen(CONFIG_APP_CLOUD_LLM_MODEL) == 0) {
        strlcpy(reply, "LLM is not configured. Set URL, model and API key in menuconfig.", reply_size);
        set_status("LLM: not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_network_is_ready()) {
        snprintf(reply, reply_size, "Network is not ready: %s", app_network_status_text());
        set_status("LLM: wait network");
        return ESP_ERR_INVALID_STATE;
    }

    char *payload = NULL;
    esp_err_t ret = build_payload(query, &payload);
    if (ret != ESP_OK) {
        strlcpy(reply, "LLM payload build failed.", reply_size);
        return ret;
    }

    char *rx_buf = calloc(1, HTTP_RX_BUFFER_SIZE);
    if (rx_buf == NULL) {
        free(payload);
        strlcpy(reply, "LLM response buffer allocation failed.", reply_size);
        return ESP_ERR_NO_MEM;
    }

    http_rx_t rx = {
        .buf = rx_buf,
        .len = 0,
        .cap = HTTP_RX_BUFFER_SIZE,
    };

    esp_http_client_config_t config = {
        .url = CONFIG_APP_CLOUD_LLM_URL,
        .event_handler = http_event_handler,
        .user_data = &rx,
        .timeout_ms = CONFIG_APP_CLOUD_LLM_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(payload);
        free(rx_buf);
        strlcpy(reply, "LLM HTTP client creation failed.", reply_size);
        return ESP_FAIL;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_APP_CLOUD_LLM_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    set_status("LLM: requesting");
    ret = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    const int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d content_length=%d rx_len=%u",
             status_code, content_length, (unsigned)rx.len);

    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = parse_reply(rx.buf, reply, reply_size);
        if (ret == ESP_OK) {
            set_status("LLM: ok");
        } else {
            strlcpy(reply, "LLM returned data, but choices[0].message.content parse failed.", reply_size);
            set_status("LLM: parse failed");
        }
    } else {
        snprintf(reply, reply_size, "LLM request failed: HTTP=%d err=%s", status_code, esp_err_to_name(ret));
        set_status("LLM: request failed");
        ret = ret == ESP_OK ? ESP_FAIL : ret;
    }

    esp_http_client_cleanup(client);
    free(payload);
    free(rx_buf);
    return ret;
#else
    if (reply != NULL && reply_size > 0) {
        strlcpy(reply, "Cloud LLM is disabled in menuconfig.", reply_size);
    }
    set_status("LLM: disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *app_cloud_llm_status_text(void)
{
    return s_status;
}
