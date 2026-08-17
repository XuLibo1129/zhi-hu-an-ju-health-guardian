#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_audio_input.h"
#include "app_cloud_asr.h"
#include "app_cloud_context.h"
#include "app_cloud_llm.h"
#include "app_cloud_tts.h"
#include "app_csi_receiver.h"
#include "app_k230_vision.h"
#include "app_network.h"
#include "app_radar.h"
#include "app_vitals_history.h"
#include "app_weather.h"
#include "app_voice_output.h"

static const char *TAG = "voice_agent_ui";

#ifndef CONFIG_APP_SIM800_ENABLE
#define CONFIG_APP_SIM800_ENABLE 0
#endif

#ifndef CONFIG_APP_SIM800_EMERGENCY_PHONE
#define CONFIG_APP_SIM800_EMERGENCY_PHONE ""
#endif

#ifndef CONFIG_APP_SIM800_AUTO_SMS_ENABLE
#define CONFIG_APP_SIM800_AUTO_SMS_ENABLE 0
#endif

#ifndef CONFIG_APP_SIM800_AUTO_CALL_ENABLE
#define CONFIG_APP_SIM800_AUTO_CALL_ENABLE 0
#endif

#define APP_EMERGENCY_BUTTONS_ENABLE CONFIG_APP_SIM800_ENABLE

#if CONFIG_APP_SIM800_ENABLE
#include "app_sim800.h"
#endif

#define C_BG       0xF7F9FC
#define C_SURFACE  0xFFFFFF
#define C_BORDER   0xE5EAF2
#define C_TEXT     0x172033
#define C_MUTED    0x667085
#define C_BLUE     0x2F6BFF
#define C_GREEN    0x0FA36B
#define C_AMBER    0xD08B00
#define C_RED      0xD92D20
#define C_HEART    0xE5486D
#define C_BREATH   0x1B8A7A

#define AI_ADVICE_TTS_MAX_BYTES 420
#define FALL_FUSION_ALERT_COOLDOWN_MS 15000
#define FALL_FUSION_PRIMARY_SCORE 45
#define FALL_FUSION_AUX_SCORE 20
#define FALL_FUSION_THRESHOLD 65
#define FALL_POPUP_AUTO_CLOSE_MS 4200
#define NODE_STATUS_STALE_MS 10000
#define FALL_ALERT_TONE_COUNT 10
#define FALL_ALERT_TONE_MS 260
#define FALL_ALERT_TONE_GAP_MS 140
#define FALL_ALERT_SOUND_WAIT_TIMEOUT_MS 5500
#define FALL_CELLULAR_SETTLE_MS 200

