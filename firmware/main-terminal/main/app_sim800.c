#include "app_sim800.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_APP_SIM800_ENABLE
#define CONFIG_APP_SIM800_ENABLE 0
#endif

#ifndef CONFIG_APP_SIM800_UART_NUM
#define CONFIG_APP_SIM800_UART_NUM 2
#endif

#ifndef CONFIG_APP_SIM800_UART_TX_GPIO
#define CONFIG_APP_SIM800_UART_TX_GPIO 28
#endif

#ifndef CONFIG_APP_SIM800_UART_RX_GPIO
#define CONFIG_APP_SIM800_UART_RX_GPIO 29
#endif

#ifndef CONFIG_APP_SIM800_UART_BAUD
#define CONFIG_APP_SIM800_UART_BAUD 115200
#endif

#ifndef CONFIG_APP_SIM800_PWRKEY_GPIO
#define CONFIG_APP_SIM800_PWRKEY_GPIO -1
#endif

#ifndef CONFIG_APP_SIM800_EMERGENCY_PHONE
#define CONFIG_APP_SIM800_EMERGENCY_PHONE ""
#endif

#ifndef CONFIG_APP_SIM800_EMERGENCY_SMS_TEXT
#define CONFIG_APP_SIM800_EMERGENCY_SMS_TEXT "Emergency alert from health terminal."
#endif

#ifndef CONFIG_APP_SIM800_AUTO_SMS_ENABLE
#define CONFIG_APP_SIM800_AUTO_SMS_ENABLE 0
#endif

#ifndef CONFIG_APP_SIM800_AUTO_CALL_ENABLE
#define CONFIG_APP_SIM800_AUTO_CALL_ENABLE 0
#endif

#define A7670_AUTO_SMS_CALL_GAP_MS 5000

#ifndef CONFIG_APP_SIM800_AUTO_COOLDOWN_SECONDS
#define CONFIG_APP_SIM800_AUTO_COOLDOWN_SECONDS 300
#endif

#define SIM800_RX_BUF_SIZE 2048
#define SIM800_TX_BUF_SIZE 512
#define SIM800_RESP_SIZE 768
#define SIM800_SMS_BODY_HEX_MAX 560
#define SIM800_SMS_NUMBER_HEX_MAX 64
#define SIM800_BAUD_SCAN_COUNT 7

static const char *TAG = "app_a7670";

static SemaphoreHandle_t s_cmd_lock;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_at_ready;
static bool s_sim_ready;
static bool s_registered;
static int s_rssi = 99;
#if CONFIG_APP_SIM800_AUTO_SMS_ENABLE || CONFIG_APP_SIM800_AUTO_CALL_ENABLE
static uint32_t s_last_auto_alert_ms;
#endif
static char s_status[128] = "A7670C waiting";

static uart_port_t sim_uart_port(void)
{
    return (uart_port_t)CONFIG_APP_SIM800_UART_NUM;
}

