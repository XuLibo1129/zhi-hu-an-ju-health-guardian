#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifndef CONFIG_APP_WIFI_SSID
#define CONFIG_APP_WIFI_SSID ""
#endif

#ifndef CONFIG_APP_WIFI_PASSWORD
#define CONFIG_APP_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_APP_MAIN_TERMINAL_URL
#define CONFIG_APP_MAIN_TERMINAL_URL ""
#endif

#ifndef CONFIG_APP_NODE_ID
#define CONFIG_APP_NODE_ID "ld6002c_01"
#endif

#ifndef CONFIG_APP_LD6002C_UART_NUM
#define CONFIG_APP_LD6002C_UART_NUM 1
#endif

#ifndef CONFIG_APP_LD6002C_UART_TX_GPIO
#define CONFIG_APP_LD6002C_UART_TX_GPIO 23
#endif

#ifndef CONFIG_APP_LD6002C_UART_RX_GPIO
#define CONFIG_APP_LD6002C_UART_RX_GPIO 22
#endif

#ifndef CONFIG_APP_LD6002C_UART_BAUD
#define CONFIG_APP_LD6002C_UART_BAUD 115200
#endif

#ifndef CONFIG_APP_LD6002C_TX2_GPIO
#define CONFIG_APP_LD6002C_TX2_GPIO -1
#endif

#ifndef CONFIG_APP_REPORT_INTERVAL_MS
#define CONFIG_APP_REPORT_INTERVAL_MS 2000
#endif

#ifndef CONFIG_APP_HTTP_TIMEOUT_MS
#define CONFIG_APP_HTTP_TIMEOUT_MS 2000
#endif

#ifndef CONFIG_APP_LD6002C_QUERY_FIRMWARE_ON_BOOT
#define CONFIG_APP_LD6002C_QUERY_FIRMWARE_ON_BOOT 1
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define LD6002C_SOF              0x01
#define LD6002C_MAX_PAYLOAD      256
#define LD6002C_RX_BUF_SIZE      2048
#define LD6002C_TYPE_FIRMWARE    0xFFFF
#define LD6002C_TYPE_FALL_STATUS 0x0E02

static const char *TAG = "ld6002c_node";

typedef struct {
    bool valid;
    bool fall_detected;
    bool uart_valid;
    bool uart_fall;
    bool tx2_valid;
    bool tx2_fall;
    uint32_t frame_count;
    uint32_t fall_count;
    uint32_t header_error_count;
    uint32_t data_error_count;
    int64_t last_update_ms;
    char source[12];
} app_state_t;

typedef enum {
    TF_WAIT_SOF = 0,
    TF_READ_HEADER,
    TF_READ_DATA,
} tf_state_t;

typedef struct {
    tf_state_t state;
    uint8_t header[8];
    size_t header_pos;
    uint16_t payload_len;
    uint16_t type;
    uint8_t payload[LD6002C_MAX_PAYLOAD + 1];
    size_t payload_pos;
} tf_parser_t;

static EventGroupHandle_t s_wifi_event_group;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static app_state_t s_state;
static volatile bool s_wifi_ready;
static int s_wifi_retry_count;
static uint16_t s_tx_id = 1;