#define TXT_NET_CONNECTED "\xE7\xBD\x91\xE7\xBB\x9C\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5  %s"
#define TXT_NET_MISSING    "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x9C\xAA\xE9\x85\x8D\xE7\xBD\xAE"
#define TXT_NET_CONNECTING "\xE7\xBD\x91\xE7\xBB\x9C\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD"
#define TXT_NET_RECONNECT  "\xE7\xBD\x91\xE7\xBB\x9C\xE9\x87\x8D\xE8\xBF\x9E\xE4\xB8\xAD"
#define TXT_NET_DISABLED   "\xE7\xBD\x91\xE7\xBB\x9C\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"
#define TXT_NET_WAITING    "\xE7\xBD\x91\xE7\xBB\x9C\xE7\xAD\x89\xE5\xBE\x85\xE4\xB8\xAD"
#define TXT_LISTENING      "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\x81\x86\xE5\x90\xAC"
#define TXT_RELEASE_ASR    "\xE6\x9D\xBE\xE5\xBC\x80\xE8\xAF\x86\xE5\x88\xAB"
#define TXT_MIC_ERROR      "\xE9\xBA\xA6\xE5\x85\x8B\xE9\xA3\x8E\xE9\x94\x99\xE8\xAF\xAF"
#define TXT_HOLD_TALK      "\xE6\x8C\x89\xE4\xBD\x8F\xE8\xAF\xB4\xE8\xAF\x9D"
#define TXT_ASR            "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xAF\x86\xE5\x88\xAB"
#define TXT_WAIT           "\xE8\xAF\xB7\xE7\xA8\x8D\xE7\xAD\x89"
#define TXT_ASR_FALLBACK   "\xE5\xBD\x95\xE9\x9F\xB3\xE6\x97\xB6\xE9\x95\xBF %u \xE6\xAF\xAB\xE7\xA7\x92\xEF\xBC\x8C\xE4\xBD\x86\xE8\xAF\xAD\xE9\x9F\xB3\xE8\xAF\x86\xE5\x88\xAB\xE6\x9A\x82\xE6\x97\xB6\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8\xE3\x80\x82\xE8\xAF\xB7\xE7\x94\xA8\xE4\xB8\xAD\xE6\x96\x87\xE6\xB8\xA9\xE5\x92\x8C\xE5\x9C\xB0\xE8\xAF\xA2\xE9\x97\xAE\xE8\x80\x81\xE4\xBA\xBA\xE9\x9C\x80\xE8\xA6\x81\xE4\xBB\x80\xE4\xB9\x88\xE5\xB8\xAE\xE5\x8A\xA9\xE3\x80\x82"
#define TXT_NET_NOT_READY  "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x9C\xAA\xE5\xB0\xB1\xE7\xBB\xAA"
#define TXT_THINKING       "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x80\x9D\xE8\x80\x83"
#define TXT_REPEAT         "\xE6\x88\x91\xE5\x9C\xA8\xE5\x91\xA2\xEF\xBC\x8C\xE8\xAF\xB7\xE5\x86\x8D\xE8\xAF\xB4\xE4\xB8\x80\xE9\x81\x8D\xE3\x80\x82"
#define TXT_SPEAKING       "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x9B\x9E\xE7\xAD\x94"
#define TXT_READY          "\xE5\x87\x86\xE5\xA4\x87\xE5\xB0\xB1\xE7\xBB\xAA"
#define TXT_PLAY_ERROR     "\xE6\x92\xAD\xE6\x94\xBE\xE9\x94\x99\xE8\xAF\xAF"
#define TXT_BUSY           "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\xA4\x84\xE7\x90\x86"
#define TXT_TASK_ERROR     "\xE4\xBB\xBB\xE5\x8A\xA1\xE5\xA4\xB1\xE8\xB4\xA5"
#define TXT_PROCESSING     "\xE5\xA4\x84\xE7\x90\x86\xE4\xB8\xAD"
#define TXT_ANALYZING      "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90"
#define TXT_AI_ADVICE      "AI\xE5\xBB\xBA\xE8\xAE\xAE"
#define TXT_ONE_TAP        "\xE4\xB8\x80\xE9\x94\xAE\xE6\x92\xAD\xE6\x8A\xA5"
#define TXT_PHONE_HELP     "\xE7\x94\xB5\xE8\xAF\x9D\xE6\xB1\x82\xE5\x8A\xA9"
#define TXT_SMS_HELP       "\xE7\x9F\xAD\xE4\xBF\xA1\xE6\xB1\x82\xE5\x8A\xA9"
#define TXT_ONE_TAP_CALL   "\xE4\xB8\x80\xE9\x94\xAE\xE6\x8B\xA8\xE6\x89\x93"
#define TXT_ONE_TAP_SEND   "\xE4\xB8\x80\xE9\x94\xAE\xE5\x8F\x91\xE9\x80\x81"
#define TXT_CALLING        "\xE6\x8B\xA8\xE5\x8F\xB7\xE4\xB8\xAD"
#define TXT_SENDING        "\xE5\x8F\x91\xE9\x80\x81\xE4\xB8\xAD"
#define TXT_CALLED         "\xE5\xB7\xB2\xE6\x8B\xA8\xE5\x8F\xB7"
#define TXT_SENT           "\xE5\xB7\xB2\xE5\x8F\x91\xE9\x80\x81"
#define TXT_HELP_FAILED    "\xE6\xB1\x82\xE5\x8A\xA9\xE5\xA4\xB1\xE8\xB4\xA5"
#define TXT_PHONE_EMPTY    "\xE5\x8F\xB7\xE7\xA0\x81\xE6\x9C\xAA\xE5\xA1\xAB"
#define TXT_SIM_WAIT       "\xE6\xA8\xA1\xE5\x9D\x97\xE7\xAD\x89\xE5\xBE\x85"
#define TXT_MANUAL_HELP    "\xE6\x89\x8B\xE5\x8A\xA8\xE6\xB1\x82\xE5\x8A\xA9"
#define TXT_AUTO_HELP      "\xE8\x87\xAA\xE5\x8A\xA8\xE6\x8A\xA5\xE8\xAD\xA6"
#define TXT_FALL_ALERT     "\xE6\xA3\x80\xE6\xB5\x8B\xE5\x88\xB0\xE8\xB7\x8C\xE5\x80\x92"
#define TXT_VITAL_ALERT    "\xE7\x94\x9F\xE5\x91\xBD\xE4\xBD\x93\xE5\xBE\x81\xE5\xBC\x82\xE5\xB8\xB8"
#define TXT_DATA_MISSING   "\xE6\x95\xB0\xE6\x8D\xAE\xE6\x9C\xAA\xE7\xA8\xB3"
#define TXT_ADVICE_FALLBACK "\xE6\x9A\x82\xE6\x97\xB6\xE6\x97\xA0\xE6\xB3\x95\xE8\xBF\x9E\xE6\x8E\xA5" "AI" "\xEF\xBC\x8C\xE8\xAF\xB7\xE4\xBF\x9D\xE6\x8C\x81\xE8\xA7\x84\xE5\xBE\x8B\xE4\xBD\x9C\xE6\x81\xAF\xE3\x80\x81\xE9\x80\x82\xE9\x87\x8F\xE9\xA5\xAE\xE6\xB0\xB4\xEF\xBC\x8C\xE5\xA4\x96\xE5\x87\xBA\xE5\x89\x8D\xE5\x85\xB3\xE6\xB3\xA8\xE5\xA4\xA9\xE6\xB0\x94\xEF\xBC\x9B\xE5\xA6\x82\xE6\x9C\x89\xE6\x98\x8E\xE6\x98\xBE\xE4\xB8\x8D\xE9\x80\x82\xEF\xBC\x8C\xE8\xAF\xB7\xE5\x8F\x8A\xE6\x97\xB6\xE8\x81\x94\xE7\xB3\xBB\xE5\xAE\xB6\xE4\xBA\xBA\xE6\x88\x96\xE5\x8C\xBB\xE7\x94\x9F\xE3\x80\x82"
#define TXT_HEART_TITLE    "\xE5\xBD\x93\xE5\x89\x8D\xE5\xBF\x83\xE7\x8E\x87"
#define TXT_BREATH_TITLE   "\xE5\x91\xBC\xE5\x90\xB8\xE7\x8E\x87"
#define TXT_UNIT_BPM       "\xE6\xAC\xA1/\xE5\x88\x86"
#define TXT_HISTORY_TITLE  "\xE8\xBF\x91\xE4\xBA\x94\xE5\xA4\xA9\xE5\xB9\xB3\xE5\x9D\x87"
#define TXT_HISTORY_FMT    "%s  \xE5\xBF\x83%s  \xE5\x91\xBC%s"
#define TXT_TIME_WAIT      "\xE6\x97\xB6\xE9\x97\xB4\xE5\x90\x8C\xE6\xAD\xA5\xE4\xB8\xAD"
#define TXT_WEATHER_WAIT   "\xE5\xA4\xA9\xE6\xB0\x94\xE8\x8E\xB7\xE5\x8F\x96\xE4\xB8\xAD"
#define TXT_HUMIDITY_FMT   "\xE6\xB9\xBF\xE5\xBA\xA6 %d%%"
#define TXT_FALL_POPUP     "\xE6\x82\xA8\xE5\xB7\xB2\xE6\x91\x94\xE5\x80\x92\xEF\xBC\x8C\xE5\xB7\xB2\xE4\xB8\xBA\xE6\x82\xA8\xE5\x91\xBC\xE6\x95\x91"

LV_FONT_DECLARE(ui_font_cn_20);

#if CONFIG_LV_FONT_MONTSERRAT_48
#define FONT_VALUE &lv_font_montserrat_48
#elif CONFIG_LV_FONT_MONTSERRAT_28
#define FONT_VALUE &lv_font_montserrat_28
#else
#define FONT_VALUE LV_FONT_DEFAULT
#endif

#if CONFIG_LV_FONT_MONTSERRAT_28
#define FONT_DATE &lv_font_montserrat_28
#else
#define FONT_DATE FONT_VALUE
#endif

#define FONT_CN &ui_font_cn_20
#define FONT_CN_SMALL FONT_CN

static lv_obj_t *s_network_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_weekday_label;
static lv_obj_t *s_weather_temp_label;
static lv_obj_t *s_weather_unit_label;
static lv_obj_t *s_weather_desc_label;
static lv_obj_t *s_weather_meta_label;
static lv_obj_t *s_fall_alert_popup;
static lv_obj_t *s_heart_value_label;
static lv_obj_t *s_breath_value_label;
static lv_obj_t *s_history_status_label;
static lv_obj_t *s_history_day_labels[APP_VITALS_HISTORY_DAYS];
static lv_obj_t *s_voice_status_label;
static lv_obj_t *s_hold_button;
static lv_obj_t *s_hold_label;
static lv_obj_t *s_advice_button;
static lv_obj_t *s_advice_label;
static lv_obj_t *s_advice_status_label;
#if APP_EMERGENCY_BUTTONS_ENABLE
static lv_obj_t *s_call_button;
static lv_obj_t *s_call_label;
static lv_obj_t *s_call_status_label;
static lv_obj_t *s_sms_button;
static lv_obj_t *s_sms_label;
static lv_obj_t *s_sms_status_label;
#endif
static volatile bool s_record_stop_requested;
static bool s_pipeline_running;
#if APP_EMERGENCY_BUTTONS_ENABLE
static bool s_emergency_running;
#endif
static bool s_button_down;
static bool s_fusion_fall_latched;
static volatile bool s_fall_alert_sound_running;
static int64_t s_last_fall_alert_ms;
#if CONFIG_APP_SIM800_ENABLE && (CONFIG_APP_SIM800_AUTO_SMS_ENABLE || CONFIG_APP_SIM800_AUTO_CALL_ENABLE)
static TaskHandle_t s_auto_alert_task_handle;
#endif

static lv_color_t hex(uint32_t color)
{
    return lv_color_hex(color);
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    return label;
}

