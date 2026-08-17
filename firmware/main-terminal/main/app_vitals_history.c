#include "app_vitals_history.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_radar.h"

#ifndef CONFIG_APP_HISTORY_SAMPLE_SECONDS
#define CONFIG_APP_HISTORY_SAMPLE_SECONDS 60
#endif

#define HISTORY_FILE_PATH BSP_SD_MOUNT_POINT "/vitals_history.csv"
#define HISTORY_MIN_HEART_BPM 30.0f
#define HISTORY_MAX_HEART_BPM 220.0f
#define HISTORY_MIN_BREATH_BPM 4.0f
#define HISTORY_MAX_BREATH_BPM 60.0f
#define HISTORY_DATA_FRESH_MS 120000

static const char *TAG = "vitals_history";

typedef struct {
    bool used;
    char key[16];
    uint64_t heart_sum;
    uint32_t heart_count;
    uint64_t breath_sum;
    uint32_t breath_count;
} history_record_t;

static SemaphoreHandle_t s_mutex;
static history_record_t s_records[APP_VITALS_HISTORY_DAYS];
static bool s_started;
static bool s_sd_ready;
static bool s_file_loaded;
static char s_status[96] = "history idle";

static void set_status(const char *text)
{
    strlcpy(s_status, text, sizeof(s_status));
    ESP_LOGI(TAG, "%s", s_status);
}

static bool is_valid_heart(float value)
{
    return value >= HISTORY_MIN_HEART_BPM && value <= HISTORY_MAX_HEART_BPM;
}

static bool is_valid_breath(float value)
{
    return value >= HISTORY_MIN_BREATH_BPM && value <= HISTORY_MAX_BREATH_BPM;
}

static uint16_t round_bpm(float value)
{
    return (uint16_t)(value + 0.5f);
}

static bool key_is_day_index(const char *key)
{
    return key != NULL && key[0] == 'D' && key[1] == 'A' && key[2] == 'Y';
}

static int compare_day_key(const char *a, const char *b)
{
    if (key_is_day_index(a) && key_is_day_index(b)) {
        int ai = atoi(a + 3);
        int bi = atoi(b + 3);
        return (ai > bi) - (ai < bi);
    }
    return strcmp(a, b);
}

