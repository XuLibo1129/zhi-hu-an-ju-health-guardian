#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_csi_tx";
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to CSI receiver SSID=%s", CONFIG_CSI_WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected from receiver, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_CSI_WIFI_SSID,
            .password = CONFIG_CSI_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(CONFIG_CSI_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                          WIFI_PROTOCOL_11B |
                                          WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void udp_sender_task(void *arg)
{
    (void)arg;
    uint8_t payload[CONFIG_CSI_TX_PAYLOAD_LEN];
    uint32_t seq = 0;

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CSI_UDP_PORT),
    };
    dest_addr.sin_addr.s_addr = inet_addr(CONFIG_CSI_RX_IP);

    while (true) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "UDP sender started: %s:%d every %d ms, payload=%d bytes",
                 CONFIG_CSI_RX_IP, CONFIG_CSI_UDP_PORT,
                 CONFIG_CSI_TX_INTERVAL_MS, CONFIG_CSI_TX_PAYLOAD_LEN);

        while ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0) {
            memset(payload, 0xA5, sizeof(payload));
            memcpy(payload, &seq, sizeof(seq));

            int err = sendto(sock, payload, sizeof(payload), 0,
                             (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err < 0) {
                ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
                break;
            }

            if ((seq % 100) == 0) {
                ESP_LOGI(TAG, "sent seq=%" PRIu32, seq);
            }

            seq++;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CSI_TX_INTERVAL_MS));
        }

        shutdown(sock, 0);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    uint8_t sta_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(sta_mac, ESP_MAC_WIFI_STA));

    ESP_LOGI(TAG, "ESP32-S3 WiFi CSI TX starting");
    ESP_LOGI(TAG, "B board STA MAC: " MACSTR, MAC2STR(sta_mac));
    ESP_LOGI(TAG, "Target receiver: SSID=%s UDP=%s:%d interval=%d ms",
             CONFIG_CSI_WIFI_SSID, CONFIG_CSI_RX_IP,
             CONFIG_CSI_UDP_PORT, CONFIG_CSI_TX_INTERVAL_MS);

    nvs_init();
    wifi_init_sta();
    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);
}