static void set_talk_state(const char *state, const char *button, uint32_t color)
{
    lv_label_set_text(s_voice_status_label, state);
    lv_obj_set_style_text_color(s_voice_status_label, hex(color), LV_PART_MAIN);
    lv_label_set_text(s_hold_label, button);
    lv_obj_set_style_border_color(s_hold_button, hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hold_button, hex(color), LV_STATE_PRESSED);
}

static void set_advice_state(const char *state, const char *button, uint32_t color)
{
    lv_label_set_text(s_advice_status_label, state);
    lv_obj_set_style_text_color(s_advice_status_label, hex(color), LV_PART_MAIN);
    lv_label_set_text(s_advice_label, button);
    lv_obj_set_style_border_color(s_advice_button, hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_advice_button, hex(color), LV_STATE_PRESSED);
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static void set_call_state(const char *state, const char *button, uint32_t color)
{
    if (s_call_status_label == NULL || s_call_label == NULL || s_call_button == NULL) {
        return;
    }

    lv_label_set_text(s_call_status_label, state);
    lv_obj_set_style_text_color(s_call_status_label, hex(color), LV_PART_MAIN);
    lv_label_set_text(s_call_label, button);
    lv_obj_set_style_border_color(s_call_button, hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_call_button, hex(color), LV_STATE_PRESSED);
}

static void set_sms_state(const char *state, const char *button, uint32_t color)
{
    if (s_sms_status_label == NULL || s_sms_label == NULL || s_sms_button == NULL) {
        return;
    }

    lv_label_set_text(s_sms_status_label, state);
    lv_obj_set_style_text_color(s_sms_status_label, hex(color), LV_PART_MAIN);
    lv_label_set_text(s_sms_label, button);
    lv_obj_set_style_border_color(s_sms_button, hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sms_button, hex(color), LV_STATE_PRESSED);
}
#endif

static const char *network_status_text_cn(void)
{
    static char text[96];
    const char *raw = app_network_status_text();

    if (app_network_is_ready()) {
        const char *ip = raw;
        if (strncmp(raw, "NET: ", 5) == 0) {
            ip = raw + 5;
        }
        snprintf(text, sizeof(text), TXT_NET_CONNECTED, ip);
        return text;
    }

    if (strstr(raw, "SSID") != NULL) {
        return TXT_NET_MISSING;
    }
    if (strstr(raw, "connecting") != NULL) {
        return TXT_NET_CONNECTING;
    }
    if (strstr(raw, "reconnect") != NULL) {
        return TXT_NET_RECONNECT;
    }
    if (strstr(raw, "disabled") != NULL) {
        return TXT_NET_DISABLED;
    }
    return TXT_NET_WAITING;
}

static const char *weekday_text_cn(int wday)
{
    static const char *weekdays[] = {
        "\xE5\x91\xA8\xE6\x97\xA5",
        "\xE5\x91\xA8\xE4\xB8\x80",
        "\xE5\x91\xA8\xE4\xBA\x8C",
        "\xE5\x91\xA8\xE4\xB8\x89",
        "\xE5\x91\xA8\xE5\x9B\x9B",
        "\xE5\x91\xA8\xE4\xBA\x94",
        "\xE5\x91\xA8\xE5\x85\xAD",
    };

    if (wday < 0 || wday > 6) {
        return "--";
    }
    return weekdays[wday];
}

static void update_time_labels(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year + 1900 < 2024) {
        lv_label_set_text(s_time_label, "--:--");
        lv_label_set_text(s_date_label, "---- -- --");
        lv_label_set_text(s_weekday_label, TXT_TIME_WAIT);
        return;
    }

    char time_text[16];
    char date_text[40];
    snprintf(time_text, sizeof(time_text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_date_label, date_text);
    lv_label_set_text(s_weekday_label, weekday_text_cn(tm_now.tm_wday));
}

static void update_weather_labels(void)
{
    app_weather_info_t weather = {0};
    app_weather_get_latest(&weather);

    if (!weather.valid) {
        lv_label_set_text(s_weather_temp_label, "--");
        lv_label_set_text(s_weather_unit_label, "\xE2\x84\x83");
        lv_label_set_text(s_weather_desc_label, TXT_WEATHER_WAIT);
        lv_label_set_text(s_weather_meta_label, app_weather_status_text());
        lv_obj_set_style_text_color(s_weather_meta_label, hex(C_AMBER), LV_PART_MAIN);
        return;
    }

    char temp_text[12];
    char desc_text[80];
    char meta_text[24];
    snprintf(temp_text, sizeof(temp_text), "%d", weather.temperature_c);
    snprintf(desc_text, sizeof(desc_text), "%s\xE5\xA4\xA9\xE6\xB0\x94  %s", weather.city, weather.condition);
    snprintf(meta_text, sizeof(meta_text), TXT_HUMIDITY_FMT, weather.humidity);

    lv_label_set_text(s_weather_temp_label, temp_text);
    lv_label_set_text(s_weather_unit_label, "\xE2\x84\x83");
    lv_label_set_text(s_weather_desc_label, desc_text);
    lv_label_set_text(s_weather_meta_label, meta_text);
    lv_obj_set_style_text_color(s_weather_meta_label, hex(C_MUTED), LV_PART_MAIN);
}

static void fall_alert_sound_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "fall alarm audio start: dma_free=%u largest_dma=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    for (int i = 0; i < FALL_ALERT_TONE_COUNT; ++i) {
        esp_err_t ret = app_voice_output_play_alarm_tone(FALL_ALERT_TONE_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "fall alert sound failed: %s", esp_err_to_name(ret));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(FALL_ALERT_TONE_GAP_MS));
    }

    ESP_LOGI(TAG, "fall alarm audio done: stack_free=%u dma_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    s_fall_alert_sound_running = false;
    vTaskDelete(NULL);
}

static void start_fall_alert_sound(void)
{
    if (s_fall_alert_sound_running) {
        return;
    }

    s_fall_alert_sound_running = true;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(fall_alert_sound_task,
                                                    "fall_alarm",
                                                    6144,
                                                    NULL,
                                                    2,
                                                    NULL,
                                                    1,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_fall_alert_sound_running = false;
        ESP_LOGW(TAG, "Failed to create fall alert sound task");
    }
}

static void close_fall_alert_popup(void)
{
    if (s_fall_alert_popup != NULL) {
        lv_obj_delete(s_fall_alert_popup);
        s_fall_alert_popup = NULL;
    }
}

static void fall_popup_close_timer_cb(lv_timer_t *timer)
{
    close_fall_alert_popup();
    lv_timer_delete(timer);
}

static void show_fall_alert_popup(void)
{
    close_fall_alert_popup();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *overlay = lv_obj_create(screen);
    s_fall_alert_popup = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 560, 220);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 28, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, hex(0x8FA4C7), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *message = label_create(card, TXT_FALL_POPUP, C_RED, FONT_CN);
    lv_obj_set_width(message, 480);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);

    lv_timer_t *timer = lv_timer_create(fall_popup_close_timer_cb, FALL_POPUP_AUTO_CLOSE_MS, NULL);
    if (timer != NULL) {
        lv_timer_set_repeat_count(timer, 1);
    }
}

