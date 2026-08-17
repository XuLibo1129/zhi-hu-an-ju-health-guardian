#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#ifndef CONFIG_ESP_WIFI_CSI_ENABLED
#error "CONFIG_ESP_WIFI_CSI_ENABLED must be enabled for the CSI receiver"
#endif

static const char *TAG = "wifi_csi_rx";

#define WIFI_STA_CONNECTED_BIT BIT0

typedef struct {
    uint32_t seq;
    uint16_t len;
    bool first_word_invalid;
    uint8_t mac[6];
    wifi_pkt_rx_ctrl_t rx_ctrl;
    int8_t buf[CONFIG_CSI_MAX_LEN];
} csi_frame_t;

static QueueHandle_t s_csi_queue;
static volatile uint32_t s_csi_seq;
static volatile uint32_t s_csi_dropped;
static volatile uint32_t s_udp_packets;
static volatile uint32_t s_udp_bytes;
static volatile uint32_t s_forward_packets;
static volatile uint32_t s_forward_errors;
#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
static volatile uint32_t s_feature_updates;
static volatile uint32_t s_forward_repeated;
#endif
#if CONFIG_CSI_STA_ENABLE
static EventGroupHandle_t s_wifi_event_group;
static volatile uint32_t s_sta_disconnects;
#endif

typedef enum {
    CSI_FILTER_OFF = 0,
    CSI_FILTER_FIXED,
    CSI_FILTER_AUTO,
} csi_filter_mode_t;

typedef struct {
    uint8_t mac[6];
    uint32_t count;
} csi_mac_counter_t;

#define MAX_AP_STA_FILTERS 4

static csi_filter_mode_t s_filter_mode = CSI_FILTER_AUTO;
static uint8_t s_filter_mac[6];
static bool s_filter_locked;
static volatile uint32_t s_filter_dropped;
static uint8_t s_ap_sta_macs[MAX_AP_STA_FILTERS][6];
static uint8_t s_ap_sta_count;

#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
typedef struct {
    char line[128];
    uint32_t seq;
    bool valid;
} csi_feature_snapshot_t;

static portMUX_TYPE s_feature_mux = portMUX_INITIALIZER_UNLOCKED;
static csi_feature_snapshot_t s_latest_feature;
#endif

static bool mac_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool ap_sta_contains_mac(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < s_ap_sta_count; i++) {
        if (mac_equal(s_ap_sta_macs[i], mac)) {
            return true;
        }
    }
    return false;
}

static void csi_auto_lock_to_mac(const uint8_t mac[6], const char *reason)
{
    if (s_filter_mode != CSI_FILTER_AUTO || s_filter_locked || mac_is_zero(mac)) {
        return;
    }

    memcpy(s_filter_mac, mac, sizeof(s_filter_mac));
    s_filter_locked = true;
    ESP_LOGI(TAG, "CSI MAC auto-locked to " MACSTR " (%s)",
             MAC2STR(s_filter_mac), reason ? reason : "auto");
}

static void ap_sta_add_mac(const uint8_t mac[6])
{
    if (mac_is_zero(mac) || ap_sta_contains_mac(mac)) {
        return;
    }

    if (s_ap_sta_count >= MAX_AP_STA_FILTERS) {
        ESP_LOGW(TAG, "AP STA filter table full; ignoring " MACSTR, MAC2STR(mac));
        return;
    }

    memcpy(s_ap_sta_macs[s_ap_sta_count], mac, 6);
    s_ap_sta_count++;
    ESP_LOGI(TAG, "AP STA CSI source added: " MACSTR " count=%u",
             MAC2STR(mac), s_ap_sta_count);

    csi_auto_lock_to_mac(mac, "first AP STA");
}

static void ap_sta_remove_mac(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < s_ap_sta_count; i++) {
        if (!mac_equal(s_ap_sta_macs[i], mac)) {
            continue;
        }

        uint8_t last = s_ap_sta_count - 1;
        if (i != last) {
            memcpy(s_ap_sta_macs[i], s_ap_sta_macs[last], 6);
        }
        memset(s_ap_sta_macs[last], 0, 6);
        s_ap_sta_count--;
        ESP_LOGI(TAG, "AP STA CSI source removed: " MACSTR " count=%u",
                 MAC2STR(mac), s_ap_sta_count);

        if (s_filter_mode == CSI_FILTER_AUTO && s_filter_locked &&
            mac_equal(s_filter_mac, mac)) {
            memset(s_filter_mac, 0, sizeof(s_filter_mac));
            s_filter_locked = false;
            ESP_LOGW(TAG, "CSI MAC auto-lock cleared because AP STA disconnected");
        }
        return;
    }
}

