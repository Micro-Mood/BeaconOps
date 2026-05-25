/**
 * @file event.h
 * @brief 端事件上行 — protocol v1 §3.6
 *
 * 用于上报 ack_give_up / parse_reject / store_full / dedup_drop 等异常。
 * 离线时进 RAM 短队列(8 条),上线后 flush;超出丢弃。
 */
#ifndef EVENT_H
#define EVENT_H

#include "esp_err.h"
#include <stdint.h>
#include "mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVENT_ACK_GIVE_UP = 0,
    EVENT_PARSE_REJECT,
    EVENT_STORE_FULL,
    EVENT_DEDUP_DROP,
} event_kind_e;

typedef struct event_dev event_dev_t;

/**
 * 创建 event 设备,绑定 mqtt 句柄与 device_id。
 * mqtt 可为 NULL,后续通过 event_bind_mqtt 注入。
 */
esp_err_t event_init(event_dev_t **out, mqtt_dev_t *mqtt, const char *device_id);

/** 在 MQTT 就绪后再绑定(mqtt 可在 event 之后初始化)。 */
void event_bind_mqtt(event_dev_t *dev, mqtt_dev_t *mqtt);

/**
 * 上报一条事件。msg_id 可为 NULL 或 "";detail 同理。
 * 非阻塞,立即返回。
 */
esp_err_t event_emit(event_dev_t *dev, event_kind_e kind,
                     const char *msg_id, const char *detail);

#ifdef __cplusplus
}
#endif

#endif // EVENT_H