static void handle_fusion_fall_alert(void)
{
    app_k230_vision_sample_t vision = {0};
    app_csi_receiver_sample_t csi = {0};
    app_csi_receiver_sample_t ld6002c = {0};

    app_k230_vision_get_latest(&vision);
    app_csi_receiver_get_csi_latest(&csi);
    app_csi_receiver_get_ld6002c_latest(&ld6002c);

    int64_t now = esp_timer_get_time() / 1000;
    bool vision_alarm = vision.valid && vision.last_update_ms > 0 &&
                        (now - vision.last_update_ms) <= NODE_STATUS_STALE_MS &&
                        vision.fall_detected;
    bool ld6002c_alarm = ld6002c.valid && ld6002c.last_update_ms > 0 &&
                         (now - ld6002c.last_update_ms) <= NODE_STATUS_STALE_MS &&
                         ld6002c.alarm;
    bool csi_alarm = csi.valid && csi.last_update_ms > 0 &&
                     (now - csi.last_update_ms) <= NODE_STATUS_STALE_MS &&
                     csi.alarm;

    int fusion_score = 0;
    if (vision_alarm) {
        fusion_score += FALL_FUSION_PRIMARY_SCORE;
    }
    if (ld6002c_alarm) {
        fusion_score += FALL_FUSION_PRIMARY_SCORE;
    }
    if (csi_alarm) {
        fusion_score += FALL_FUSION_AUX_SCORE;
    }

    bool fusion_alarm = fusion_score >= FALL_FUSION_THRESHOLD;
    if (!fusion_alarm) {
        s_fusion_fall_latched = false;
        return;
    }

    if (s_fusion_fall_latched) {
        return;
    }

    if (s_last_fall_alert_ms > 0 &&
        now - s_last_fall_alert_ms < FALL_FUSION_ALERT_COOLDOWN_MS) {
        s_fusion_fall_latched = true;
        return;
    }

    s_fusion_fall_latched = true;
    s_last_fall_alert_ms = now;
    ESP_LOGW(TAG, "fusion fall alert: score=%d vision=%d ld6002c=%d csi=%d",
             fusion_score, vision_alarm, ld6002c_alarm, csi_alarm);
    show_fall_alert_popup();
    start_fall_alert_sound();
#if CONFIG_APP_SIM800_ENABLE && (CONFIG_APP_SIM800_AUTO_SMS_ENABLE || CONFIG_APP_SIM800_AUTO_CALL_ENABLE)
    if (s_auto_alert_task_handle != NULL) {
        xTaskNotifyGive(s_auto_alert_task_handle);
    }
#endif
}

static void update_vital_label(lv_obj_t *label, bool has_value, float value)
{
    if (has_value && value > 0.0f) {
        char text[12];
        int rounded = (int)(value + 0.5f);
        snprintf(text, sizeof(text), "%d", rounded);
        lv_label_set_text(label, text);
    } else {
        lv_label_set_text(label, "--");
    }
}

static const char *compact_day_text(const char *day, char *buf, size_t buf_size)
{
    if (day == NULL || day[0] == '\0') {
        return "--";
    }

    if (strlen(day) == 10 && day[4] == '-' && day[7] == '-') {
        snprintf(buf, buf_size, "%c%c-%c%c", day[5], day[6], day[8], day[9]);
        return buf;
    }

    return day;
}

static void update_history_labels(void)
{
    app_vitals_history_day_t days[APP_VITALS_HISTORY_DAYS] = {0};
    size_t count = app_vitals_history_get_recent(days, APP_VITALS_HISTORY_DAYS);

    if (s_history_status_label != NULL) {
        lv_label_set_text(s_history_status_label,
                          app_vitals_history_sd_ready() ? "SD OK" : "SD --");
        lv_obj_set_style_text_color(s_history_status_label,
                                    hex(app_vitals_history_sd_ready() ? C_GREEN : C_AMBER),
                                    LV_PART_MAIN);
    }

    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        if (s_history_day_labels[i] == NULL) {
            continue;
        }

        if (i >= count) {
            lv_label_set_text(s_history_day_labels[i], "--");
            continue;
        }

        char day_buf[8] = {0};
        char heart_buf[8] = "--";
        char breath_buf[8] = "--";
        char text[48] = {0};
        const char *day_text = compact_day_text(days[i].day, day_buf, sizeof(day_buf));

        if (days[i].has_heart_bpm) {
            snprintf(heart_buf, sizeof(heart_buf), "%u", (unsigned)days[i].heart_bpm);
        }
        if (days[i].has_breath_bpm) {
            snprintf(breath_buf, sizeof(breath_buf), "%u", (unsigned)days[i].breath_bpm);
        }

        snprintf(text, sizeof(text), TXT_HISTORY_FMT, day_text, heart_buf, breath_buf);
        lv_label_set_text(s_history_day_labels[i], text);
    }
}

static void update_runtime_labels(void)
{
    app_radar_sample_t sample;
    app_radar_get_latest(&sample);

    lv_label_set_text(s_network_label, network_status_text_cn());
    update_time_labels();
    update_weather_labels();
    handle_fusion_fall_alert();
    update_vital_label(s_heart_value_label, sample.has_heart_bpm, sample.heart_bpm);
    update_vital_label(s_breath_value_label, sample.has_breath_bpm, sample.breath_bpm);
    update_history_labels();

#if APP_EMERGENCY_BUTTONS_ENABLE
    if (!s_emergency_running) {
        if (CONFIG_APP_SIM800_EMERGENCY_PHONE[0] == '\0') {
            set_call_state(TXT_PHONE_EMPTY, TXT_PHONE_HELP, C_AMBER);
            set_sms_state(TXT_PHONE_EMPTY, TXT_SMS_HELP, C_AMBER);
        } else if (!app_sim800_is_ready()) {
            set_call_state(TXT_SIM_WAIT, TXT_PHONE_HELP, C_AMBER);
            set_sms_state(TXT_SIM_WAIT, TXT_SMS_HELP, C_AMBER);
        } else {
            set_call_state(TXT_ONE_TAP_CALL, TXT_PHONE_HELP, C_RED);
            set_sms_state(TXT_ONE_TAP_SEND, TXT_SMS_HELP, C_BLUE);
        }
    }
#endif
}

static bool is_blank_text(const char *text)
{
    if (text == NULL) {
        return true;
    }

    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text;
        if (ch > ' ') {
            return false;
        }
        text++;
    }

    return true;
}

static void set_stage_from_task(const char *state, const char *button, uint32_t color)
{
    bsp_display_lock(0);
    set_talk_state(state, button, color);
    update_runtime_labels();
    bsp_display_unlock();
}

static void set_advice_stage_from_task(const char *state, const char *button, uint32_t color)
{
    bsp_display_lock(0);
    set_advice_state(state, button, color);
    update_runtime_labels();
    bsp_display_unlock();
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static void set_call_stage_from_task(const char *state, const char *button, uint32_t color)
{
    bsp_display_lock(0);
    set_call_state(state, button, color);
    bsp_display_unlock();
}

static void set_sms_stage_from_task(const char *state, const char *button, uint32_t color)
{
    bsp_display_lock(0);
    set_sms_state(state, button, color);
    bsp_display_unlock();
}
#endif

static void runtime_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_runtime_labels();
}