static int find_record_locked(const char *key)
{
    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        if (s_records[i].used && strcmp(s_records[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_free_record_locked(void)
{
    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        if (!s_records[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static int find_oldest_record_locked(void)
{
    int oldest = -1;
    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        if (!s_records[i].used) {
            continue;
        }
        if (oldest < 0 || compare_day_key(s_records[i].key, s_records[oldest].key) < 0) {
            oldest = (int)i;
        }
    }
    return oldest;
}

static history_record_t *get_or_create_record_locked(const char *key)
{
    int index = find_record_locked(key);
    if (index >= 0) {
        return &s_records[index];
    }

    index = find_free_record_locked();
    if (index < 0) {
        index = find_oldest_record_locked();
        if (index < 0) {
            return NULL;
        }
        if (compare_day_key(key, s_records[index].key) <= 0) {
            return NULL;
        }
    }

    memset(&s_records[index], 0, sizeof(s_records[index]));
    s_records[index].used = true;
    strlcpy(s_records[index].key, key, sizeof(s_records[index].key));
    return &s_records[index];
}

static void merge_record_locked(const history_record_t *src)
{
    if (!src->used || src->key[0] == '\0') {
        return;
    }

    history_record_t *dst = get_or_create_record_locked(src->key);
    if (dst == NULL) {
        return;
    }

    dst->heart_sum += src->heart_sum;
    dst->heart_count += src->heart_count;
    dst->breath_sum += src->breath_sum;
    dst->breath_count += src->breath_count;
}

static bool current_day_key(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);

    int year = tm_now.tm_year + 1900;
    if (year >= 2024) {
        snprintf(out, out_size, "%04d-%02d-%02d",
                 year, tm_now.tm_mon + 1, tm_now.tm_mday);
        return true;
    }

    int64_t uptime_s = esp_timer_get_time() / 1000000;
    snprintf(out, out_size, "DAY%" PRId64, uptime_s / 86400);
    return false;
}

static esp_err_t ensure_sd_ready(void)
{
    if (s_sd_ready) {
        return ESP_OK;
    }

    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_sd_ready = true;
        set_status("SD card mounted");
        return ESP_OK;
    }

    snprintf(s_status, sizeof(s_status), "SD mount failed: %s", esp_err_to_name(ret));
    ESP_LOGW(TAG, "%s", s_status);
    return ret;
}

static esp_err_t load_records_from_sd(void)
{
    FILE *fp = fopen(HISTORY_FILE_PATH, "r");
    if (fp == NULL) {
        set_status("history file not found");
        return ESP_ERR_NOT_FOUND;
    }

    char line[160];
    uint32_t loaded = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        history_record_t rec = {0};
        char key[sizeof(rec.key)] = {0};
        uint64_t heart_sum = 0;
        uint32_t heart_count = 0;
        uint64_t breath_sum = 0;
        uint32_t breath_count = 0;
        int matched = sscanf(line, "%15[^,],%" SCNu64 ",%" SCNu32 ",%" SCNu64 ",%" SCNu32,
                             key, &heart_sum, &heart_count, &breath_sum, &breath_count);
        if (matched != 5) {
            continue;
        }

        rec.used = true;
        strlcpy(rec.key, key, sizeof(rec.key));
        rec.heart_sum = heart_sum;
        rec.heart_count = heart_count;
        rec.breath_sum = breath_sum;
        rec.breath_count = breath_count;

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            merge_record_locked(&rec);
            xSemaphoreGive(s_mutex);
            loaded++;
        }
    }
    fclose(fp);

    snprintf(s_status, sizeof(s_status), "history loaded: %" PRIu32, loaded);
    ESP_LOGI(TAG, "%s", s_status);
    return ESP_OK;
}

static esp_err_t save_records_to_sd(void)
{
    history_record_t snapshot[APP_VITALS_HISTORY_DAYS] = {0};

    if (!s_sd_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(snapshot, s_records, sizeof(snapshot));
    xSemaphoreGive(s_mutex);

    FILE *fp = fopen(HISTORY_FILE_PATH, "w");
    if (fp == NULL) {
        set_status("history file open failed");
        return ESP_FAIL;
    }

    fprintf(fp, "#key,heart_sum,heart_count,breath_sum,breath_count\n");
    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        const history_record_t *rec = &snapshot[i];
        if (!rec->used) {
            continue;
        }
        fprintf(fp, "%s,%" PRIu64 ",%" PRIu32 ",%" PRIu64 ",%" PRIu32 "\n",
                rec->key, rec->heart_sum, rec->heart_count,
                rec->breath_sum, rec->breath_count);
    }
    fclose(fp);
    set_status("history saved");
    return ESP_OK;
}

static bool add_sample_to_history(const char *key, bool has_heart, uint16_t heart,
                                  bool has_breath, uint16_t breath)
{
    bool changed = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    history_record_t *rec = get_or_create_record_locked(key);
    if (rec != NULL) {
        if (has_heart) {
            rec->heart_sum += heart;
            rec->heart_count++;
            changed = true;
        }
        if (has_breath) {
            rec->breath_sum += breath;
            rec->breath_count++;
            changed = true;
        }
    }

    xSemaphoreGive(s_mutex);
    return changed;
}

static void maybe_load_history_file(void)
{
    if (s_file_loaded || ensure_sd_ready() != ESP_OK) {
        return;
    }

    esp_err_t ret = load_records_from_sd();
    if (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND) {
        s_file_loaded = true;
    }
}

static void history_task(void *arg)
{
    (void)arg;

    setenv("TZ", "CST-8", 1);
    tzset();

    vTaskDelay(pdMS_TO_TICKS(5000));
    maybe_load_history_file();

    while (true) {
        char day_key[16] = {0};
        app_radar_sample_t sample = {0};
        app_radar_get_latest(&sample);

        int64_t now_ms = esp_timer_get_time() / 1000;
        bool has_heart = sample.has_heart_bpm &&
                         sample.last_heart_ms > 0 &&
                         now_ms - sample.last_heart_ms <= HISTORY_DATA_FRESH_MS &&
                         is_valid_heart(sample.heart_bpm);
        bool has_breath = sample.has_breath_bpm &&
                          sample.last_breath_ms > 0 &&
                          now_ms - sample.last_breath_ms <= HISTORY_DATA_FRESH_MS &&
                          is_valid_breath(sample.breath_bpm);

        if (has_heart || has_breath) {
            current_day_key(day_key, sizeof(day_key));
            bool changed = add_sample_to_history(day_key,
                                                 has_heart, round_bpm(sample.heart_bpm),
                                                 has_breath, round_bpm(sample.breath_bpm));
            if (changed) {
                maybe_load_history_file();
                if (s_sd_ready) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(save_records_to_sd());
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_HISTORY_SAMPLE_SECONDS * 1000));
    }
}

esp_err_t app_vitals_history_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(history_task, "vitals_history", 6144, NULL, 3, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    set_status("history started");
    return ESP_OK;
}

size_t app_vitals_history_get_recent(app_vitals_history_day_t *out, size_t max_count)
{
    history_record_t sorted[APP_VITALS_HISTORY_DAYS] = {0};
    size_t count = 0;

    if (out == NULL || max_count == 0 || s_mutex == NULL) {
        return 0;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    for (size_t i = 0; i < APP_VITALS_HISTORY_DAYS; ++i) {
        if (!s_records[i].used) {
            continue;
        }
        sorted[count++] = s_records[i];
    }
    xSemaphoreGive(s_mutex);

    for (size_t i = 1; i < count; ++i) {
        history_record_t item = sorted[i];
        size_t j = i;
        while (j > 0 && compare_day_key(sorted[j - 1].key, item.key) > 0) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = item;
    }

    if (count > max_count) {
        count = max_count;
    }

    memset(out, 0, max_count * sizeof(out[0]));
    for (size_t i = 0; i < count; ++i) {
        const history_record_t *rec = &sorted[i];
        strlcpy(out[i].day, rec->key, sizeof(out[i].day));

        out[i].heart_samples = rec->heart_count;
        if (rec->heart_count > 0) {
            out[i].has_heart_bpm = true;
            out[i].heart_bpm = (uint16_t)((rec->heart_sum + rec->heart_count / 2) / rec->heart_count);
        }

        out[i].breath_samples = rec->breath_count;
        if (rec->breath_count > 0) {
            out[i].has_breath_bpm = true;
            out[i].breath_bpm = (uint16_t)((rec->breath_sum + rec->breath_count / 2) / rec->breath_count);
        }
    }

    return count;
}

bool app_vitals_history_sd_ready(void)
{
    return s_sd_ready;
}

const char *app_vitals_history_status_text(void)
{
    return s_status;
}
