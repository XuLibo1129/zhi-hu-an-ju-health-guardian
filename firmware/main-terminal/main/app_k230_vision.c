#include "app_k230_vision.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_APP_K230_VISION_ENABLE
#define CONFIG_APP_K230_VISION_ENABLE 1
#endif

#ifndef CONFIG_APP_K230_VISION_UART_NUM
#define CONFIG_APP_K230_VISION_UART_NUM 3
#endif

#ifndef CONFIG_APP_K230_VISION_UART_RX_GPIO
#define CONFIG_APP_K230_VISION_UART_RX_GPIO 20
#endif

#ifndef CONFIG_APP_K230_VISION_UART_BAUD
#define CONFIG_APP_K230_VISION_UART_BAUD 115200
#endif

#define K230_RX_BUF_SIZE 1024
#define K230_LINE_MAX_LEN 256
#define K230_SCAN_WINDOW_LEN 48
#define K230_ERROR_LOG_INTERVAL_MS 2000

static const char *TAG = "app_k230_vision";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_k230_vision_sample_t s_latest;
static bool s_started;
static int64_t s_last_error_log_ms;

static uart_port_t k230_uart_port(void)
{
    return (uart_port_t)CONFIG_APP_K230_VISION_UART_NUM;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_status(bool valid, bool fall_detected, const char *line, bool error)
{
    portENTER_CRITICAL(&s_lock);
    s_latest.valid = valid;
    s_latest.fall_detected = fall_detected;
    s_latest.last_update_ms = now_ms();
    if (valid) {
        s_latest.frame_count++;
    }
    if (error) {
        s_latest.error_count++;
    }
    if (line != NULL) {
        snprintf(s_latest.last_line, sizeof(s_latest.last_line), "%s", line);
    }
    portEXIT_CRITICAL(&s_lock);
}

static void record_error(const char *line)
{
    portENTER_CRITICAL(&s_lock);
    s_latest.error_count++;
    if (line != NULL) {
        snprintf(s_latest.last_line, sizeof(s_latest.last_line), "%s", line);
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool is_normal_token(const char *line)
{
    return strcasecmp(line, "K230,NOFALL") == 0 ||
           strcasecmp(line, "NOFALL") == 0 ||
           strcasecmp(line, "K230,NORMAL") == 0 ||
           strcasecmp(line, "NORMAL") == 0 ||
           strcmp(line, "0") == 0;
}

static bool is_fall_token(const char *line)
{
    return strcasecmp(line, "K230,FALL") == 0 ||
           strcasecmp(line, "FALL") == 0 ||
           strcmp(line, "1") == 0;
}

static bool parse_line(char *line)
{
    if (line == NULL || line[0] == '\0') {
        return false;
    }

    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == '\n' ||
                          end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    if (is_normal_token(line)) {
        set_status(true, false, line, false);
        ESP_LOGI(TAG, "K230 vision normal: %s", line);
        return true;
    }

    if (is_fall_token(line)) {
        set_status(true, true, line, false);
        ESP_LOGW(TAG, "K230 vision fall detected: %s", line);
        return true;
    }

    record_error(line);
    ESP_LOGW(TAG, "unknown K230 line: %s", line);
    return false;
}

static char ascii_upper(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - 'a' + 'A');
    }
    return ch;
}

static bool contains_text(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static bool scan_stream_marker(char *window, size_t *window_len, char ch)
{
    if (window == NULL || window_len == NULL) {
        return false;
    }

    char normalized = ascii_upper(ch);
    bool keep = (normalized >= 'A' && normalized <= 'Z') ||
                (normalized >= '0' && normalized <= '9') ||
                normalized == ',' || normalized == ':' || normalized == '_' ||
                normalized == '-';
    if (!keep) {
        return false;
    }

    if (*window_len + 1 >= K230_SCAN_WINDOW_LEN) {
        memmove(window, window + 1, K230_SCAN_WINDOW_LEN - 2);
        *window_len = K230_SCAN_WINDOW_LEN - 2;
    }
    window[(*window_len)++] = normalized;
    window[*window_len] = '\0';

    if (contains_text(window, "K230,NOFALL") ||
        contains_text(window, "K230,NORMAL") ||
        contains_text(window, "K230,0")) {
        set_status(true, false, "stream:K230,NOFALL", false);
        ESP_LOGI(TAG, "K230 vision normal from stream");
        *window_len = 0;
        window[0] = '\0';
        return true;
    }

    if (contains_text(window, "K230,FALL") ||
        contains_text(window, "K230,1")) {
        set_status(true, true, "stream:K230,FALL", false);
        ESP_LOGW(TAG, "K230 vision fall from stream");
        *window_len = 0;
        window[0] = '\0';
        return true;
    }

    return false;
}

static void log_long_line_once(const char *sample)
{
    int64_t now = now_ms();
    if (now - s_last_error_log_ms < K230_ERROR_LOG_INTERVAL_MS) {
        return;
    }
    s_last_error_log_ms = now;
    ESP_LOGW(TAG, "K230 line too long, scanning stream sample=\"%s\"", sample ? sample : "");
}

static void k230_rx_task(void *arg)
{
    (void)arg;

    uint8_t rx[128];
    char line[K230_LINE_MAX_LEN];
    char scan_window[K230_SCAN_WINDOW_LEN] = {0};
    size_t line_len = 0;
    size_t scan_len = 0;

    while (true) {
        int n = uart_read_bytes(k230_uart_port(), rx, sizeof(rx), pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; ++i) {
            char ch = (char)rx[i];
            scan_stream_marker(scan_window, &scan_len, ch);
            if (ch == '\n') {
                line[line_len] = '\0';
                parse_line(line);
                line_len = 0;
                continue;
            }

            if (ch == '\r') {
                continue;
            }

            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                char sample[48] = {0};
                size_t copy_len = line_len < sizeof(sample) - 1 ? line_len : sizeof(sample) - 1;
                memcpy(sample, line, copy_len);
                sample[copy_len] = '\0';
                line_len = 0;
                record_error("line too long");
                log_long_line_once(sample);
            }
        }
    }
}

esp_err_t app_k230_vision_start(void)
{
#if !CONFIG_APP_K230_VISION_ENABLE
    ESP_LOGI(TAG, "K230 vision receiver disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_started) {
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_APP_K230_VISION_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(k230_uart_port(), K230_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(k230_uart_port(), &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(k230_uart_port(),
                       UART_PIN_NO_CHANGE,
                       CONFIG_APP_K230_VISION_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(CONFIG_APP_K230_VISION_UART_RX_GPIO, GPIO_PULLUP_ONLY));

    uart_flush_input(k230_uart_port());
    set_status(false, false, "waiting", false);

    BaseType_t ok = xTaskCreate(k230_rx_task, "k230_rx", 4096, NULL, 6, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "K230 vision UART started: UART=%d RX_GPIO=%d baud=%d",
             CONFIG_APP_K230_VISION_UART_NUM,
             CONFIG_APP_K230_VISION_UART_RX_GPIO,
             CONFIG_APP_K230_VISION_UART_BAUD);
    return ESP_OK;
#endif
}

void app_k230_vision_get_latest(app_k230_vision_sample_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_lock);
}

const char *app_k230_vision_status_text(void)
{
    static char status[64];
    app_k230_vision_sample_t sample = {0};
    app_k230_vision_get_latest(&sample);

    if (!sample.valid) {
        snprintf(status, sizeof(status), "waiting");
    } else if (sample.fall_detected) {
        snprintf(status, sizeof(status), "fall");
    } else {
        snprintf(status, sizeof(status), "normal");
    }

    return status;
}