static void network_start_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_err_t ret = app_network_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "app_network_start failed: %s", esp_err_to_name(ret));
    } else {
        ret = app_csi_receiver_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "app_csi_receiver_start failed: %s", esp_err_to_name(ret));
        }
    }

    vTaskDeleteWithCaps(NULL);
}

static void format_vital_text(char *out, size_t out_size, bool has_value, float value)
{
    if (has_value && value > 0.0f) {
        snprintf(out, out_size, "%d", (int)(value + 0.5f));
    } else {
        strlcpy(out, "\xE6\x9C\xAA\xE7\xA8\xB3\xE5\xAE\x9A", out_size);
    }
}

static void truncate_utf8_inplace(char *text, size_t max_bytes)
{
    if (text == NULL || max_bytes == 0) {
        return;
    }

    const size_t len = strlen(text);
    if (len <= max_bytes) {
        return;
    }

    size_t cut = max_bytes;
    while (cut > 0 && (((uint8_t)text[cut]) & 0xC0) == 0x80) {
        cut--;
    }
    text[cut] = '\0';
}

static void build_ai_advice_prompt(char *out, size_t out_size)
{
    app_radar_sample_t sample = {0};
    app_weather_info_t weather = {0};
    char heart[16] = {0};
    char breath[16] = {0};
    char weather_text[96] = "\xE5\x8D\x97\xE4\xBA\xAC\xE5\xB8\x82\xE6\xB1\x9F\xE5\x8C\x97\xE6\x96\xB0\xE5\x8C\xBA"
                            "\xE5\xA4\xA9\xE6\xB0\x94\xE6\x9A\x82\xE6\x9C\xAA\xE8\x8E\xB7\xE5\x8F\x96";

    app_radar_get_latest(&sample);
    app_weather_get_latest(&weather);
    format_vital_text(heart, sizeof(heart), sample.has_heart_bpm, sample.heart_bpm);
    format_vital_text(breath, sizeof(breath), sample.has_breath_bpm, sample.breath_bpm);

    if (weather.valid) {
        snprintf(weather_text, sizeof(weather_text), "%s%s %d\xE2\x84\x83 \xE6\xB9\xBF\xE5\xBA\xA6%d%%",
                 weather.city, weather.condition, weather.temperature_c, weather.humidity);
    }

    snprintf(out, out_size,
             "\xE8\x80\x81\xE4\xBA\xBA\xE5\xBD\x93\xE5\x89\x8D\xE5\x81\xA5\xE5\xBA\xB7\xE7\xBB\x88\xE7\xAB\xAF\xE6\x95\xB0\xE6\x8D\xAE\xEF\xBC\x9A"
              "\xE5\xBF\x83\xE7\x8E\x87%s\xE6\xAC\xA1/\xE5\x88\x86\xEF\xBC\x8C"
              "\xE5\x91\xBC\xE5\x90\xB8\xE7\x8E\x87%s\xE6\xAC\xA1/\xE5\x88\x86\xEF\xBC\x8C"
              "\xE5\xA4\xA9\xE6\xB0\x94%s\xE3\x80\x82"
              "\xE8\xAF\xB7\xE7\x94\xA8\xE4\xB8\xAD\xE6\x96\x87\xE8\x87\xAA\xE7\x84\xB6\xE5\x9B\x9E\xE7\xAD\x94\xEF\xBC\x8C"
              "\xE8\xAF\xB4\xE6\x88\x90" "2-3\xE5\x8F\xA5\xEF\xBC\x8C"
              "\xE7\xBA\xA6" "80-120\xE5\xAD\x97\xEF\xBC\x8C"
              "\xE5\x85\x88\xE8\xAF\xB4\xE5\xBD\x93\xE5\x89\x8D\xE7\x8A\xB6\xE6\x80\x81\xEF\xBC\x8C"
              "\xE5\x86\x8D\xE7\xBB\x99\xE5\x81\xA5\xE5\xBA\xB7\xE5\xBB\xBA\xE8\xAE\xAE\xE5\x92\x8C\xE5\x87\xBA\xE8\xA1\x8C\xE5\xBB\xBA\xE8\xAE\xAE\xEF\xBC\x8C"
              "\xE4\xB8\x8D\xE8\xA6\x81\xE8\xAF\x8A\xE6\x96\xAD\xEF\xBC\x8C\xE4\xB8\x8D\xE8\xA6\x81\xE5\x83\x8F\xE6\xA8\xA1\xE6\x9D\xBF\xE3\x80\x82"
              " If one vital sign is unavailable, still give useful advice from the available vital sign and weather; do not say all data is incomplete.",
              heart, breath, weather_text);
}

static void format_local_time_context(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year + 1900 < 2024) {
        strlcpy(out, "time not synchronized", out_size);
        return;
    }

    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d %s",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, weekday_text_cn(tm_now.tm_wday));
}

#if CONFIG_APP_SIM800_ENABLE
static void build_emergency_sms_text(char *out, size_t out_size, const char *reason)
{
    app_radar_sample_t sample = {0};
    char heart[16] = {0};
    char breath[16] = {0};
    char local_time[64] = {0};

    app_radar_get_latest(&sample);
    format_vital_text(heart, sizeof(heart), sample.has_heart_bpm, sample.heart_bpm);
    format_vital_text(breath, sizeof(breath), sample.has_breath_bpm, sample.breath_bpm);
    format_local_time_context(local_time, sizeof(local_time));

    snprintf(out, out_size,
             "\xE8\x80\x81\xE4\xBA\xBA\xE6\xB1\x82\xE5\x8A\xA9:%s;"
             "\xE5\xBF\x83\xE7\x8E\x87%s;"
             "\xE5\x91\xBC\xE5\x90\xB8%s;"
             "%s",
             reason != NULL ? reason : TXT_MANUAL_HELP,
             heart, breath, local_time);
}
#endif

static void build_voice_chat_prompt(const char *transcript, char *out, size_t out_size)
{
    app_radar_sample_t sample = {0};
    app_weather_info_t weather = {0};
    char heart[16] = {0};
    char breath[16] = {0};
    char local_time[64] = {0};
    char weather_text[128] = "weather not available";
    char realtime[1024] = {0};
    char realtime_section[1200] = {0};

    app_radar_get_latest(&sample);
    app_weather_get_latest(&weather);
    format_vital_text(heart, sizeof(heart), sample.has_heart_bpm, sample.heart_bpm);
    format_vital_text(breath, sizeof(breath), sample.has_breath_bpm, sample.breath_bpm);
    format_local_time_context(local_time, sizeof(local_time));

    if (weather.valid) {
        snprintf(weather_text, sizeof(weather_text), "%s %s %d C humidity %d%%",
                 weather.city, weather.condition, weather.temperature_c, weather.humidity);
    }

    if (app_cloud_context_needs_realtime(transcript)) {
        esp_err_t ctx_ret = app_cloud_context_fetch_realtime(transcript, realtime, sizeof(realtime));
        if (ctx_ret == ESP_OK && !is_blank_text(realtime)) {
            snprintf(realtime_section, sizeof(realtime_section),
                     "Realtime tool result:\n%s\n", realtime);
        } else {
            snprintf(realtime_section, sizeof(realtime_section),
                     "Realtime tool result: unavailable, error=%s.\n",
                     esp_err_to_name(ctx_ret));
        }
    }

    snprintf(out, out_size,
             "Device context:\n"
             "- Local date and time: %s\n"
             "- Weather: %s\n"
             "- Current heart rate: %s bpm\n"
             "- Current breath rate: %s bpm\n"
             "%s"
             "User said: %s\n"
             "Answer in Simplified Chinese. Use the device context for date, time, weather, heart rate, and breath rate. "
             "If the user asks news or latest events, summarize the realtime news items first; never answer only with the date or time. "
             "If realtime news is unavailable, say it briefly. "
             "Keep the answer natural, spoken, and concise.",
             local_time, weather_text, heart, breath, realtime_section,
             transcript != NULL ? transcript : "");
}