static uart_port_t radar_uart(void)
{
    return (uart_port_t)CONFIG_APP_LD6002C_UART_NUM;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint8_t tf_checksum(const uint8_t *data, size_t len)
{
    uint8_t ret = 0;
    for (size_t i = 0; i < len; ++i) {
        ret ^= data[i];
    }
    return (uint8_t)~ret;
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void refresh_overall_locked(const char *source)
{
    bool fall = (s_state.uart_valid && s_state.uart_fall) ||
                (s_state.tx2_valid && s_state.tx2_fall);

    if (fall && !s_state.fall_detected) {
        s_state.fall_count++;
    }

    s_state.valid = s_state.uart_valid || s_state.tx2_valid;
    s_state.fall_detected = fall;
    s_state.last_update_ms = now_ms();
    snprintf(s_state.source, sizeof(s_state.source), "%s", source ? source : "-");
}

static void update_uart_fall(bool fall)
{
    bool first_report;
    bool changed;
    bool previous;

    portENTER_CRITICAL(&s_state_lock);
    first_report = !s_state.uart_valid;
    previous = s_state.uart_fall;
    changed = first_report || previous != fall;
    s_state.uart_valid = true;
    s_state.uart_fall = fall;
    s_state.frame_count++;
    refresh_overall_locked("uart");
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        if (first_report) {
            ESP_LOGI(TAG, "LD6002C UART initial fall=%d", fall);
        } else {
            ESP_LOGW(TAG, "LD6002C UART fall changed: %d -> %d", previous, fall);
        }
    }
}

#if CONFIG_APP_LD6002C_TX2_GPIO >= 0
static void update_tx2_fall(bool fall)
{
    bool first_report;
    bool changed;
    bool previous;

    portENTER_CRITICAL(&s_state_lock);
    first_report = !s_state.tx2_valid;
    previous = s_state.tx2_fall;
    changed = first_report || previous != fall;
    s_state.tx2_valid = true;
    s_state.tx2_fall = fall;
    refresh_overall_locked("tx2");
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        if (first_report) {
            ESP_LOGI(TAG, "LD6002C TX2 initial fall=%d", fall);
        } else {
            ESP_LOGW(TAG, "LD6002C TX2 fall changed: %d -> %d", previous, fall);
        }
    }
}
#endif

static void count_header_error(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.header_error_count++;
    portEXIT_CRITICAL(&s_state_lock);
}

static void count_data_error(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.data_error_count++;
    portEXIT_CRITICAL(&s_state_lock);
}

static app_state_t get_state_snapshot(void)
{
    app_state_t copy;
    portENTER_CRITICAL(&s_state_lock);
    copy = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return copy;
}

static void dispatch_frame(uint16_t type, const uint8_t *payload, uint16_t len)
{
    if (type == LD6002C_TYPE_FALL_STATUS) {
        if (len < 1 || payload == NULL) {
            ESP_LOGW(TAG, "fall status frame too short: len=%u", len);
            return;
        }
        update_uart_fall(payload[0] != 0);
        return;
    }

    if (type == LD6002C_TYPE_FIRMWARE) {
        ESP_LOGI(TAG, "firmware response len=%u", len);
        return;
    }

    ESP_LOGD(TAG, "frame type=0x%04X len=%u", type, len);
}

static void parser_reset(tf_parser_t *parser)
{
    parser->state = TF_WAIT_SOF;
    parser->header_pos = 0;
    parser->payload_len = 0;
    parser->type = 0;
    parser->payload_pos = 0;
}

static void parser_feed(tf_parser_t *parser, uint8_t byte)
{
    switch (parser->state) {
    case TF_WAIT_SOF:
        if (byte == LD6002C_SOF) {
            parser->header[0] = byte;
            parser->header_pos = 1;
            parser->state = TF_READ_HEADER;
        }
        break;

    case TF_READ_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos < sizeof(parser->header)) {
            break;
        }

        if (tf_checksum(parser->header, 7) != parser->header[7]) {
            count_header_error();
            ESP_LOGW(TAG, "header checksum mismatch");
            parser_reset(parser);
            if (byte == LD6002C_SOF) {
                parser_feed(parser, byte);
            }
            break;
        }

        parser->payload_len = be16(&parser->header[3]);
        parser->type = be16(&parser->header[5]);
        if (parser->payload_len > LD6002C_MAX_PAYLOAD) {
            count_data_error();
            ESP_LOGW(TAG, "payload too long: %u", parser->payload_len);
            parser_reset(parser);
            break;
        }

        if (parser->payload_len == 0) {
            dispatch_frame(parser->type, NULL, 0);
            parser_reset(parser);
        } else {
            parser->payload_pos = 0;
            parser->state = TF_READ_DATA;
        }
        break;

    case TF_READ_DATA:
        parser->payload[parser->payload_pos++] = byte;
        if (parser->payload_pos < (size_t)parser->payload_len + 1) {
            break;
        }

        if (tf_checksum(parser->payload, parser->payload_len) !=
            parser->payload[parser->payload_len]) {
            count_data_error();
            ESP_LOGW(TAG, "data checksum mismatch type=0x%04X len=%u",
                     parser->type, parser->payload_len);
        } else {
            dispatch_frame(parser->type, parser->payload, parser->payload_len);
        }
        parser_reset(parser);
        break;
    }
}