static void set_status(const char *fmt, ...)
{
    char tmp[sizeof(s_status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_state_lock);
    strlcpy(s_status, tmp, sizeof(s_status));
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_ready_flags(bool at_ready, bool sim_ready, bool registered, int rssi)
{
    portENTER_CRITICAL(&s_state_lock);
    s_at_ready = at_ready;
    s_sim_ready = sim_ready;
    s_registered = registered;
    s_rssi = rssi;
    portEXIT_CRITICAL(&s_state_lock);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool response_has_final_ok(const char *resp)
{
    return strstr(resp, "\r\nOK\r\n") != NULL ||
           strstr(resp, "\nOK\r\n") != NULL ||
           strstr(resp, "\r\nOK") != NULL ||
           strcmp(resp, "OK\r\n") == 0 ||
           strcmp(resp, "OK") == 0;
}

static bool response_has_error(const char *resp)
{
    return strstr(resp, "ERROR") != NULL ||
           strstr(resp, "NO CARRIER") != NULL ||
           strstr(resp, "NO ANSWER") != NULL ||
           strstr(resp, "BUSY") != NULL ||
           strstr(resp, "NO DIALTONE") != NULL;
}

static void log_rx_raw(const uint8_t *buf, int len)
{
    if (buf == NULL || len <= 0) {
        return;
    }

    char hex[3 * 32 + 1] = {0};
    char ascii[33] = {0};
    int show = len > 32 ? 32 : len;
    size_t pos = 0;
    for (int i = 0; i < show && pos + 4 < sizeof(hex); ++i) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
        ascii[i] = isprint(buf[i]) ? (char)buf[i] : '.';
    }
    ascii[show] = '\0';
    ESP_LOGI(TAG, "RX raw len=%d show=%d hex=%s ascii=%s", len, show, hex, ascii);
}

static esp_err_t read_response(char *out, size_t out_size, uint32_t timeout_ms, bool wait_prompt)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    size_t used = 0;
    uint8_t buf[96];
    int64_t deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int len = uart_read_bytes(sim_uart_port(), buf, sizeof(buf), pdMS_TO_TICKS(80));
        if (len > 0) {
            log_rx_raw(buf, len);
            size_t copy = (size_t)len;
            if (copy > out_size - 1 - used) {
                copy = out_size - 1 - used;
            }
            if (copy > 0) {
                memcpy(out + used, buf, copy);
                used += copy;
                out[used] = '\0';
            }

            ESP_LOGD(TAG, "rx: %.*s", len, (const char *)buf);

            if (wait_prompt && strchr(out, '>') != NULL) {
                return ESP_OK;
            }
            if (response_has_error(out)) {
                return ESP_FAIL;
            }
            if (response_has_final_ok(out)) {
                return ESP_OK;
            }
        }
    }

    if (used == 0) {
        ESP_LOGW(TAG, "AT rx timeout, no bytes, rx_level=%d", gpio_get_level(CONFIG_APP_SIM800_UART_RX_GPIO));
    } else {
        ESP_LOGW(TAG, "AT rx timeout/partial: %s", out);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t write_raw(const void *data, size_t len)
{
    int written = uart_write_bytes(sim_uart_port(), data, len);
    if (written != (int)len) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(sim_uart_port(), pdMS_TO_TICKS(1000));
}

static esp_err_t send_command_locked(const char *cmd, uint32_t timeout_ms,
                                     char *resp, size_t resp_size)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char local_resp[SIM800_RESP_SIZE];
    if (resp == NULL || resp_size == 0) {
        resp = local_resp;
        resp_size = sizeof(local_resp);
    }

    ESP_LOGI(TAG, "AT tx: %s", cmd);
    uart_flush_input(sim_uart_port());
    esp_err_t ret = write_raw(cmd, strlen(cmd));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = write_raw("\r\n", 2);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = read_response(resp, resp_size, timeout_ms, false);
    ESP_LOGI(TAG, "AT rx: %s", resp);
    return ret;
}

static esp_err_t send_command_prompt_locked(const char *cmd, uint32_t timeout_ms,
                                            char *resp, size_t resp_size)
{
    ESP_LOGI(TAG, "AT tx: %s", cmd);
    uart_flush_input(sim_uart_port());
    esp_err_t ret = write_raw(cmd, strlen(cmd));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = write_raw("\r\n", 2);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = read_response(resp, resp_size, timeout_ms, true);
    ESP_LOGI(TAG, "AT prompt rx: %s", resp);
    return ret;
}

static bool parse_cpin_ready(const char *resp)
{
    return resp != NULL && strstr(resp, "+CPIN: READY") != NULL;
}

static int parse_cereg_status(const char *resp)
{
    if (resp == NULL) {
        return -1;
    }

    const char *p = strstr(resp, "+CEREG:");
    if (p == NULL) {
        return -1;
    }

    int n = -1;
    int stat = -1;
    if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2 ||
        sscanf(p, "+CEREG:%d,%d", &n, &stat) == 2) {
        return stat;
    }
    if (sscanf(p, "+CEREG: %d", &stat) == 1 ||
        sscanf(p, "+CEREG:%d", &stat) == 1) {
        return stat;
    }
    return -1;
}

static int parse_csq_rssi(const char *resp)
{
    if (resp == NULL) {
        return 99;
    }

    const char *p = strstr(resp, "+CSQ:");
    if (p == NULL) {
        return 99;
    }

    int rssi = 99;
    int ber = 99;
    if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2 ||
        sscanf(p, "+CSQ:%d,%d", &rssi, &ber) == 2) {
        return rssi;
    }
    return 99;
}

static bool baud_seen(const int *bauds, size_t count, int baud)
{
    for (size_t i = 0; i < count; ++i) {
        if (bauds[i] == baud) {
            return true;
        }
    }
    return false;
}