static void voice_pipeline_task(void *arg)
{
    (void)arg;

    app_audio_recording_t rec = {0};
    char transcript[384] = {0};
    char prompt[2300] = {0};
    char reply[768] = {0};

    set_stage_from_task(TXT_LISTENING, TXT_RELEASE_ASR, C_BLUE);
    esp_err_t ret = app_audio_input_record_until_stopped(CONFIG_APP_AUDIO_RECORD_MAX_MS,
                                                         &s_record_stop_requested,
                                                         &rec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(ret));
        set_stage_from_task(TXT_MIC_ERROR, TXT_HOLD_TALK, C_RED);
        goto out;
    }

    ESP_LOGI(TAG, "record stats: duration=%ums peak=%ld rms=%u",
             (unsigned)rec.duration_ms, (long)rec.peak, (unsigned)rec.rms);

    set_stage_from_task(TXT_ASR, TXT_WAIT, C_AMBER);
    ret = app_cloud_asr_transcribe_pcm(rec.pcm, rec.samples, rec.sample_rate,
                                       transcript, sizeof(transcript));
    if (ret != ESP_OK || transcript[0] == '\0') {
        ESP_LOGW(TAG, "ASR unavailable: %s", esp_err_to_name(ret));
        snprintf(transcript, sizeof(transcript),
                 TXT_ASR_FALLBACK,
                 (unsigned)rec.duration_ms);
    }
    ESP_LOGI(TAG, "ASR text: %s", transcript);

    if (!app_network_is_ready()) {
        set_stage_from_task(TXT_NET_NOT_READY, TXT_HOLD_TALK, C_AMBER);
        goto out;
    }

    set_stage_from_task(TXT_THINKING, TXT_WAIT, C_AMBER);
    build_voice_chat_prompt(transcript, prompt, sizeof(prompt));
    ESP_LOGI(TAG, "LLM prompt: %s", prompt);
    ret = app_cloud_llm_chat(prompt, reply, sizeof(reply));
    ESP_LOGI(TAG, "LLM reply: %s", reply);
    if (ret != ESP_OK || is_blank_text(reply)) {
        ESP_LOGW(TAG, "LLM reply unavailable/blank: %s", esp_err_to_name(ret));
        strlcpy(reply, TXT_REPEAT, sizeof(reply));
    }

    set_stage_from_task(TXT_SPEAKING, TXT_WAIT, C_GREEN);
    ret = app_cloud_tts_speak(reply);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cloud TTS unavailable: %s; using local feedback tone",
                 esp_err_to_name(ret));
        ret = app_voice_output_speak_text(reply);
    }

    if (ret == ESP_OK) {
        set_stage_from_task(TXT_READY, TXT_HOLD_TALK, C_GREEN);
    } else {
        set_stage_from_task(TXT_PLAY_ERROR, TXT_HOLD_TALK, C_RED);
    }

out:
    app_audio_recording_free(&rec);
    s_button_down = false;
    s_pipeline_running = false;
    vTaskDelete(NULL);
}

static void ai_advice_task(void *arg)
{
    (void)arg;

    char prompt[512] = {0};
    char reply[512] = {0};

    if (!app_network_is_ready()) {
        set_advice_stage_from_task(TXT_NET_NOT_READY, TXT_AI_ADVICE, C_AMBER);
        goto out;
    }

    build_ai_advice_prompt(prompt, sizeof(prompt));
    ESP_LOGI(TAG, "AI advice prompt: %s", prompt);

    set_advice_stage_from_task(TXT_ANALYZING, TXT_WAIT, C_AMBER);
    esp_err_t ret = app_cloud_llm_chat(prompt, reply, sizeof(reply));
    ESP_LOGI(TAG, "AI advice reply: %s", reply);
    if (ret != ESP_OK || is_blank_text(reply)) {
        ESP_LOGW(TAG, "AI advice unavailable/blank: %s", esp_err_to_name(ret));
        strlcpy(reply, TXT_ADVICE_FALLBACK, sizeof(reply));
    }
    truncate_utf8_inplace(reply, AI_ADVICE_TTS_MAX_BYTES);
    ESP_LOGI(TAG, "AI advice reply bytes=%u text=%s", (unsigned)strlen(reply), reply);

    set_advice_stage_from_task(TXT_SPEAKING, TXT_WAIT, C_GREEN);
    ret = app_cloud_tts_speak(reply);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cloud TTS unavailable: %s; using local feedback tone",
                 esp_err_to_name(ret));
        ret = app_voice_output_speak_text(reply);
    }

    if (ret == ESP_OK) {
        set_advice_stage_from_task(TXT_READY, TXT_AI_ADVICE, C_GREEN);
    } else {
        set_advice_stage_from_task(TXT_PLAY_ERROR, TXT_AI_ADVICE, C_RED);
    }

out:
    s_pipeline_running = false;
    vTaskDelete(NULL);
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static void emergency_call_task(void *arg)
{
    (void)arg;

    if (CONFIG_APP_SIM800_EMERGENCY_PHONE[0] == '\0') {
        set_call_stage_from_task(TXT_PHONE_EMPTY, TXT_PHONE_HELP, C_AMBER);
        s_emergency_running = false;
        vTaskDelete(NULL);
        return;
    }

    set_call_stage_from_task(TXT_CALLING, TXT_PHONE_HELP, C_RED);
    esp_err_t ret = app_sim800_emergency_call();
    ESP_LOGI(TAG, "emergency call result: %s status=%s",
             esp_err_to_name(ret), app_sim800_status_text());
    set_call_stage_from_task(ret == ESP_OK ? TXT_CALLED : app_sim800_status_text(),
                             TXT_PHONE_HELP,
                             ret == ESP_OK ? C_GREEN : C_RED);

    s_emergency_running = false;
    vTaskDelete(NULL);
}

static void emergency_sms_task(void *arg)
{
    (void)arg;

    char sms_text[384] = {0};

    if (CONFIG_APP_SIM800_EMERGENCY_PHONE[0] == '\0') {
        set_sms_stage_from_task(TXT_PHONE_EMPTY, TXT_SMS_HELP, C_AMBER);
        s_emergency_running = false;
        vTaskDelete(NULL);
        return;
    }

    build_emergency_sms_text(sms_text, sizeof(sms_text), TXT_MANUAL_HELP);
    set_sms_stage_from_task(TXT_SENDING, TXT_SMS_HELP, C_BLUE);
    esp_err_t ret = app_sim800_emergency_sms(sms_text);
    ESP_LOGI(TAG, "emergency SMS result: %s status=%s text=%s",
             esp_err_to_name(ret), app_sim800_status_text(), sms_text);
    set_sms_stage_from_task(ret == ESP_OK ? TXT_SENT : app_sim800_status_text(),
                            TXT_SMS_HELP,
                            ret == ESP_OK ? C_GREEN : C_RED);

    s_emergency_running = false;
    vTaskDelete(NULL);
}
#endif

