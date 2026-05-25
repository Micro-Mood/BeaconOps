/**
 * @file time_sync.c
 * @brief SNTP 时间同步组件实现
 */

#include "time_sync.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sntp";

/* ---- 模块单例状态 ------------------------------------------------------ */
static volatile bool         s_synced  = false;
static bool                  s_started = false;
static time_sync_on_sync_fn  s_on_sync = NULL;
static void                 *s_user    = NULL;

/* ---- lwip sntp 同步通知回调 ------------------------------------------- */
static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    ESP_LOGI(TAG, "时间同步成功");
    if (s_on_sync) s_on_sync(tv, s_user);
}

esp_err_t time_sync_start(const time_sync_config_t *config)
{
    if (s_started) return ESP_OK;

    /* 取配置(可为 NULL → 全用默认) */
    const char *primary  = SNTP_PRIMARY_SERVER;
    const char *fallback = SNTP_FALLBACK_SERVER;
    const char *tz       = TZ_STRING;

    if (config) {
        if (config->primary_server)  primary  = config->primary_server;
        if (config->fallback_server) fallback = config->fallback_server;
        if (config->tz_string)       tz       = config->tz_string;
        s_on_sync = config->on_sync;
        s_user    = config->user;
    } else {
        s_on_sync = NULL;
        s_user    = NULL;
    }

    if (!primary || !*primary) {
        ESP_LOGE(TAG, "primary_server 为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, primary);
    if (fallback && *fallback) {
        esp_sntp_setservername(1, fallback);
    }
    sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    setenv("TZ", tz, 1);
    tzset();

    s_started = true;
    ESP_LOGI(TAG, "SNTP 启动: primary=%s, fallback=%s, tz=%s",
             primary, fallback ? fallback : "(none)", tz);
    return ESP_OK;
}

esp_err_t time_sync_stop(void)
{
    if (!s_started) return ESP_OK;
    esp_sntp_stop();
    s_synced  = false;
    s_started = false;
    s_on_sync = NULL;
    s_user    = NULL;
    ESP_LOGI(TAG, "SNTP 已停止");
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    if (s_synced) return ESP_OK;
    if (timeout_ms == 0) return ESP_ERR_TIMEOUT;

    const TickType_t step = pdMS_TO_TICKS(100);
    TickType_t       waited = 0;
    const TickType_t total  = pdMS_TO_TICKS(timeout_ms);

    while (!s_synced && waited < total) {
        vTaskDelay(step);
        waited += step;
    }
    return s_synced ? ESP_OK : ESP_ERR_TIMEOUT;
}

void time_sync_get_hhmm(char *buf, size_t len)
{
    if (!buf || len < 6) return;
    time_t now = 0;
    time(&now);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(buf, len, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}