static esp_err_t sync_at_locked(char *resp, size_t resp_size)
{
    int bauds[SIM800_BAUD_SCAN_COUNT] = {0};
    size_t count = 0;
    const int candidates[] = {
        CONFIG_APP_SIM800_UART_BAUD,
        115200,
        9600,
        19200,
        38400,
        57600,
        230400,
        460800,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && count < SIM800_BAUD_SCAN_COUNT; ++i) {
        if (!baud_seen(bauds, count, candidates[i])) {
            bauds[count++] = candidates[i];
        }
    }

    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "baud probe: %d", bauds[i]);
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(sim_uart_port(), bauds[i]));
        vTaskDelay(pdMS_TO_TICKS(120));
        uart_flush_input(sim_uart_port());

        for (int attempt = 0; attempt < 4; ++attempt) {
            esp_err_t ret = send_command_locked("AT", 900, resp, resp_size);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "A7670C AT sync ok baud=%d attempt=%d", bauds[i], attempt + 1);
                set_status("A7670C AT ok baud=%d", bauds[i]);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(180));
        }
    }

    set_ready_flags(false, false, false, 99);
    set_status("A7670C no AT response rx=%d", gpio_get_level(CONFIG_APP_SIM800_UART_RX_GPIO));
    return ESP_ERR_TIMEOUT;
}