static esp_err_t tf_send(uint16_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[8 + LD6002C_MAX_PAYLOAD + 1];
    if (len > LD6002C_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[0] = LD6002C_SOF;
    put_be16(&frame[1], s_tx_id++);
    put_be16(&frame[3], len);
    put_be16(&frame[5], type);
    frame[7] = tf_checksum(frame, 7);

    size_t frame_len = 8;
    if (len > 0) {
        memcpy(&frame[8], payload, len);
        frame[8 + len] = tf_checksum(payload, len);
        frame_len += (size_t)len + 1;
    }

    int written = uart_write_bytes(radar_uart(), frame, frame_len);
    if (written != (int)frame_len) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TX type=0x%04X len=%u", type, len);
    return ESP_OK;
}

static esp_err_t radar_start(void)
{
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_APP_LD6002C_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(radar_uart(), LD6002C_RX_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(radar_uart(), &uart_config),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(radar_uart(),
                                     CONFIG_APP_LD6002C_UART_TX_GPIO,
                                     CONFIG_APP_LD6002C_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    uart_flush_input(radar_uart());

    ESP_LOGI(TAG, "LD6002C UART started: UART=%d baud=%d TX=%d RX=%d",
             CONFIG_APP_LD6002C_UART_NUM,
             CONFIG_APP_LD6002C_UART_BAUD,
             CONFIG_APP_LD6002C_UART_TX_GPIO,
             CONFIG_APP_LD6002C_UART_RX_GPIO);
    return ESP_OK;
}

static void radar_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    tf_parser_t parser = {0};
    parser_reset(&parser);

    while (true) {
        int len = uart_read_bytes(radar_uart(), buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len; ++i) {
            parser_feed(&parser, buf[i]);
        }
    }
}

static void tx2_poll_task(void *arg)
{
    (void)arg;
#if CONFIG_APP_LD6002C_TX2_GPIO >= 0
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_APP_LD6002C_TX2_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int last_level = -1;
    while (true) {
        int level = gpio_get_level(CONFIG_APP_LD6002C_TX2_GPIO);
        if (level != last_level) {
            last_level = level;
            update_tx2_fall(level != 0);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#else
    ESP_LOGI(TAG, "LD6002C TX2 GPIO disabled");
    vTaskDelete(NULL);
#endif
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ready = false;
        if (s_wifi_retry_count < 100) {
            esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi reconnecting, retry=%d", s_wifi_retry_count);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        s_wifi_ready = true;
        ESP_LOGI(TAG, "Wi-Fi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_event_loop_create_default failed");
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "register IP_EVENT failed");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "Wi-Fi connecting to %s", CONFIG_APP_WIFI_SSID);
    return ESP_OK;
}

static esp_err_t post_state(const app_state_t *state, bool log_success)
{
    if (state == NULL || strlen(CONFIG_APP_MAIN_TERMINAL_URL) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[512];
    const char *state_text = state->fall_detected ? "fall" : "normal";
    float prob = state->fall_detected ? 1.0f : 0.0f;
    int len = snprintf(payload, sizeof(payload),
                       "{"
                       "\"sensor\":\"ld6002c_fall_radar\","
                       "\"node_id\":\"%s\","
                       "\"time_ms\":%" PRId64 ","
                       "\"alarm\":%s,"
                       "\"fall_detected\":%s,"
                       "\"state\":\"%s\","
                       "\"source\":\"%s\","
                       "\"fall_probability\":%.1f,"
                       "\"smooth_probability\":%.1f,"
                       "\"threshold\":0.5,"
                       "\"effective_threshold\":0.5,"
                       "\"motion_ok\":true,"
                       "\"samples\":%" PRIu32 ","
                       "\"frames\":%" PRIu32 ","
                       "\"falls\":%" PRIu32 ","
                       "\"header_errors\":%" PRIu32 ","
                       "\"data_errors\":%" PRIu32
                       "}",
                       CONFIG_APP_NODE_ID,
                       now_ms(),
                       state->fall_detected ? "true" : "false",
                       state->fall_detected ? "true" : "false",
                       state_text,
                       state->source[0] ? state->source : "-",
                       prob,
                       prob,
                       state->frame_count,
                       state->frame_count,
                       state->fall_count,
                       state->header_error_count,
                       state->data_error_count);

    if (len <= 0 || len >= (int)sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_APP_MAIN_TERMINAL_URL,
        .timeout_ms = CONFIG_APP_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, len);
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && status >= 200 && status < 300) {
        if (log_success) {
            ESP_LOGI(TAG, "POST ok fall=%d frames=%" PRIu32, state->fall_detected, state->frame_count);
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "POST failed ret=%s status=%d url=%s",
             esp_err_to_name(ret), status, CONFIG_APP_MAIN_TERMINAL_URL);
    return ret == ESP_OK ? ESP_FAIL : ret;
}

static void report_task(void *arg)
{
    (void)arg;
    bool last_fall = false;
    bool last_valid = false;
    int64_t last_post_ms = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));

        if (!s_wifi_ready) {
            continue;
        }

        app_state_t state = get_state_snapshot();
        if (!state.valid) {
            continue;
        }

        int64_t now = now_ms();
        bool changed = !last_valid || state.fall_detected != last_fall;
        bool interval_due = last_post_ms == 0 ||
                            now - last_post_ms >= CONFIG_APP_REPORT_INTERVAL_MS;
        if (!changed && !interval_due) {
            continue;
        }

        esp_err_t ret = post_state(&state, changed || state.fall_detected);
        if (ret == ESP_OK) {
            last_post_ms = now;
            last_fall = state.fall_detected;
            last_valid = true;
        }
    }
}

static void status_task(void *arg)
{
    (void)arg;
    while (true) {
        app_state_t state = get_state_snapshot();
        ESP_LOGI(TAG,
                 "STAT wifi=%d valid=%d fall=%d source=%s frames=%" PRIu32
                 " falls=%" PRIu32 " hdr_err=%" PRIu32 " data_err=%" PRIu32,
                 s_wifi_ready,
                 state.valid,
                 state.fall_detected,
                 state.source[0] ? state.source : "-",
                 state.frame_count,
                 state.fall_count,
                 state.header_error_count,
                 state.data_error_count);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "LD6002C fall node starting");
    ESP_LOGI(TAG, "Target URL: %s", CONFIG_APP_MAIN_TERMINAL_URL);

    ESP_ERROR_CHECK(radar_start());
    ESP_ERROR_CHECK(wifi_start());

    xTaskCreate(radar_rx_task, "radar_rx", 4096, NULL, 8, NULL);
    xTaskCreate(tx2_poll_task, "tx2_poll", 2048, NULL, 5, NULL);
    xTaskCreate(report_task, "report", 6144, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);

#if CONFIG_APP_LD6002C_QUERY_FIRMWARE_ON_BOOT
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_err_t ret = tf_send(LD6002C_TYPE_FIRMWARE, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "firmware query failed: %s", esp_err_to_name(ret));
    }
#endif
}