static bool parse_mac_string(const char *text, uint8_t out[6])
{
    unsigned int values[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        if (values[i] > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)values[i];
    }
    return true;
}

static void csi_filter_init(void)
{
    const char *config = CONFIG_CSI_FILTER_MAC;

    if (config[0] == '\0' || strcmp(config, "off") == 0 || strcmp(config, "none") == 0) {
        s_filter_mode = CSI_FILTER_OFF;
        ESP_LOGI(TAG, "CSI MAC filter disabled");
        return;
    }

    if (strcmp(config, "auto") == 0) {
        s_filter_mode = CSI_FILTER_AUTO;
        s_filter_locked = false;
        ESP_LOGI(TAG, "CSI MAC filter auto enabled; AP-connected STA MACs are preferred");
        return;
    }

    if (parse_mac_string(config, s_filter_mac)) {
        s_filter_mode = CSI_FILTER_FIXED;
        s_filter_locked = true;
        ESP_LOGI(TAG, "CSI MAC filter fixed to " MACSTR, MAC2STR(s_filter_mac));
        return;
    }

    s_filter_mode = CSI_FILTER_AUTO;
    s_filter_locked = false;
    ESP_LOGW(TAG, "Invalid CSI_FILTER_MAC=%s; falling back to auto", config);
}

static bool csi_filter_accept(const csi_frame_t *frame)
{
    if (s_filter_mode == CSI_FILTER_OFF) {
        return true;
    }

    if (mac_is_zero(frame->mac)) {
        s_filter_dropped++;
        return false;
    }

    if (s_filter_mode == CSI_FILTER_FIXED || s_filter_locked) {
        if (mac_equal(frame->mac, s_filter_mac)) {
            return true;
        }
        s_filter_dropped++;
        return false;
    }

    if (s_filter_mode == CSI_FILTER_AUTO && s_ap_sta_count > 0 &&
        ap_sta_contains_mac(frame->mac)) {
        return true;
    }

    s_filter_dropped++;
    return false;
}

static void csi_auto_filter_update(csi_mac_counter_t counters[], size_t counter_count,
                                   const csi_frame_t *frame, uint32_t *window_frames,
                                   uint32_t *last_udp_packets)
{
    if (s_filter_mode != CSI_FILTER_AUTO || s_filter_locked || mac_is_zero(frame->mac)) {
        return;
    }

    (*window_frames)++;

    for (size_t i = 0; i < counter_count; i++) {
        if (counters[i].count == 0) {
            memcpy(counters[i].mac, frame->mac, sizeof(counters[i].mac));
            counters[i].count = 1;
            break;
        }
        if (mac_equal(counters[i].mac, frame->mac)) {
            counters[i].count++;
            break;
        }
    }

    if (*window_frames < 150) {
        return;
    }

    uint32_t udp_delta = s_udp_packets - *last_udp_packets;
    *last_udp_packets = s_udp_packets;

    size_t best = 0;
    for (size_t i = 1; i < counter_count; i++) {
        if (counters[i].count > counters[best].count) {
            best = i;
        }
    }

    if (udp_delta >= 20 && counters[best].count >= 20) {
        memcpy(s_filter_mac, counters[best].mac, sizeof(s_filter_mac));
        s_filter_locked = true;
        ESP_LOGI(TAG, "CSI MAC auto-locked to " MACSTR " frames=%" PRIu32 " udp_delta=%" PRIu32,
                 MAC2STR(s_filter_mac), counters[best].count, udp_delta);
    } else {
        memset(counters, 0, sizeof(csi_mac_counter_t) * counter_count);
        *window_frames = 0;
    }
}

static uint32_t csi_pair_amp2(const csi_frame_t *frame, uint16_t pair_index)
{
    uint16_t idx = pair_index * 2;
    if (idx + 1 >= frame->len) {
        return 0;
    }

    int32_t imag = frame->buf[idx];
    int32_t real = frame->buf[idx + 1];
    return (uint32_t)(real * real + imag * imag);
}

static uint32_t csi_mean_amp2(const csi_frame_t *frame)
{
    uint32_t sum = 0;
    uint32_t count = 0;

    for (uint16_t i = 0; i + 1 < frame->len; i += 2) {
        int32_t imag = frame->buf[i];
        int32_t real = frame->buf[i + 1];
        if (imag == 0 && real == 0) {
            continue;
        }
        sum += (uint32_t)(real * real + imag * imag);
        count++;
    }

    return count ? (sum / count) : 0;
}