static esp_err_t probe_network_locked(void)
{
    char resp[SIM800_RESP_SIZE];
    esp_err_t ret = sync_at_locked(resp, sizeof(resp));

    if (ret != ESP_OK) {
        return ret;
    }

    (void)send_command_locked("ATE0", 1200, resp, sizeof(resp));
    (void)send_command_locked("AT+CMEE=2", 1200, resp, sizeof(resp));
    (void)send_command_locked("AT+CEREG=2", 3000, resp, sizeof(resp));

    bool sim_ready = false;
    ret = send_command_locked("AT+CPIN?", 3000, resp, sizeof(resp));
    if (ret == ESP_OK) {
        sim_ready = parse_cpin_ready(resp);
    }

    int cereg_status = -1;
    ret = send_command_locked("AT+CEREG?", 3000, resp, sizeof(resp));
    if (ret == ESP_OK) {
        cereg_status = parse_cereg_status(resp);
    }
    bool registered = cereg_status == 1 || cereg_status == 5;

    int rssi = 99;
    ret = send_command_locked("AT+CSQ", 3000, resp, sizeof(resp));
    if (ret == ESP_OK) {
        rssi = parse_csq_rssi(resp);
    }

    (void)send_command_locked("AT+CPSI?", 5000, resp, sizeof(resp));

    set_ready_flags(true, sim_ready, registered, rssi);
    if (!sim_ready) {
        set_status("SIM card not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!registered) {
        set_status("LTE not registered stat=%d rssi=%d", cereg_status, rssi);
        return ESP_ERR_INVALID_STATE;
    }

    (void)send_command_locked("AT+CMGF=1", 1200, resp, sizeof(resp));
    (void)send_command_locked("AT+CVHU=0", 1200, resp, sizeof(resp));
    set_status("A7670C LTE ready rssi=%d", rssi);
    return ESP_OK;
}

static bool emergency_phone_configured(void)
{
    return CONFIG_APP_SIM800_EMERGENCY_PHONE[0] != '\0';
}

static int utf8_decode_one(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    if (s[0] < 0x80) {
        (*p)++;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        int cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
        return cp;
    }
    if ((s[0] & 0xF0) == 0xE0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        int cp = ((s[0] & 0x0F) << 12) |
                 ((s[1] & 0x3F) << 6) |
                 (s[2] & 0x3F);
        *p += 3;
        return cp;
    }
    if ((s[0] & 0xF8) == 0xF0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *p += 4;
        return '?';
    }

    (*p)++;
    return '?';
}

static esp_err_t append_u16_hex(char *out, size_t out_size, size_t *pos, uint16_t value)
{
    if (*pos + 4 >= out_size) {
        return ESP_ERR_NO_MEM;
    }
    static const char hex_digits[] = "0123456789ABCDEF";
    out[(*pos)++] = hex_digits[(value >> 12) & 0xF];
    out[(*pos)++] = hex_digits[(value >> 8) & 0xF];
    out[(*pos)++] = hex_digits[(value >> 4) & 0xF];
    out[(*pos)++] = hex_digits[value & 0xF];
    out[*pos] = '\0';
    return ESP_OK;
}

static esp_err_t utf8_to_ucs2_hex(const char *text, char *out, size_t out_size)
{
    if (text == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    size_t pos = 0;
    const char *p = text;

    while (*p != '\0') {
        int cp = utf8_decode_one(&p);
        if (cp < 0) {
            cp = '?';
        }
        if (cp > 0xFFFF) {
            cp = '?';
        }
        if (append_u16_hex(out, out_size, &pos, (uint16_t)cp) != ESP_OK) {
            break;
        }
    }

    return pos > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t phone_to_ucs2_hex(const char *number, char *out, size_t out_size)
{
    if (number == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    size_t pos = 0;
    for (const char *p = number; *p != '\0'; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (isdigit(ch) || ch == '+') {
            esp_err_t ret = append_u16_hex(out, out_size, &pos, (uint16_t)ch);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return pos > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void set_sms_error_status(const char *stage, esp_err_t ret, const char *resp)
{
    if (resp != NULL && resp[0] != '\0') {
        char clean[80] = {0};
        size_t pos = 0;
        for (const char *p = resp; *p != '\0' && pos + 1 < sizeof(clean); ++p) {
            char ch = *p;
            clean[pos++] = (ch == '\r' || ch == '\n') ? ' ' : ch;
        }
        set_status("%s failed: %s", stage, clean);
    } else {
        set_status("%s failed: %s", stage, esp_err_to_name(ret));
    }
}

static esp_err_t send_sms_ascii_locked(const char *number, const char *text)
{
    if (number == NULL || number[0] == '\0' || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char resp[SIM800_RESP_SIZE];
    char cmd[96];

    esp_err_t ret = app_sim800_is_ready() ? ESP_OK : probe_network_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = send_command_locked("AT+CMGF=1", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CMGF", ret, resp);
        return ret;
    }
    ret = send_command_locked("AT+CSCS=\"GSM\"", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CSCS GSM", ret, resp);
        return ret;
    }
    ret = send_command_locked("AT+CSMP=17,167,0,0", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CSMP GSM", ret, resp);
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    ret = send_command_prompt_locked(cmd, 5000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS prompt GSM", ret, resp);
        return ret;
    }

    ret = write_raw(text, strlen(text));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS write GSM", ret, NULL);
        return ret;
    }
    const uint8_t ctrl_z = 0x1A;
    ret = write_raw(&ctrl_z, 1);
    if (ret != ESP_OK) {
        set_sms_error_status("SMS ctrl-z GSM", ret, NULL);
        return ret;
    }

    ret = read_response(resp, sizeof(resp), 60000, false);
    ESP_LOGI(TAG, "SMS GSM rx: %s", resp);
    if (ret == ESP_OK) {
        set_status("SMS sent GSM rssi=%d", s_rssi);
    } else {
        set_sms_error_status("SMS send GSM", ret, resp);
    }
    return ret;
}

static esp_err_t send_sms_ucs2_locked(const char *number, const char *text)
{
    if (number == NULL || number[0] == '\0' || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char resp[SIM800_RESP_SIZE];
    char num_hex[SIM800_SMS_NUMBER_HEX_MAX];
    char body_hex[SIM800_SMS_BODY_HEX_MAX];
    char cmd[96];

    esp_err_t ret = app_sim800_is_ready() ? ESP_OK : probe_network_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = phone_to_ucs2_hex(number, num_hex, sizeof(num_hex));
    if (ret != ESP_OK) {
        set_status("SMS number invalid");
        return ret;
    }

    ret = utf8_to_ucs2_hex(text, body_hex, sizeof(body_hex));
    if (ret != ESP_OK) {
        set_status("SMS text invalid");
        return ret;
    }

    ret = send_command_locked("AT+CMGF=1", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CMGF UCS2", ret, resp);
        return ret;
    }
    ret = send_command_locked("AT+CSCS=\"UCS2\"", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CSCS UCS2", ret, resp);
        return ret;
    }
    ret = send_command_locked("AT+CSMP=17,167,2,25", 2000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS CSMP UCS2", ret, resp);
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", num_hex);
    ret = send_command_prompt_locked(cmd, 5000, resp, sizeof(resp));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS prompt UCS2", ret, resp);
        return ret;
    }

    ret = write_raw(body_hex, strlen(body_hex));
    if (ret != ESP_OK) {
        set_sms_error_status("SMS write UCS2", ret, NULL);
        return ret;
    }
    const uint8_t ctrl_z = 0x1A;
    ret = write_raw(&ctrl_z, 1);
    if (ret != ESP_OK) {
        set_sms_error_status("SMS ctrl-z UCS2", ret, NULL);
        return ret;
    }

    ret = read_response(resp, sizeof(resp), 60000, false);
    ESP_LOGI(TAG, "SMS UCS2 rx: %s", resp);
    if (ret == ESP_OK) {
        set_status("SMS sent UCS2 rssi=%d", s_rssi);
    } else {
        set_sms_error_status("SMS send UCS2", ret, resp);
    }
    char restore_resp[SIM800_RESP_SIZE];
    (void)send_command_locked("AT+CSCS=\"GSM\"", 2000, restore_resp, sizeof(restore_resp));
    return ret;
}

static void pulse_pwrkey_if_configured(void)
{
#if CONFIG_APP_SIM800_PWRKEY_GPIO >= 0
    gpio_num_t gpio = (gpio_num_t)CONFIG_APP_SIM800_PWRKEY_GPIO;
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io_conf));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(gpio, 1));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "pulse A7670C PWRKEY GPIO=%d", CONFIG_APP_SIM800_PWRKEY_GPIO);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(gpio, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(gpio, 1));
    vTaskDelay(pdMS_TO_TICKS(12000));
#endif
}

static void sim800_monitor_task(void *arg)
{
    (void)arg;

    pulse_pwrkey_if_configured();

    while (true) {
        if (app_sim800_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_err_t ret = probe_network_locked();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "A7670C probe failed: %s", esp_err_to_name(ret));
            }
            xSemaphoreGive(s_cmd_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t app_sim800_start(void)
{
#if !CONFIG_APP_SIM800_ENABLE
        set_status("A7670C disabled");
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    s_cmd_lock = xSemaphoreCreateMutex();
    if (s_cmd_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = CONFIG_APP_SIM800_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(sim_uart_port(), SIM800_RX_BUF_SIZE,
                                        SIM800_TX_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        set_status("A7670C UART install failed");
        return ret;
    }

    ret = uart_param_config(sim_uart_port(), &config);
    if (ret != ESP_OK) {
        set_status("A7670C UART config failed");
        return ret;
    }

    ret = uart_set_pin(sim_uart_port(), CONFIG_APP_SIM800_UART_TX_GPIO,
                       CONFIG_APP_SIM800_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        set_status("A7670C UART pins failed");
        return ret;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(CONFIG_APP_SIM800_UART_RX_GPIO, GPIO_PULLUP_ONLY));

    BaseType_t ok = xTaskCreate(sim800_monitor_task, "a7670_mon", 6144, NULL, 3, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    set_status("A7670C starting");
    ESP_LOGI(TAG, "A7670C started UART=%d baud=%d TX=%d RX=%d PWRKEY=%d",
             CONFIG_APP_SIM800_UART_NUM, CONFIG_APP_SIM800_UART_BAUD,
             CONFIG_APP_SIM800_UART_TX_GPIO, CONFIG_APP_SIM800_UART_RX_GPIO,
             CONFIG_APP_SIM800_PWRKEY_GPIO);
    return ESP_OK;
#endif
}

bool app_sim800_is_ready(void)
{
    bool ready;
    portENTER_CRITICAL(&s_state_lock);
    ready = s_at_ready && s_sim_ready && s_registered;
    portEXIT_CRITICAL(&s_state_lock);
    return ready;
}

const char *app_sim800_status_text(void)
{
    static char text[128];
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(text, s_status, sizeof(text));
    portEXIT_CRITICAL(&s_state_lock);
    return text;
}

esp_err_t app_sim800_call(const char *number)
{
#if !CONFIG_APP_SIM800_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (number == NULL || number[0] == '\0') {
        set_status("Call number empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        set_status("A7670C busy");
        return ESP_ERR_TIMEOUT;
    }

    char resp[SIM800_RESP_SIZE];
    char cmd[64];
    esp_err_t ret = app_sim800_is_ready() ? ESP_OK : probe_network_locked();
    if (ret == ESP_OK) {
        (void)send_command_locked("AT+CSCS=\"GSM\"", 2000, resp, sizeof(resp));
        (void)send_command_locked("AT+CVHU=0", 2000, resp, sizeof(resp));
        snprintf(cmd, sizeof(cmd), "ATD%s;", number);
        set_status("Calling %s", number);
        ret = send_command_locked(cmd, 20000, resp, sizeof(resp));
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_err_t clcc_ret = send_command_locked("AT+CLCC", 5000, resp, sizeof(resp));
            set_status(clcc_ret == ESP_OK && strstr(resp, "+CLCC:") != NULL
                           ? "Call dialing/active"
                           : "Call command accepted");
        } else {
            set_sms_error_status("Call dial", ret, resp);
        }
    }

    xSemaphoreGive(s_cmd_lock);
    return ret;
#endif
}

esp_err_t app_sim800_hangup(void)
{
#if !CONFIG_APP_SIM800_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    char resp[SIM800_RESP_SIZE];
    esp_err_t ret = send_command_locked("ATH", 20000, resp, sizeof(resp));
    set_status(ret == ESP_OK ? "Call hangup" : "Hangup failed");
    xSemaphoreGive(s_cmd_lock);
    return ret;
#endif
}

esp_err_t app_sim800_send_sms(const char *number, const char *text)
{
#if !CONFIG_APP_SIM800_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        set_status("A7670C busy");
        return ESP_ERR_TIMEOUT;
    }
    set_status("Sending SMS");
    esp_err_t ret = send_sms_ucs2_locked(number, text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "UCS2 SMS failed, retry with short GSM SMS: %s", esp_err_to_name(ret));
        ret = send_sms_ascii_locked(number, "HELP! Health terminal emergency. Please check elder now.");
    }
    xSemaphoreGive(s_cmd_lock);
    return ret;
#endif
}

esp_err_t app_sim800_emergency_call(void)
{
    if (!emergency_phone_configured()) {
        set_status("Emergency phone empty");
        return ESP_ERR_INVALID_ARG;
    }
    return app_sim800_call(CONFIG_APP_SIM800_EMERGENCY_PHONE);
}

esp_err_t app_sim800_emergency_sms(const char *text)
{
    if (!emergency_phone_configured()) {
        set_status("Emergency phone empty");
        return ESP_ERR_INVALID_ARG;
    }

    const char *body = text;
    if (body == NULL || body[0] == '\0') {
        body = CONFIG_APP_SIM800_EMERGENCY_SMS_TEXT;
    }
    return app_sim800_send_sms(CONFIG_APP_SIM800_EMERGENCY_PHONE, body);
}

esp_err_t app_sim800_auto_alert(const char *text)
{
#if !CONFIG_APP_SIM800_AUTO_SMS_ENABLE && !CONFIG_APP_SIM800_AUTO_CALL_ENABLE
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint32_t now = (uint32_t)(now_ms() / 1000);
    portENTER_CRITICAL(&s_state_lock);
    uint32_t last = s_last_auto_alert_ms;
    portEXIT_CRITICAL(&s_state_lock);

    if (last != 0 && now - last < CONFIG_APP_SIM800_AUTO_COOLDOWN_SECONDS) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
#if CONFIG_APP_SIM800_AUTO_SMS_ENABLE
    ESP_LOGW(TAG, "Auto alert stage 1/2: sending SMS");
    ret = app_sim800_emergency_sms(text);
    ESP_LOGW(TAG, "Auto alert SMS finished: %s status=%s",
             esp_err_to_name(ret), app_sim800_status_text());
#endif
#if CONFIG_APP_SIM800_AUTO_CALL_ENABLE
#if CONFIG_APP_SIM800_AUTO_SMS_ENABLE
    ESP_LOGI(TAG, "Waiting %d ms between SMS and call", A7670_AUTO_SMS_CALL_GAP_MS);
    vTaskDelay(pdMS_TO_TICKS(A7670_AUTO_SMS_CALL_GAP_MS));
#endif
    ESP_LOGW(TAG, "Auto alert stage 2/2: placing call");
    esp_err_t call_ret = app_sim800_emergency_call();
    ESP_LOGW(TAG, "Auto alert call finished: %s status=%s",
             esp_err_to_name(call_ret), app_sim800_status_text());
    if (ret == ESP_OK) {
        ret = call_ret;
    }
#endif

    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_last_auto_alert_ms = now;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return ret;
#endif
}
