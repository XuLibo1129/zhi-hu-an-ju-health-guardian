#include "app_cloud_tts.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_network.h"
#include "app_voice_output.h"

static const char *TAG = "app_cloud_tts";

#ifndef CONFIG_APP_CLOUD_TTS_URL
#define CONFIG_APP_CLOUD_TTS_URL ""
#endif
#ifndef CONFIG_APP_CLOUD_TTS_API_KEY
#define CONFIG_APP_CLOUD_TTS_API_KEY ""
#endif
#ifndef CONFIG_APP_CLOUD_TTS_MODEL
#define CONFIG_APP_CLOUD_TTS_MODEL ""
#endif

#ifndef CONFIG_APP_CLOUD_TTS_VOICE
#define CONFIG_APP_CLOUD_TTS_VOICE ""
#endif
#ifndef CONFIG_APP_CLOUD_TTS_MAX_BYTES
#define CONFIG_APP_CLOUD_TTS_MAX_BYTES 262144
#endif
#ifndef CONFIG_APP_CLOUD_TTS_TIMEOUT_MS
#define CONFIG_APP_CLOUD_TTS_TIMEOUT_MS 30000
#endif
#ifndef CONFIG_APP_CLOUD_TTS_SAMPLE_RATE
#define CONFIG_APP_CLOUD_TTS_SAMPLE_RATE 16000
#endif

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} http_bin_rx_t;

static char s_status[96] = "TTS: idle";

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_bin_rx_t *rx = (http_bin_rx_t *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && rx != NULL && event->data_len > 0) {
        const size_t remain = rx->cap - rx->len;
        const size_t copy_len = event->data_len < remain ? event->data_len : remain;
        if (copy_len > 0) {
            memcpy(rx->buf + rx->len, event->data, copy_len);
            rx->len += copy_len;
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
        if ((unsigned char)*text > ' ') {
            return false;
        }
        text++;
    }

    return true;
}

static esp_err_t build_payload(const char *text, char **out)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", CONFIG_APP_CLOUD_TTS_MODEL);
    cJSON_AddStringToObject(root, "voice", CONFIG_APP_CLOUD_TTS_VOICE);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "response_format", "pcm");

    *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t app_cloud_tts_speak(const char *text)
{
#if CONFIG_APP_CLOUD_TTS_ENABLE
    if (text == NULL) {
        text = "";
    }

    if (is_blank_text(text)) {
        set_status("TTS: empty text");
        ESP_LOGW(TAG, "TTS skipped because input text is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(CONFIG_APP_CLOUD_TTS_URL) == 0 ||
        strlen(CONFIG_APP_CLOUD_TTS_API_KEY) == 0 ||
        strlen(CONFIG_APP_CLOUD_TTS_MODEL) == 0 ||
        strlen(CONFIG_APP_CLOUD_TTS_VOICE) == 0) {
        set_status("TTS: not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_network_is_ready()) {
        set_status("TTS: wait network");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "TTS text bytes=%u", (unsigned)strlen(text));

    char *payload = NULL;
    esp_err_t ret = build_payload(text, &payload);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *rx_buf = heap_caps_malloc(CONFIG_APP_CLOUD_TTS_MAX_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rx_buf == NULL) {
        rx_buf = heap_caps_malloc(CONFIG_APP_CLOUD_TTS_MAX_BYTES,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (rx_buf == NULL) {
        free(payload);
        ESP_LOGE(TAG, "TTS rx alloc failed: %u bytes", (unsigned)CONFIG_APP_CLOUD_TTS_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }

    http_bin_rx_t rx = {
        .buf = rx_buf,
        .len = 0,
        .cap = CONFIG_APP_CLOUD_TTS_MAX_BYTES,
    };

    esp_http_client_config_t config = {
        .url = CONFIG_APP_CLOUD_TTS_URL,
        .event_handler = http_event_handler,
        .user_data = &rx,
        .timeout_ms = CONFIG_APP_CLOUD_TTS_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(payload);
        heap_caps_free(rx_buf);
        return ESP_FAIL;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_APP_CLOUD_TTS_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    set_status("TTS: requesting");
    ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS HTTP status=%d bytes=%u", status_code, (unsigned)rx.len);

    if (ret == ESP_OK && status_code >= 200 && status_code < 300 && rx.len >= sizeof(int16_t)) {
        ret = app_voice_output_play_pcm((const int16_t *)rx.buf,
                                        rx.len / sizeof(int16_t),
                                        CONFIG_APP_CLOUD_TTS_SAMPLE_RATE);
        set_status(ret == ESP_OK ? "TTS: played" : "TTS: play failed");
    } else {
        ESP_LOGW(TAG, "TTS request failed status=%d err=%s bytes=%u",
                 status_code, esp_err_to_name(ret), (unsigned)rx.len);
        if (rx.len > 0) {
            ESP_LOGW(TAG, "TTS error body: %.*s", (int)rx.len, (const char *)rx.buf);
        }
        set_status("TTS: request failed");
        ret = ret == ESP_OK ? ESP_FAIL : ret;
    }

    esp_http_client_cleanup(client);
    free(payload);
    heap_caps_free(rx_buf);
    return ret;
#else
    (void)text;
    set_status("TTS: disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *app_cloud_tts_status_text(void)
{
    return s_status;
}