static bool csi_frame_has_feature_len(const csi_frame_t *frame)
{
    return frame->len >= CONFIG_CSI_MIN_FEATURE_LEN;
}

#if CONFIG_CSI_FORWARD_UDP
static int udp_forward_open(struct sockaddr_in *dest_addr)
{
    memset(dest_addr, 0, sizeof(*dest_addr));
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(CONFIG_CSI_FORWARD_PORT);

    if (inet_pton(AF_INET, CONFIG_CSI_FORWARD_IP, &dest_addr->sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid CSI_FORWARD_IP: %s", CONFIG_CSI_FORWARD_IP);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP forward socket: errno=%d", errno);
        return -1;
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGW(TAG, "Set SO_BROADCAST failed: errno=%d", errno);
    }

    ESP_LOGI(TAG, "CSI UDP forward target=%s:%d", CONFIG_CSI_FORWARD_IP, CONFIG_CSI_FORWARD_PORT);
    return sock;
}

#if CONFIG_CSI_FORWARD_FIXED_RATE
static void latest_feature_update(const char *line, int len)
{
    if (len <= 0 || len >= (int)sizeof(s_latest_feature.line)) {
        return;
    }

    portENTER_CRITICAL(&s_feature_mux);
    memcpy(s_latest_feature.line, line, len);
    s_latest_feature.line[len] = '\0';
    s_latest_feature.seq++;
    s_latest_feature.valid = true;
    portEXIT_CRITICAL(&s_feature_mux);

    s_feature_updates++;
}

static bool latest_feature_copy(char *line, size_t line_size, uint32_t *seq)
{
    bool valid;

    portENTER_CRITICAL(&s_feature_mux);
    valid = s_latest_feature.valid;
    if (valid) {
        snprintf(line, line_size, "%s", s_latest_feature.line);
        *seq = s_latest_feature.seq;
    }
    portEXIT_CRITICAL(&s_feature_mux);

    return valid;
}
#endif
#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "STA connected: " MACSTR " aid=%d",
                 MAC2STR(event->mac), event->aid);
        ap_sta_add_mac(event->mac);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected: " MACSTR " aid=%d",
                 MAC2STR(event->mac), event->aid);
        ap_sta_remove_mac(event->mac);
#if CONFIG_CSI_STA_ENABLE
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting STA to router SSID=%s", CONFIG_CSI_STA_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        }
        s_sta_disconnects++;
        ESP_LOGW(TAG, "Router STA disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Router STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        }
#endif
    }
}

static void csi_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;

    if (!data || !data->buf || data->len == 0 || !s_csi_queue) {
        return;
    }

    csi_frame_t frame = {0};
    frame.seq = ++s_csi_seq;
    frame.len = data->len > CONFIG_CSI_MAX_LEN ? CONFIG_CSI_MAX_LEN : data->len;
    frame.first_word_invalid = data->first_word_invalid;
    memcpy(frame.mac, data->mac, sizeof(frame.mac));
    memcpy(&frame.rx_ctrl, &data->rx_ctrl, sizeof(frame.rx_ctrl));
    memcpy(frame.buf, data->buf, frame.len);

    if (xQueueSend(s_csi_queue, &frame, 0) != pdTRUE) {
        s_csi_dropped++;
    }
}

