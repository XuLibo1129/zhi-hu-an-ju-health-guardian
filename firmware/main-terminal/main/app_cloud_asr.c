#include "app_cloud_asr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_network.h"

static const char *TAG = "app_cloud_asr";

#ifndef CONFIG_APP_CLOUD_ASR_URL
#define CONFIG_APP_CLOUD_ASR_URL ""
#endif
#ifndef CONFIG_APP_CLOUD_ASR_API_KEY
#define CONFIG_APP_CLOUD_ASR_API_KEY ""
#endif
#ifndef CONFIG_APP_CLOUD_ASR_MODEL
#define CONFIG_APP_CLOUD_ASR_MODEL ""
#endif
#ifndef CONFIG_APP_CLOUD_ASR_LANGUAGE
#define CONFIG_APP_CLOUD_ASR_LANGUAGE ""
#endif
#ifndef CONFIG_APP_CLOUD_ASR_TIMEOUT_MS
#define CONFIG_APP_CLOUD_ASR_TIMEOUT_MS 30000
#endif

#define ASR_RX_BUFFER_SIZE 4096
#define WAV_HEADER_BYTES 44

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_rx_t;

static char s_status[96] = "ASR: idle";

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

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void write_wav_header(uint8_t *wav, uint32_t pcm_bytes, uint32_t sample_rate)
{
    memcpy(wav + 0, "RIFF", 4);
    put_le32(wav + 4, 36 + pcm_bytes);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put_le32(wav + 16, 16);
    put_le16(wav + 20, 1);
    put_le16(wav + 22, 1);
    put_le32(wav + 24, sample_rate);
    put_le32(wav + 28, sample_rate * 2);
    put_le16(wav + 32, 2);
    put_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put_le32(wav + 40, pcm_bytes);
}

static esp_err_t parse_transcript(const char *json, char *text, size_t text_size)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(text, item->valuestring, text_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static void *caps_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

esp_err_t app_cloud_asr_transcribe_pcm(const int16_t *pcm,
                                       size_t samples,
                                       uint32_t sample_rate,
                                       char *text,
                                       size_t text_size)
{
#if CONFIG_APP_CLOUD_ASR_ENABLE
    if (pcm == NULL || samples == 0 || text == NULL || text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(CONFIG_APP_CLOUD_ASR_URL) == 0 ||
        strlen(CONFIG_APP_CLOUD_ASR_API_KEY) == 0 ||
        strlen(CONFIG_APP_CLOUD_ASR_MODEL) == 0) {
        set_status("ASR: not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_network_is_ready()) {
        set_status("ASR: wait network");
        return ESP_ERR_INVALID_STATE;
    }

    const char *boundary = "----esp32p4-asr-boundary";
    char preamble[512];
    int pre_len = snprintf(preamble, sizeof(preamble),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           boundary,
                           CONFIG_APP_CLOUD_ASR_MODEL,
                           boundary,
                           CONFIG_APP_CLOUD_ASR_LANGUAGE,
                           boundary);
    if (pre_len <= 0 || pre_len >= (int)sizeof(preamble)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char epilogue[64];
    int epi_len = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", boundary);
    if (epi_len <= 0 || epi_len >= (int)sizeof(epilogue)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pcm_bytes = samples * sizeof(int16_t);
    const size_t body_len = (size_t)pre_len + WAV_HEADER_BYTES + pcm_bytes + (size_t)epi_len;
    uint8_t *body = caps_alloc(body_len);
    if (body == NULL) {
        ESP_LOGE(TAG, "ASR body alloc failed: %u bytes", (unsigned)body_len);
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    memcpy(body + off, preamble, pre_len);
    off += pre_len;
    write_wav_header(body + off, (uint32_t)pcm_bytes, sample_rate);
    off += WAV_HEADER_BYTES;
    memcpy(body + off, pcm, pcm_bytes);
    off += pcm_bytes;
    memcpy(body + off, epilogue, epi_len);

    char *rx_buf = calloc(1, ASR_RX_BUFFER_SIZE);
    if (rx_buf == NULL) {
        heap_caps_free(body);
        return ESP_ERR_NO_MEM;
    }

    http_rx_t rx = {
        .buf = rx_buf,
        .len = 0,
        .cap = ASR_RX_BUFFER_SIZE,
    };

    esp_http_client_config_t config = {
        .url = CONFIG_APP_CLOUD_ASR_URL,
        .event_handler = http_event_handler,
        .user_data = &rx,
        .timeout_ms = CONFIG_APP_CLOUD_ASR_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(body);
        free(rx_buf);
        return ESP_FAIL;
    }

    char auth[256];
    char content_type[96];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_APP_CLOUD_ASR_API_KEY);
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    set_status("ASR: requesting");
    esp_err_t ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "ASR HTTP status=%d rx_len=%u", status_code, (unsigned)rx.len);

    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = parse_transcript(rx.buf, text, text_size);
        set_status(ret == ESP_OK ? "ASR: ok" : "ASR: parse failed");
    } else {
        ESP_LOGW(TAG, "ASR request failed status=%d err=%s body=%s",
                 status_code, esp_err_to_name(ret), rx.buf);
        set_status("ASR: request failed");
        ret = ret == ESP_OK ? ESP_FAIL : ret;
    }

    esp_http_client_cleanup(client);
    heap_caps_free(body);
    free(rx_buf);
    return ret;
#else
    (void)pcm;
    (void)samples;
    (void)sample_rate;
    if (text != NULL && text_size > 0) {
        text[0] = '\0';
    }
    set_status("ASR: disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *app_cloud_asr_status_text(void)
{
    return s_status;
}
