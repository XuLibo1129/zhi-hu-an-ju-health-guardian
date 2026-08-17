#include "app_weather.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_network.h"

#ifndef CONFIG_APP_WEATHER_ENABLE
#define CONFIG_APP_WEATHER_ENABLE 1
#endif

#ifndef CONFIG_APP_WEATHER_AUTO_LOCATION
#define CONFIG_APP_WEATHER_AUTO_LOCATION 0
#endif

#ifndef CONFIG_APP_WEATHER_FIXED_LATITUDE
#define CONFIG_APP_WEATHER_FIXED_LATITUDE "32.0805"
#endif

#ifndef CONFIG_APP_WEATHER_FIXED_LONGITUDE
#define CONFIG_APP_WEATHER_FIXED_LONGITUDE "118.6729"
#endif

#ifndef CONFIG_APP_WEATHER_FIXED_CITY
#define CONFIG_APP_WEATHER_FIXED_CITY "\xE5\x8D\x97\xE4\xBA\xAC\xE5\xB8\x82\xE6\xB1\x9F\xE5\x8C\x97\xE6\x96\xB0\xE5\x8C\xBA"
#endif

#ifndef CONFIG_APP_WEATHER_CHINA_STATION_ID
#define CONFIG_APP_WEATHER_CHINA_STATION_ID "101190107"
#endif

#ifndef CONFIG_APP_WEATHER_UPDATE_MINUTES
#define CONFIG_APP_WEATHER_UPDATE_MINUTES 30
#endif

#define WEATHER_HTTP_RX_BUFFER_SIZE 4096
#define WEATHER_IP_URL "http://ip-api.com/json/?fields=status,message,city,lat,lon&lang=zh-CN"
#define WEATHER_URL_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto"
#define WEATHER_CN_URL_FMT "http://d1.weather.com.cn/sk_2d/%s.html"

static const char *TAG = "app_weather";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_rx_t;

static SemaphoreHandle_t s_mutex;
static bool s_started;
static app_weather_info_t s_latest;
static char s_status[96] = "weather idle";

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

static esp_err_t http_get_text(const char *url, char *out, size_t out_size, int timeout_ms)
{
    if (url == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    http_rx_t rx = {
        .buf = out,
        .len = 0,
        .cap = out_size,
    };
    out[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &rx,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "User-Agent", "ESP32-P4-Health-Terminal/1.0");
    if (strstr(url, "weather.com.cn") != NULL) {
        esp_http_client_set_header(client, "Referer", "http://www.weather.com.cn/");
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "GET %s status=%d bytes=%u err=%s",
             url, status_code, (unsigned)rx.len, esp_err_to_name(ret));

    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        return ret;
    }
    if (status_code < 200 || status_code >= 300) {
        return ESP_FAIL;
    }
    return rx.len > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static const char *condition_from_code(int code)
{
    switch (code) {
    case 0:
        return "\xE6\x99\xB4";
    case 1:
    case 2:
        return "\xE5\xA4\x9A\xE4\xBA\x91";
    case 3:
        return "\xE9\x98\xB4";
    case 45:
    case 48:
        return "\xE9\x9B\xBE";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
        return "\xE5\xB0\x8F\xE9\x9B\xA8";
    case 63:
    case 66:
    case 67:
    case 80:
    case 81:
        return "\xE4\xB8\xAD\xE9\x9B\xA8";
    case 65:
    case 82:
        return "\xE5\xA4\xA7\xE9\x9B\xA8";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "\xE9\x9B\xAA";
    case 95:
    case 96:
    case 99:
        return "\xE9\x9B\xB7\xE9\x9B\xA8";
    default:
        return "\xE6\x9C\xAA\xE7\x9F\xA5";
    }
}

static int round_to_int(double value)
{
    if (value >= 0.0) {
        return (int)(value + 0.5);
    }
    return (int)(value - 0.5);
}

static const char *condition_from_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return "\xE6\x9C\xAA\xE7\x9F\xA5";
    }
    if (strstr(text, "\xE9\x9B\xB7") != NULL) {
        return "\xE9\x9B\xB7\xE9\x9B\xA8";
    }
    if (strstr(text, "\xE9\x9B\xAA") != NULL) {
        return "\xE9\x9B\xAA";
    }
    if (strstr(text, "\xE9\x9B\xBE") != NULL || strstr(text, "\xE9\x9C\xBE") != NULL) {
        return "\xE9\x9B\xBE";
    }
    if (strstr(text, "\xE6\x9A\xB4\xE9\x9B\xA8") != NULL || strstr(text, "\xE5\xA4\xA7\xE9\x9B\xA8") != NULL) {
        return "\xE5\xA4\xA7\xE9\x9B\xA8";
    }
    if (strstr(text, "\xE4\xB8\xAD\xE9\x9B\xA8") != NULL) {
        return "\xE4\xB8\xAD\xE9\x9B\xA8";
    }
    if (strstr(text, "\xE9\x9B\xA8") != NULL) {
        return "\xE5\xB0\x8F\xE9\x9B\xA8";
    }
    if (strstr(text, "\xE9\x98\xB4") != NULL) {
        return "\xE9\x98\xB4";
    }
    if (strstr(text, "\xE4\xBA\x91") != NULL) {
        return "\xE5\xA4\x9A\xE4\xBA\x91";
    }
    return "\xE6\x99\xB4";
}