static void csi_print_task(void *arg)
{
    (void)arg;
    csi_frame_t frame;
    csi_mac_counter_t auto_counters[8] = {0};
    uint32_t feature_count = 0;
    uint32_t auto_window_frames = 0;
    uint32_t auto_last_udp_packets = 0;

#if CONFIG_CSI_FORWARD_UDP && !CONFIG_CSI_FORWARD_FIXED_RATE
    struct sockaddr_in forward_addr;
    int forward_sock = udp_forward_open(&forward_addr);
#endif

#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_CSI_DATA
    printf("CSI_HEADER,seq,src_mac,rssi,rate,sig_mode,mcs,cwb,channel,secondary_channel,timestamp,noise_floor,ant,len,first_word_invalid,data\n");
#elif CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_VOFA
    ESP_LOGI(TAG, "VOFA+ mode enabled. Data engine: FireWater. Channels: rssi,mean_amp2,amp2_8,amp2_24,amp2_48,amp2_80");
#else
    ESP_LOGI(TAG, "Serial CSI output disabled");
#endif

    while (true) {
        if (xQueueReceive(s_csi_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        csi_auto_filter_update(auto_counters,
                               sizeof(auto_counters) / sizeof(auto_counters[0]),
                               &frame,
                               &auto_window_frames,
                               &auto_last_udp_packets);

        if (!csi_filter_accept(&frame)) {
            continue;
        }

#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_CSI_DATA
        const wifi_pkt_rx_ctrl_t *rx = &frame.rx_ctrl;
        printf("CSI_DATA,%" PRIu32 "," MACSTR ",%d,%u,%u,%u,%u,%u,%u,%" PRIu32 ",%d,%u,%u,%u,[",
               frame.seq,
               MAC2STR(frame.mac),
               rx->rssi,
               rx->rate,
               rx->sig_mode,
               rx->mcs,
               rx->cwb,
               rx->channel,
               rx->secondary_channel,
               (uint32_t)rx->timestamp,
               rx->noise_floor,
               rx->ant,
               frame.len,
               frame.first_word_invalid ? 1 : 0);

        for (uint16_t i = 0; i < frame.len; i++) {
            printf("%d%s", frame.buf[i], (i + 1 == frame.len) ? "" : ",");
        }
        printf("]\n");
#endif

        if (!csi_frame_has_feature_len(&frame)) {
            continue;
        }

        feature_count++;
        bool serial_feature_due = false;
        bool forward_due = false;

#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_VOFA
#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
        serial_feature_due = false;
#else
        serial_feature_due = ((feature_count % CONFIG_CSI_VOFA_DECIMATE) == 0);
#endif
#endif

#if CONFIG_CSI_FORWARD_UDP
        forward_due = ((feature_count % CONFIG_CSI_FORWARD_DECIMATE) == 0);
#endif

        if (!serial_feature_due && !forward_due) {
            continue;
        }

        char feature_line[128];
        int feature_len = snprintf(feature_line, sizeof(feature_line),
                                   "%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                                   frame.rx_ctrl.rssi,
                                   csi_mean_amp2(&frame),
                                   csi_pair_amp2(&frame, 8),
                                   csi_pair_amp2(&frame, 24),
                                   csi_pair_amp2(&frame, 48),
                                   csi_pair_amp2(&frame, 80));
        if (feature_len <= 0 || feature_len >= (int)sizeof(feature_line)) {
            continue;
        }

        if (serial_feature_due) {
#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_VOFA
            printf("%s", feature_line);
#endif
        }

#if CONFIG_CSI_FORWARD_UDP
#if CONFIG_CSI_FORWARD_FIXED_RATE
        if (forward_due) {
            latest_feature_update(feature_line, feature_len);
        }
#else
        if (forward_due && forward_sock >= 0) {
            int sent = sendto(forward_sock, feature_line, feature_len, 0,
                              (struct sockaddr *)&forward_addr, sizeof(forward_addr));
            if (sent == feature_len) {
                s_forward_packets++;
            } else {
                s_forward_errors++;
            }
        }
#endif
#endif
    }
}

#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
static void csi_forward_task(void *arg)
{
    (void)arg;
    struct sockaddr_in forward_addr;
    int forward_sock = -1;
    uint32_t last_seq = 0;
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "Fixed-rate UDP forward enabled: interval=%d ms target=%s:%d",
             CONFIG_CSI_FORWARD_INTERVAL_MS, CONFIG_CSI_FORWARD_IP,
             CONFIG_CSI_FORWARD_PORT);

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_CSI_FORWARD_INTERVAL_MS));

        if (forward_sock < 0) {
            forward_sock = udp_forward_open(&forward_addr);
            if (forward_sock < 0) {
                s_forward_errors++;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        char feature_line[128];
        uint32_t seq = 0;
        if (!latest_feature_copy(feature_line, sizeof(feature_line), &seq)) {
            continue;
        }

#if CONFIG_CSI_FORWARD_REQUIRE_FRESH
        if (seq == last_seq) {
            continue;
        }
#endif

#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_VOFA
        printf("%s", feature_line);
#endif

        int feature_len = (int)strlen(feature_line);
        int sent = sendto(forward_sock, feature_line, feature_len, 0,
                          (struct sockaddr *)&forward_addr, sizeof(forward_addr));
        if (sent == feature_len) {
            if (seq == last_seq) {
                s_forward_repeated++;
            }
            s_forward_packets++;
            last_seq = seq;
        } else {
            s_forward_errors++;
            close(forward_sock);
            forward_sock = -1;
        }
    }
}
#endif

static void udp_server_task(void *arg)
{
    (void)arg;
    uint8_t rx_buffer[512];

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CSI_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int err = bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP server listening on port %d", CONFIG_CSI_UDP_PORT);

    while (true) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL);
        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        s_udp_packets++;
        s_udp_bytes += len;
    }
}

