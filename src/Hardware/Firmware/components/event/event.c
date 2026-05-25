/**
 * @file event.c
 */
#include "event.h"
#include "config.h"
#include "mqtt.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <time.h>

static const char *TAG = "event";

#define EVENT_Q_LEN   8

typedef struct {
    int kind;
    char msg_id[40];
    char detail[64];
    int64_t ts;
} ev_item_t;

struct event_dev {
    mqtt_dev_t *mqtt;
    char device_id[16];
    char topic[80];
    QueueHandle_t q;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
};

static const char *kind_to_str(int k) {
    switch (k) {
        case EVENT_ACK_GIVE_UP:  return "ack_give_up";
        case EVENT_PARSE_REJECT: return "parse_reject";
        case EVENT_STORE_FULL:   return "store_full";
        case EVENT_DEDUP_DROP:   return "dedup_drop";
        default:                 return "unknown";
    }
}

static esp_err_t do_publish(event_dev_t *d, const ev_item_t *it) {
    if (!d->mqtt || !mqtt_is_connected(d->mqtt)) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "kind", kind_to_str(it->kind));
    if (it->msg_id[0]) cJSON_AddStringToObject(root, "msg_id", it->msg_id);
    if (it->detail[0]) cJSON_AddStringToObject(root, "reason", it->detail);
    cJSON_AddNumberToObject(root, "ts", (double)it->ts);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_ERR_NO_MEM;
    int rc = mqtt_publish_event(d->mqtt, s);
    cJSON_free(s);
    return (rc == ESP_OK) ? ESP_OK : ESP_FAIL;
}

static void event_task(void *arg) {
    event_dev_t *d = (event_dev_t *)arg;
    ev_item_t it;
    for (;;) {
        if (xQueueReceive(d->q, &it, portMAX_DELAY) != pdTRUE) continue;
        // 等连接,最多 10 s
        int wait = 0;
        while ((!d->mqtt || !mqtt_is_connected(d->mqtt)) && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait++;
        }
        if (do_publish(d, &it) != ESP_OK) {
            ESP_LOGW(TAG, "drop event kind=%s msg_id=%s", kind_to_str(it.kind), it.msg_id);
        }
    }
}

esp_err_t event_init(event_dev_t **out, mqtt_dev_t *mqtt, const char *device_id) {
    if (!out || !device_id) return ESP_ERR_INVALID_ARG;
    event_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->mqtt = mqtt;
    strncpy(d->device_id, device_id, sizeof(d->device_id) - 1);
    snprintf(d->topic, sizeof(d->topic), MQTT_TOPIC_EVENT_FMT, device_id);
    d->q = xQueueCreate(EVENT_Q_LEN, sizeof(ev_item_t));
    d->lock = xSemaphoreCreateMutex();
    if (!d->q || !d->lock) {
        if (d->q)    vQueueDelete(d->q);
        if (d->lock) vSemaphoreDelete(d->lock);
        free(d);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(event_task, "event", 4096, d, 4, &d->task) != pdPASS) {
        vQueueDelete(d->q);
        vSemaphoreDelete(d->lock);
        free(d);
        return ESP_FAIL;
    }
    *out = d;
    ESP_LOGI(TAG, "event ready topic=%s", d->topic);
    return ESP_OK;
}

void event_bind_mqtt(event_dev_t *dev, mqtt_dev_t *mqtt) {
    if (!dev) return;
    xSemaphoreTake(dev->lock, portMAX_DELAY);
    dev->mqtt = mqtt;
    xSemaphoreGive(dev->lock);
}

esp_err_t event_emit(event_dev_t *dev, event_kind_e kind,
                     const char *msg_id, const char *detail) {
    if (!dev) return ESP_ERR_INVALID_STATE;
    ev_item_t it = { .kind = (int)kind, .ts = (int64_t)time(NULL) };
    if (msg_id) strncpy(it.msg_id, msg_id, sizeof(it.msg_id) - 1);
    if (detail) strncpy(it.detail, detail, sizeof(it.detail) - 1);
    if (xQueueSend(dev->q, &it, 0) != pdTRUE) {
        ESP_LOGW("event", "queue full, drop kind=%s", kind_to_str(it.kind));
        return ESP_FAIL;
    }
    return ESP_OK;
}
