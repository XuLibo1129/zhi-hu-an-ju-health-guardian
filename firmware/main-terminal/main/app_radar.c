#include "app_radar.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_APP_RADAR_ENABLE
#define CONFIG_APP_RADAR_ENABLE 1
#endif

#ifndef CONFIG_APP_RADAR_UART_NUM
#define CONFIG_APP_RADAR_UART_NUM 1
#endif

#ifndef CONFIG_APP_RADAR_UART_TX_GPIO
#define CONFIG_APP_RADAR_UART_TX_GPIO 23
#endif

#ifndef CONFIG_APP_RADAR_UART_RX_GPIO
#define CONFIG_APP_RADAR_UART_RX_GPIO 22
#endif

#ifndef CONFIG_APP_RADAR_UART_BAUD
#define CONFIG_APP_RADAR_UART_BAUD 115200
#endif

#ifndef CONFIG_APP_RADAR_QUERY_ENABLE
#define CONFIG_APP_RADAR_QUERY_ENABLE 1
#endif

#define RADAR_RX_BUF_SIZE 4096
#define RADAR_TX_BUF_SIZE 512
#define RADAR_MAX_PAYLOAD_LEN 1024

static const char *TAG = "app_radar";

typedef struct {
    uint16_t id;
    uint16_t type;
    uint16_t len;
    uint8_t data[RADAR_MAX_PAYLOAD_LEN];
} tf_frame_t;

typedef enum {
    TF_WAIT_SOF = 0,
    TF_READ_HEADER,
    TF_READ_PAYLOAD,
    TF_READ_DATA_CKSUM,
} tf_parse_state_t;

typedef struct {
    tf_parse_state_t state;
    uint8_t header[7];
    size_t header_pos;
    uint16_t id;
    uint16_t len;
    uint16_t type;
    uint8_t data[RADAR_MAX_PAYLOAD_LEN];
    size_t data_pos;
} tf_parser_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_radar_sample_t s_latest;
static bool s_started;
static uint16_t s_next_tx_id = 1;
static uint32_t s_header_errors;
static uint32_t s_data_errors;
static uint32_t s_long_errors;

static uart_port_t radar_uart_port(void)
{
    return (uart_port_t)CONFIG_APP_RADAR_UART_NUM;
}

static uint8_t tf_cksum(const uint8_t *data, size_t len)
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

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t le_i32(const uint8_t *p)
{
    return (int32_t)le_u32(p);
}

static float le_float(const uint8_t *p)
{
    uint32_t raw = le_u32(p);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void tf_parser_reset(tf_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = TF_WAIT_SOF;
}

static void update_common(uint32_t bytes_delta, uint32_t frame_delta)
{
    portENTER_CRITICAL(&s_lock);
    s_latest.bytes_total += bytes_delta;
    s_latest.frame_count += frame_delta;
    s_latest.error_count = s_header_errors + s_data_errors + s_long_errors;
    if (frame_delta > 0) {
        s_latest.last_frame_ms = esp_timer_get_time() / 1000;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void handle_tf_frame(const tf_frame_t *frame)
{
    update_common(0, 1);

    switch (frame->type) {
    case 0xFFFF:
        ESP_LOGI(TAG, "firmware/status frame id=0x%04X len=%u", frame->id, frame->len);
        break;

    case 0x0F09:
        if (frame->len >= 1) {
            portENTER_CRITICAL(&s_lock);
            s_latest.has_human = true;
            s_latest.human_present = frame->data[0] != 0;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "human=%d", frame->data[0] != 0);
        }
        break;

    case 0x0A04:
        if (frame->len >= 4) {
            ESP_LOGI(TAG, "target position target_num=%" PRId32, le_i32(frame->data));
        }
        break;

    case 0x0A14:
        if (frame->len >= 4) {
            float breath = le_float(frame->data);
            int64_t now_ms = esp_timer_get_time() / 1000;
            portENTER_CRITICAL(&s_lock);
            s_latest.has_breath_bpm = true;
            s_latest.breath_bpm = breath;
            s_latest.last_breath_ms = now_ms;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "breath=%.1f bpm", breath);
        }
        break;

    case 0x0A15:
        if (frame->len >= 4) {
            float heart = le_float(frame->data);
            int64_t now_ms = esp_timer_get_time() / 1000;
            portENTER_CRITICAL(&s_lock);
            s_latest.has_heart_bpm = true;
            s_latest.heart_bpm = heart;
            s_latest.last_heart_ms = now_ms;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "heart=%.1f bpm", heart);
        }
        break;

    case 0x0A16:
        if (frame->len >= 8) {
            float range = le_float(&frame->data[4]);
            portENTER_CRITICAL(&s_lock);
            s_latest.has_range_m = true;
            s_latest.range_m = range;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "range=%.3f m", range);
        } else if (frame->len >= 4) {
            float range = le_float(frame->data);
            portENTER_CRITICAL(&s_lock);
            s_latest.has_range_m = true;
            s_latest.range_m = range;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "range=%.3f m", range);
        }
        break;

    case 0x0E02:
        if (frame->len >= 1) {
            portENTER_CRITICAL(&s_lock);
            s_latest.has_fall = true;
            s_latest.fall_detected = frame->data[0] != 0;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG, "fall=%d", frame->data[0] != 0);
        }
        break;

    default:
        ESP_LOGD(TAG, "frame type=0x%04X len=%u", frame->type, frame->len);
        break;
    }
}