static void stats_task(void *arg)
{
    (void)arg;

#if CONFIG_CSI_SERIAL_OUTPUT && CONFIG_CSI_OUTPUT_VOFA
    ESP_LOGI(TAG, "Stats task disabled in VOFA mode to keep serial stream numeric-only");
    vTaskDelete(NULL);
    return;
#endif

    uint32_t last_csi = 0;
    uint32_t last_udp = 0;
    uint32_t last_drop = 0;
    uint32_t last_forward = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t csi_now = s_csi_seq;
        uint32_t udp_now = s_udp_packets;
        uint32_t drop_now = s_csi_dropped;
        uint32_t forward_now = s_forward_packets;
        ESP_LOGI(TAG, "STAT csi_hz=%" PRIu32 " udp_hz=%" PRIu32 " csi_total=%" PRIu32
                      " udp_total=%" PRIu32 " forward_hz=%" PRIu32 " forward_total=%" PRIu32
                      " forward_err=%" PRIu32 " filter_drop=%" PRIu32
                      " dropped_total=%" PRIu32 " dropped_hz=%" PRIu32,
                 csi_now - last_csi,
                 udp_now - last_udp,
                 csi_now,
                 udp_now,
                 forward_now - last_forward,
                 forward_now,
                 s_forward_errors,
                 s_filter_dropped,
                 drop_now,
                 drop_now - last_drop);

#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
        ESP_LOGI(TAG, "STAT fixed_forward feature_updates=%" PRIu32
                      " repeated_total=%" PRIu32,
                 s_feature_updates,
                 s_forward_repeated);
#endif

        last_csi = csi_now;
        last_udp = udp_now;
        last_drop = drop_now;
        last_forward = forward_now;
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
#if CONFIG_CSI_STA_ENABLE
    esp_netif_create_default_wifi_sta();
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
#if CONFIG_CSI_STA_ENABLE
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
#endif

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CONFIG_CSI_WIFI_SSID,
            .ssid_len = strlen(CONFIG_CSI_WIFI_SSID),
            .channel = CONFIG_CSI_WIFI_CHANNEL,
            .password = CONFIG_CSI_WIFI_PASSWORD,
            .max_connection = CONFIG_CSI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(CONFIG_CSI_WIFI_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

#if CONFIG_CSI_STA_ENABLE
    wifi_config_t sta_config = {
        .sta = {
            .ssid = CONFIG_CSI_STA_SSID,
            .password = CONFIG_CSI_STA_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(CONFIG_CSI_STA_PASSWORD) == 0) {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(
#if CONFIG_CSI_STA_ENABLE
        WIFI_MODE_APSTA
#else
        WIFI_MODE_AP
#endif
    ));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
#if CONFIG_CSI_STA_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP,
                                          WIFI_PROTOCOL_11B |
                                          WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));
#if CONFIG_CSI_STA_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                          WIFI_PROTOCOL_11B |
                                          WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
#if CONFIG_CSI_STA_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started SSID=%s channel=%d IP=192.168.4.1",
             CONFIG_CSI_WIFI_SSID, CONFIG_CSI_WIFI_CHANNEL);
#if CONFIG_CSI_STA_ENABLE
    ESP_LOGI(TAG, "AP+STA enabled. Set CSI_FORWARD_IP to the PC IP on router LAN for final deployment.");
#endif
}

static void csi_init(void)
{
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI enabled, max_len=%d queue=%d", CONFIG_CSI_MAX_LEN, CONFIG_CSI_QUEUE_SIZE);
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
    ESP_LOGI(TAG, "ESP32-S3 WiFi CSI RX starting");
    ESP_LOGI(TAG, "Open TX project on the other ESP32-S3 and connect to SSID=%s",
             CONFIG_CSI_WIFI_SSID);

    nvs_init();

    s_csi_queue = xQueueCreate(CONFIG_CSI_QUEUE_SIZE, sizeof(csi_frame_t));
    if (!s_csi_queue) {
        ESP_LOGE(TAG, "Failed to create CSI queue");
        return;
    }

    csi_filter_init();
    wifi_init_softap();
    csi_init();

    xTaskCreate(csi_print_task, "csi_print", 8192, NULL, 4, NULL);
#if CONFIG_CSI_FORWARD_UDP && CONFIG_CSI_FORWARD_FIXED_RATE
    xTaskCreate(csi_forward_task, "csi_forward", 4096, NULL, 4, NULL);
#endif
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, NULL);
}
