#include "app_network.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "app_network";

static bool s_started;
static bool s_ready;
static bool s_sntp_started;
static char s_status[96] = "NET: idle";

static void set_status(const char *status)
{
    strlcpy(s_status, status, sizeof(s_status));
    ESP_LOGI(TAG, "%s", s_status);
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.wait_for_sync = false;
    config.start = true;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_status("NET: connecting Wi-Fi");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event ? event->reason : 0;

        s_ready = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", reason);
        snprintf(s_status, sizeof(s_status), "NET: reconnect r=%u", reason);
        ESP_LOGI(TAG, "%s", s_status);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_ready = true;
        snprintf(s_status, sizeof(s_status), "NET: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "%s", s_status);
        start_sntp_once();
    }
}

esp_err_t app_network_start(void)
{
#if CONFIG_APP_WIFI_ENABLE
    if (s_started) {
        return ESP_OK;
    }

    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        set_status("NET: SSID missing");
        return ESP_ERR_INVALID_STATE;
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
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register IP_EVENT failed");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    s_started = true;
    set_status("NET: Wi-Fi started");
    return ESP_OK;
#else
    set_status("NET: disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool app_network_is_ready(void)
{
    return s_ready;
}

const char *app_network_status_text(void)
{
    return s_status;
}