static bool tf_parser_feed(tf_parser_t *parser, uint8_t byte, tf_frame_t *out)
{
    switch (parser->state) {
    case TF_WAIT_SOF:
        if (byte == 0x01) {
            tf_parser_reset(parser);
            parser->state = TF_READ_HEADER;
        }
        return false;

    case TF_READ_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos < sizeof(parser->header)) {
            return false;
        }

        {
            uint8_t header_bytes[7] = {
                0x01,
                parser->header[0], parser->header[1],
                parser->header[2], parser->header[3],
                parser->header[4], parser->header[5],
            };
            if (parser->header[6] != tf_cksum(header_bytes, sizeof(header_bytes))) {
                ++s_header_errors;
                tf_parser_reset(parser);
                return false;
            }
        }

        parser->id = be16(&parser->header[0]);
        parser->len = be16(&parser->header[2]);
        parser->type = be16(&parser->header[4]);
        if (parser->len > RADAR_MAX_PAYLOAD_LEN) {
            ++s_long_errors;
            tf_parser_reset(parser);
            return false;
        }

        if (parser->len == 0) {
            out->id = parser->id;
            out->type = parser->type;
            out->len = 0;
            tf_parser_reset(parser);
            return true;
        }

        parser->data_pos = 0;
        parser->state = TF_READ_PAYLOAD;
        return false;

    case TF_READ_PAYLOAD:
        parser->data[parser->data_pos++] = byte;
        if (parser->data_pos >= parser->len) {
            parser->state = TF_READ_DATA_CKSUM;
        }
        return false;

    case TF_READ_DATA_CKSUM:
        if (byte != tf_cksum(parser->data, parser->len)) {
            ++s_data_errors;
            tf_parser_reset(parser);
            return false;
        }

        out->id = parser->id;
        out->type = parser->type;
        out->len = parser->len;
        memcpy(out->data, parser->data, parser->len);
        tf_parser_reset(parser);
        return true;
    }

    tf_parser_reset(parser);
    return false;
}

static size_t tf_build_frame(uint16_t id, uint16_t type, const uint8_t *data, uint16_t len,
                             uint8_t *out, size_t out_size)
{
    size_t need = 1 + 2 + 2 + 2 + 1 + len + (len ? 1 : 0);
    if (out_size < need) {
        return 0;
    }

    size_t pos = 0;
    out[pos++] = 0x01;
    put_be16(&out[pos], id);
    pos += 2;
    put_be16(&out[pos], len);
    pos += 2;
    put_be16(&out[pos], type);
    pos += 2;
    out[pos] = tf_cksum(out, pos);
    pos += 1;

    if (len > 0) {
        memcpy(&out[pos], data, len);
        pos += len;
        out[pos++] = tf_cksum(data, len);
    }

    return pos;
}