static void start_voice_pipeline(void)
{
    if (s_pipeline_running) {
        set_talk_state(TXT_BUSY, TXT_WAIT, C_AMBER);
        return;
    }

    s_record_stop_requested = false;
    s_pipeline_running = true;
    s_button_down = true;

    BaseType_t ok = xTaskCreate(voice_pipeline_task, "voice_pipeline", 16384, NULL, 3, NULL);
    if (ok != pdPASS) {
        s_pipeline_running = false;
        s_button_down = false;
        set_talk_state(TXT_TASK_ERROR, TXT_HOLD_TALK, C_RED);
    }
}

static void start_ai_advice_pipeline(void)
{
    if (s_pipeline_running) {
        set_advice_state(TXT_BUSY, TXT_WAIT, C_AMBER);
        return;
    }

    s_pipeline_running = true;
    s_button_down = false;

    BaseType_t ok = xTaskCreate(ai_advice_task, "ai_advice", 12288, NULL, 3, NULL);
    if (ok != pdPASS) {
        s_pipeline_running = false;
        set_advice_state(TXT_TASK_ERROR, TXT_AI_ADVICE, C_RED);
    }
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static void start_emergency_call(void)
{
    if (s_emergency_running) {
        set_call_state(TXT_BUSY, TXT_PHONE_HELP, C_AMBER);
        return;
    }

    s_emergency_running = true;
    BaseType_t ok = xTaskCreate(emergency_call_task, "sim800_call", 8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_emergency_running = false;
        set_call_state(TXT_TASK_ERROR, TXT_PHONE_HELP, C_RED);
    }
}

static void start_emergency_sms(void)
{
    if (s_emergency_running) {
        set_sms_state(TXT_BUSY, TXT_SMS_HELP, C_AMBER);
        return;
    }

    s_emergency_running = true;
    BaseType_t ok = xTaskCreate(emergency_sms_task, "sim800_sms", 8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_emergency_running = false;
        set_sms_state(TXT_TASK_ERROR, TXT_SMS_HELP, C_RED);
    }
}
#endif

static void talk_button_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        start_voice_pipeline();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_button_down) {
            return;
        }

        s_record_stop_requested = true;
        set_talk_state(TXT_PROCESSING, TXT_WAIT, C_AMBER);
    }
}

static void advice_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        start_ai_advice_pipeline();
    }
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static void call_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        start_emergency_call();
    }
}

static void sms_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        start_emergency_sms();
    }
}
#endif

static lv_obj_t *create_vital_card(lv_obj_t *parent, const char *title, const char *unit,
                                   uint32_t accent, int32_t x)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 340, 142);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, 190);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, hex(0xAAB6C8), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title_label = label_create(card, title, C_MUTED, FONT_CN);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 22, 18);

    lv_obj_t *value_label = label_create(card, "--", C_TEXT, FONT_VALUE);
    lv_obj_set_width(value_label, 160);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(value_label, LV_ALIGN_LEFT_MID, 34, 14);

    lv_obj_t *unit_label = label_create(card, unit, accent, FONT_CN);
    lv_obj_align_to(unit_label, value_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -8);

    lv_obj_t *accent_bar = lv_obj_create(card);
    lv_obj_set_size(accent_bar, 5, 88);
    lv_obj_align(accent_bar, LV_ALIGN_RIGHT_MID, -22, 12);
    lv_obj_set_style_radius(accent_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent_bar, hex(accent), LV_PART_MAIN);
    lv_obj_set_style_border_width(accent_bar, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(accent_bar, LV_SCROLLBAR_MODE_OFF);

    return value_label;
}

