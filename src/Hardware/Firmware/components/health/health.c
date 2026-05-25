/**
 * @file health.c
 */
#include "health.h"
#include "config.h"
#include "mqtt.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "health";

struct health_dev {
    health_config_t cfg;
    char topic[80];
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    int  last_battery;
    int  last_rssi;
    int64_t last_send_ts;
};

#define DEBOUNCE_S  5

static void build_and_publish(health_dev_t *d) {
    if (!mqtt_is_connected(d->cfg.mqtt)) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    if (d->cfg.batch_uuid && d->cfg.batch_uuid[0]) {
        cJSON_AddStringToObject(root, "batch", d->cfg.batch_uuid);
    }
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    if (d->cfg.get_battery_soc) cJSON_AddNumberToObject(root, "battery", d->cfg.get_battery_soc());
    if (d->cfg.get_charging)    cJSON_AddBoolToObject(root,   "charging", d->cfg.get_charging());
    if (d->cfg.get_rssi)        cJSON_AddNumberToObject(root, "rssi", d->cfg.get_rssi());
    if (d->cfg.get_ip)          cJSON_AddStringToObject(root, "ip", d->cfg.get_ip());
    if (d->cfg.get_uptime_s)    cJSON_AddNumberToObject(root, "uptime_s", d->cfg.get_uptime_s());
    if (d->cfg.get_spiffs_pending) cJSON_AddNumberToObject(root, "spiffs_pending", d->cfg.get_spiffs_pending());
    if (d->cfg.get_ack_pending)    cJSON_AddNumberToObject(root, "ack_pending",    d->cfg.get_ack_pending());
    if (d->cfg.get_drop_count)     cJSON_AddNumberToObject(root, "drop_count",     d->cfg.get_drop_count());

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return;
    mqtt_publish_health(d->cfg.mqtt, s);
    cJSON_free(s);
    d->last_send_ts = time(NULL);
    if (d->cfg.get_battery_soc) d->last_battery = d->cfg.get_battery_soc();
    if (d->cfg.get_rssi)        d->last_rssi    = d->cfg.get_rssi();
}

static void health_task(void *arg) {
    health_dev_t *d = (health_dev_t *)arg;
    uint32_t period = d->cfg.period_s ? d->cfg.period_s : 30;
    d->last_send_ts = 0;
    d->last_battery = -1;
    d->last_rssi = 0;
    for (;;) {
        int64_t now = time(NULL);
        bool fire = false;
        if (now - d->last_send_ts >= period) {
            fire = true;
        } else if (now - d->last_send_ts >= DEBOUNCE_S) {
            int b = d->cfg.get_battery_soc ? d->cfg.get_battery_soc() : -1;
            int r = d->cfg.get_rssi        ? d->cfg.get_rssi()        :  0;
            if (d->last_battery >= 0 && abs(b - d->last_battery) >= 3) fire = true;
            if (d->last_rssi    != 0 && abs(r - d->last_rssi)    >= 10) fire = true;
        }
        if (fire) build_and_publish(d);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t health_init(health_dev_t **out, const health_config_t *cfg) {
    if (!out || !cfg || !cfg->mqtt || !cfg->device_id) return ESP_ERR_INVALID_ARG;
    health_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *cfg;
    d->lock = xSemaphoreCreateMutex();
    snprintf(d->topic, sizeof(d->topic), MQTT_TOPIC_HEALTH_FMT, cfg->device_id);
    if (xTaskCreate(health_task, "health", 4096, d, 4, &d->task) != pdPASS) {
        vSemaphoreDelete(d->lock);
        free(d);
        return ESP_FAIL;
    }
    *out = d;
    ESP_LOGI(TAG, "health ready topic=%s period=%lus", d->topic, (unsigned long)(cfg->period_s ? cfg->period_s : 30));
    return ESP_OK;
}

esp_err_t health_kick(health_dev_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    build_and_publish(dev);
    return ESP_OK;
}