static esp_err_t radar_send_tf(uint16_t type, const uint8_t *data, uint16_t len)
{
    uint8_t frame[1 + 2 + 2 + 2 + 1 + RADAR_MAX_PAYLOAD_LEN + 1];
    uint16_t id = s_next_tx_id++;
    if (s_next_tx_id == 0) {
        s_next_tx_id = 1;
    }

    size_t frame_len = tf_build_frame(id, type, data, len, frame, sizeof(frame));
    if (frame_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = uart_write_bytes(radar_uart_port(), frame, frame_len);
    return written == (int)frame_len ? ESP_OK : ESP_FAIL;
}

static void radar_rx_task(void *arg)
{
    (void)arg;

    tf_parser_t parser;
    tf_parser_reset(&parser);

    uint8_t rx_buf[256];
    uint32_t bytes_this_period = 0;
    uint32_t last_frame_count = 0;
    TickType_t last_log_tick = xTaskGetTickCount();

    while (true) {
        int len = uart_read_bytes(radar_uart_port(), rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(200));
        if (len > 0) {
            bytes_this_period += (uint32_t)len;
            update_common((uint32_t)len, 0);
            for (int i = 0; i < len; ++i) {
                tf_frame_t frame;
                if (tf_parser_feed(&parser, rx_buf[i], &frame)) {
                    handle_tf_frame(&frame);
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_log_tick >= pdMS_TO_TICKS(5000)) {
            app_radar_sample_t sample;
            app_radar_get_latest(&sample);
            ESP_LOGI(TAG, "diag bytes=%" PRIu32 "/5s frames=%" PRIu32 "/5s total=%" PRIu32 " err=%" PRIu32,
                     bytes_this_period, sample.frame_count - last_frame_count,
                     sample.frame_count, sample.error_count);
            bytes_this_period = 0;
            last_frame_count = sample.frame_count;
            last_log_tick = now;
        }
    }
}

static void radar_query_task(void *arg)
{
    (void)arg;

#if CONFIG_APP_RADAR_QUERY_ENABLE
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK_WITHOUT_ABORT(radar_send_tf(0xFFFF, NULL, 0));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_ERROR_CHECK_WITHOUT_ABORT(radar_send_tf(0xFFFF, NULL, 0));
    }
#else
    vTaskDelete(NULL);
#endif
}

esp_err_t app_radar_start(void)
{
#if !CONFIG_APP_RADAR_ENABLE
    ESP_LOGI(TAG, "radar disabled by config");
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    uart_config_t config = {
        .baud_rate = CONFIG_APP_RADAR_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(radar_uart_port(), RADAR_RX_BUF_SIZE,
                                        RADAR_TX_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(radar_uart_port(), &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(radar_uart_port(), CONFIG_APP_RADAR_UART_TX_GPIO,
                       CONFIG_APP_RADAR_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(CONFIG_APP_RADAR_UART_RX_GPIO, GPIO_PULLUP_ONLY));

    BaseType_t ok = xTaskCreate(radar_rx_task, "radar_rx", 8192, NULL, 9, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(radar_query_task, "radar_query", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "radar started UART=%d baud=%d TX=%d RX=%d",
             CONFIG_APP_RADAR_UART_NUM, CONFIG_APP_RADAR_UART_BAUD,
             CONFIG_APP_RADAR_UART_TX_GPIO, CONFIG_APP_RADAR_UART_RX_GPIO);
    return ESP_OK;
#endif
}

void app_radar_get_latest(app_radar_sample_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_lock);
}

const char *app_radar_status_text(void)
{
    static char text[128];
    app_radar_sample_t sample;
    app_radar_get_latest(&sample);

    if (sample.frame_count == 0) {
        snprintf(text, sizeof(text), "RADAR -- waiting  bytes=%" PRIu32 " err=%" PRIu32,
                 sample.bytes_total, sample.error_count);
        return text;
    }

    char breath[16] = "--";
    char heart[16] = "--";
    char range[16] = "--";
    if (sample.has_breath_bpm) {
        snprintf(breath, sizeof(breath), "%.0f", sample.breath_bpm);
    }
    if (sample.has_heart_bpm) {
        snprintf(heart, sizeof(heart), "%.0f", sample.heart_bpm);
    }
    if (sample.has_range_m) {
        snprintf(range, sizeof(range), "%.2fm", sample.range_m);
    }

    snprintf(text, sizeof(text), "RADAR H:%s B:%s R:%s F:%s frames=%" PRIu32,
             heart, breath, range,
             sample.has_fall ? (sample.fall_detected ? "YES" : "NO") : "--",
             sample.frame_count);
    return text;
}