static void create_time_weather_header(lv_obj_t *parent)
{
    s_time_label = label_create(parent, "--:--", C_TEXT, FONT_VALUE);
    lv_obj_set_width(s_time_label, 260);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_LEFT, 64, 54);

    s_date_label = label_create(parent, "---- -- --", C_TEXT, FONT_DATE);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_LEFT, 68, 120);

    s_weekday_label = label_create(parent, TXT_TIME_WAIT, C_MUTED, FONT_CN_SMALL);
    lv_obj_set_width(s_weekday_label, 220);
    lv_label_set_long_mode(s_weekday_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_weekday_label, LV_ALIGN_TOP_LEFT, 282, 127);

    s_weather_temp_label = label_create(parent, "--", C_TEXT, FONT_VALUE);
    lv_obj_set_width(s_weather_temp_label, 170);
    lv_obj_set_style_text_align(s_weather_temp_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(s_weather_temp_label, LV_ALIGN_TOP_RIGHT, -116, 54);

    s_weather_unit_label = label_create(parent, "\xE2\x84\x83", C_TEXT, FONT_CN);
    lv_obj_align_to(s_weather_unit_label, s_weather_temp_label, LV_ALIGN_OUT_RIGHT_TOP, 8, 8);

    s_weather_desc_label = label_create(parent, TXT_WEATHER_WAIT, C_BLUE, FONT_CN_SMALL);
    lv_obj_set_width(s_weather_desc_label, 330);
    lv_label_set_long_mode(s_weather_desc_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_weather_desc_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(s_weather_desc_label, LV_ALIGN_TOP_RIGHT, -72, 124);

    s_weather_meta_label = label_create(parent, "weather idle", C_AMBER, FONT_CN_SMALL);
    lv_obj_set_width(s_weather_meta_label, 330);
    lv_label_set_long_mode(s_weather_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_weather_meta_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(s_weather_meta_label, LV_ALIGN_TOP_RIGHT, -72, 154);
}

static void create_history_strip(lv_obj_t *parent)
{
    lv_obj_t *title_label = label_create(parent, TXT_HISTORY_TITLE, C_TEXT, FONT_CN_SMALL);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 64, 344);

    s_history_status_label = label_create(parent, "SD --", C_AMBER, FONT_CN_SMALL);
    lv_obj_align_to(s_history_status_label, title_label, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    const int32_t card_w = 168;
    const int32_t card_h = 48;
    const int32_t gap = 18;
    const int32_t start_x = 64;
    const int32_t y = 372;

    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, start_x + (int32_t)i * (card_w + gap), y);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(card, hex(C_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, hex(C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        s_history_day_labels[i] = label_create(card, "--", C_MUTED, FONT_CN_SMALL);
        lv_obj_set_width(s_history_day_labels[i], card_w - 16);
        lv_label_set_long_mode(s_history_day_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_history_day_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_history_day_labels[i], LV_ALIGN_CENTER, 0, 0);
    }
}

#if APP_EMERGENCY_BUTTONS_ENABLE
static lv_obj_t *create_emergency_button(lv_obj_t *parent, const char *title, const char *status,
                                         uint32_t accent, int32_t x,
                                         lv_event_cb_t event_cb,
                                         lv_obj_t **title_label,
                                         lv_obj_t **status_label)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 162, 82);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, 492);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, hex(accent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, hex(0x8FA4C7), LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_ALL, NULL);

    *title_label = label_create(button, title, C_TEXT, FONT_CN);
    lv_obj_align(*title_label, LV_ALIGN_CENTER, 0, -14);

    *status_label = label_create(button, status, accent, FONT_CN_SMALL);
    lv_obj_set_width(*status_label, 146);
    lv_label_set_long_mode(*status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(*status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(*status_label, LV_ALIGN_CENTER, 0, 18);

    return button;
}
#endif

static void create_voice_agent_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen, hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_network_label = label_create(screen, TXT_NET_WAITING, C_MUTED, FONT_CN);
    lv_obj_set_width(s_network_label, 300);
    lv_label_set_long_mode(s_network_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_network_label, LV_ALIGN_TOP_LEFT, 32, 18);

    create_time_weather_header(screen);

    s_heart_value_label = create_vital_card(screen, TXT_HEART_TITLE, TXT_UNIT_BPM, C_HEART, 132);
    s_breath_value_label = create_vital_card(screen, TXT_BREATH_TITLE, TXT_UNIT_BPM, C_BREATH, 552);

    create_history_strip(screen);

    s_hold_button = lv_button_create(screen);
    lv_obj_set_size(s_hold_button, 148, 148);
    lv_obj_align(s_hold_button, LV_ALIGN_BOTTOM_MID, -112, -14);
    lv_obj_set_style_radius(s_hold_button, 74, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hold_button, hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hold_button, hex(C_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_hold_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hold_button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_hold_button, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_hold_button, hex(C_BLUE), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_hold_button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_hold_button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_hold_button, hex(0x8FA4C7), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_hold_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_hold_button, talk_button_event_cb, LV_EVENT_ALL, NULL);

    s_hold_label = label_create(s_hold_button, TXT_HOLD_TALK, C_TEXT, FONT_CN);
    lv_obj_align(s_hold_label, LV_ALIGN_CENTER, 0, -12);

    s_voice_status_label = label_create(s_hold_button, TXT_READY, C_GREEN, FONT_CN);
    lv_obj_align(s_voice_status_label, LV_ALIGN_CENTER, 0, 28);

    s_advice_button = lv_button_create(screen);
    lv_obj_set_size(s_advice_button, 148, 148);
    lv_obj_align(s_advice_button, LV_ALIGN_BOTTOM_MID, 112, -14);
    lv_obj_set_style_radius(s_advice_button, 74, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_advice_button, hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_advice_button, hex(C_GREEN), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_advice_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_advice_button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_advice_button, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_advice_button, hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_advice_button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_advice_button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_advice_button, hex(0x8FA4C7), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_advice_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_advice_button, advice_button_event_cb, LV_EVENT_ALL, NULL);

    s_advice_label = label_create(s_advice_button, TXT_AI_ADVICE, C_TEXT, FONT_CN);
    lv_obj_align(s_advice_label, LV_ALIGN_CENTER, 0, -12);

    s_advice_status_label = label_create(s_advice_button, TXT_ONE_TAP, C_GREEN, FONT_CN);
    lv_obj_align(s_advice_status_label, LV_ALIGN_CENTER, 0, 28);

#if APP_EMERGENCY_BUTTONS_ENABLE
    s_call_button = create_emergency_button(screen, TXT_PHONE_HELP, TXT_ONE_TAP_CALL, C_RED, 64,
                                            call_button_event_cb,
                                            &s_call_label,
                                            &s_call_status_label);

    s_sms_button = create_emergency_button(screen, TXT_SMS_HELP, TXT_ONE_TAP_SEND, C_BLUE, 798,
                                           sms_button_event_cb,
                                           &s_sms_label,
                                           &s_sms_status_label);
#endif

    update_runtime_labels();
    lv_timer_create(runtime_timer_cb, 1000, NULL);
}

#if CONFIG_APP_SIM800_ENABLE && (CONFIG_APP_SIM800_AUTO_SMS_ENABLE || CONFIG_APP_SIM800_AUTO_CALL_ENABLE)
static void sim800_auto_watch_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t fusion_alerts = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (fusion_alerts == 0) {
            continue;
        }

        int waited_ms = 0;
        ESP_LOGI(TAG, "Waiting for fall alarm audio before cellular alert");
        while (s_fall_alert_sound_running && waited_ms < FALL_ALERT_SOUND_WAIT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
        }
        ESP_LOGI(TAG, "Alarm audio wait done running=%d waited=%d ms; settling %d ms",
                 s_fall_alert_sound_running, waited_ms, FALL_CELLULAR_SETTLE_MS);
        vTaskDelay(pdMS_TO_TICKS(FALL_CELLULAR_SETTLE_MS));

        char sms_text[384] = {0};
        build_emergency_sms_text(sms_text, sizeof(sms_text), TXT_FALL_ALERT);
        esp_err_t ret = app_sim800_auto_alert(sms_text);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "SIM800 auto alert sent: %s", sms_text);
        } else if (ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "SIM800 auto alert failed: %s status=%s",
                     esp_err_to_name(ret), app_sim800_status_text());
        }
    }
}
#endif

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone configured for China Standard Time (UTC+8)");
    ESP_LOGI(TAG, "Starting voice LLM test UI, reset_reason=%d", (int)esp_reset_reason());

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }

    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_ERROR_CHECK(bsp_display_brightness_set(100));

    bsp_display_lock(0);
    create_voice_agent_screen();
    bsp_display_unlock();

    esp_err_t voice_ret = app_voice_output_init();
    if (voice_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_voice_output_init failed: %s", esp_err_to_name(voice_ret));
    } else {
        ESP_LOGI(TAG, "audio ready: dma_free=%u largest_dma=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }

    esp_err_t mic_ret = app_audio_input_init();
    if (mic_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_audio_input_init failed: %s", esp_err_to_name(mic_ret));
    } else {
        ESP_LOGI(TAG, "microphone ready: sample_rate=%d Hz", CONFIG_APP_AUDIO_SAMPLE_RATE);
    }

    esp_err_t radar_ret = app_radar_start();
    if (radar_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_radar_start failed: %s", esp_err_to_name(radar_ret));
    }

    esp_err_t k230_ret = app_k230_vision_start();
    if (k230_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_k230_vision_start failed: %s", esp_err_to_name(k230_ret));
    }

#if CONFIG_APP_SIM800_ENABLE
    esp_err_t sim800_ret = app_sim800_start();
    if (sim800_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_sim800_start failed: %s", esp_err_to_name(sim800_ret));
    }
#endif

    esp_err_t history_ret = app_vitals_history_start();
    if (history_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_vitals_history_start failed: %s", esp_err_to_name(history_ret));
    }

    esp_err_t weather_ret = app_weather_start();
    if (weather_ret != ESP_OK) {
        ESP_LOGW(TAG, "app_weather_start failed: %s", esp_err_to_name(weather_ret));
    }

    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(network_start_task,
                                                         "net_start",
                                                         8192,
                                                         NULL,
                                                         4,
                                                         NULL,
                                                         0,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create network task");
    }

#if CONFIG_APP_SIM800_ENABLE && (CONFIG_APP_SIM800_AUTO_SMS_ENABLE || CONFIG_APP_SIM800_AUTO_CALL_ENABLE)
    BaseType_t auto_ok = xTaskCreate(sim800_auto_watch_task,
                                     "sim800_auto",
                                     12288,
                                     NULL,
                                     3,
                                     &s_auto_alert_task_handle);
    if (auto_ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create SIM800 auto alert task");
    }
#endif

    ESP_LOGI(TAG, "Voice LLM test UI is ready");
}