static double json_double_value(const cJSON *item, double default_value)
{
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return atof(item->valuestring);
    }
    return default_value;
}

static esp_err_t fetch_china_weather(const char *station_id, const char *city)
{
    char url[128];
    snprintf(url, sizeof(url), WEATHER_CN_URL_FMT, station_id);

    char *rx = calloc(1, WEATHER_HTTP_RX_BUFFER_SIZE);
    if (rx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = http_get_text(url, rx, WEATHER_HTTP_RX_BUFFER_SIZE, 9000);
    if (ret != ESP_OK) {
        free(rx);
        return ret;
    }

    char *json_start = strchr(rx, '{');
    cJSON *root = json_start != NULL ? cJSON_Parse(json_start) : NULL;
    free(rx);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temp");
    const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(root, "SD");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    double temp_value = json_double_value(temp, -999.0);
    int humidity_value = cJSON_IsString(humidity) && humidity->valuestring != NULL ?
                         atoi(humidity->valuestring) : -1;

    if (temp_value < -100.0 || humidity_value < 0 || humidity_value > 100 ||
        !cJSON_IsString(weather) || weather->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    app_weather_info_t info = {
        .valid = true,
        .temperature_c = round_to_int(temp_value),
        .humidity = humidity_value,
        .weather_code = 0,
        .updated_ms = esp_timer_get_time() / 1000,
    };
    strlcpy(info.city, city, sizeof(info.city));
    strlcpy(info.condition, condition_from_text(weather->valuestring), sizeof(info.condition));
    cJSON_Delete(root);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_latest = info;
        xSemaphoreGive(s_mutex);
    }

    set_status("weather updated from Nanjing Jiangbei station");
    return ESP_OK;
}

#if CONFIG_APP_WEATHER_AUTO_LOCATION
static esp_err_t fetch_auto_location(double *lat, double *lon, char *city, size_t city_size)
{
    char *rx = calloc(1, WEATHER_HTTP_RX_BUFFER_SIZE);
    if (rx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = http_get_text(WEATHER_IP_URL, rx, WEATHER_HTTP_RX_BUFFER_SIZE, 6000);
    if (ret != ESP_OK) {
        free(rx);
        return ret;
    }

    cJSON *root = cJSON_Parse(rx);
    free(rx);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *city_item = cJSON_GetObjectItem(root, "city");
    cJSON *lat_item = cJSON_GetObjectItem(root, "lat");
    cJSON *lon_item = cJSON_GetObjectItem(root, "lon");

    if (!cJSON_IsString(status) || strcmp(status->valuestring, "success") != 0 ||
        !cJSON_IsNumber(lat_item) || !cJSON_IsNumber(lon_item)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *lat = lat_item->valuedouble;
    *lon = lon_item->valuedouble;
    if (cJSON_IsString(city_item) && city_item->valuestring != NULL && city_item->valuestring[0] != '\0') {
        strlcpy(city, city_item->valuestring, city_size);
    } else {
        strlcpy(city, CONFIG_APP_WEATHER_FIXED_CITY, city_size);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
#endif

static esp_err_t fetch_weather(double lat, double lon, const char *city)
{
    char url[240];
    snprintf(url, sizeof(url), WEATHER_URL_FMT, lat, lon);

    char *rx = calloc(1, WEATHER_HTTP_RX_BUFFER_SIZE);
    if (rx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = http_get_text(url, rx, WEATHER_HTTP_RX_BUFFER_SIZE, 9000);
    if (ret != ESP_OK) {
        free(rx);
        return ret;
    }

    cJSON *root = cJSON_Parse(rx);
    free(rx);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    cJSON *temp = current != NULL ? cJSON_GetObjectItem(current, "temperature_2m") : NULL;
    cJSON *humidity = current != NULL ? cJSON_GetObjectItem(current, "relative_humidity_2m") : NULL;
    cJSON *weather_code = current != NULL ? cJSON_GetObjectItem(current, "weather_code") : NULL;

    if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(humidity) || !cJSON_IsNumber(weather_code)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    app_weather_info_t info = {
        .valid = true,
        .temperature_c = round_to_int(temp->valuedouble),
        .humidity = round_to_int(humidity->valuedouble),
        .weather_code = weather_code->valueint,
        .updated_ms = esp_timer_get_time() / 1000,
    };
    strlcpy(info.city, city, sizeof(info.city));
    strlcpy(info.condition, condition_from_code(info.weather_code), sizeof(info.condition));

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_latest = info;
        xSemaphoreGive(s_mutex);
    }

    cJSON_Delete(root);
    set_status("weather updated");
    return ESP_OK;
}

static void weather_task(void *arg)
{
    (void)arg;

    while (true) {
        while (!app_network_is_ready()) {
            set_status("weather wait network");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        double lat = atof(CONFIG_APP_WEATHER_FIXED_LATITUDE);
        double lon = atof(CONFIG_APP_WEATHER_FIXED_LONGITUDE);
        char city[48] = CONFIG_APP_WEATHER_FIXED_CITY;

#if CONFIG_APP_WEATHER_AUTO_LOCATION
        esp_err_t loc_ret = fetch_auto_location(&lat, &lon, city, sizeof(city));
        if (loc_ret != ESP_OK) {
            ESP_LOGW(TAG, "auto location failed: %s; use fixed location", esp_err_to_name(loc_ret));
            strlcpy(city, CONFIG_APP_WEATHER_FIXED_CITY, sizeof(city));
        }
#endif

        esp_err_t ret = fetch_china_weather(CONFIG_APP_WEATHER_CHINA_STATION_ID, city);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Nanjing Jiangbei station failed: %s; fallback to Open-Meteo", esp_err_to_name(ret));
            ret = fetch_weather(lat, lon, city);
        }
        if (ret != ESP_OK) {
            snprintf(s_status, sizeof(s_status), "weather failed: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "%s", s_status);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_WEATHER_UPDATE_MINUTES * 60 * 1000));
    }
}

esp_err_t app_weather_start(void)
{
#if !CONFIG_APP_WEATHER_ENABLE
    set_status("weather disabled");
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    set_status("weather started");
    return ESP_OK;
#endif
}

void app_weather_get_latest(app_weather_info_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        *out = s_latest;
        xSemaphoreGive(s_mutex);
    }
}

const char *app_weather_status_text(void)
{
    return s_status;
}
