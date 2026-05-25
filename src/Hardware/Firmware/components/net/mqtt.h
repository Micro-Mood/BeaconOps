/**
 * @file mqtt.h
 * @brief MQTT 客户端 — BeaconOps protocol v1
 *
 * 通道(由 device_id = 12-hex MAC 替代旧 device_uuid):
 *   sub: device/{id}/cmd, broadcast/all/cmd, broadcast/dept/+/cmd
 *   pub: device/{id}/status         (online + LWT,QoS1 retain)
 *        device/{id}/uplink/ack
 *        device/{id}/uplink/event
 *        device/{id}/uplink/health
 *        device/{id}/uplink/profile
 *
 * 鉴权:username = batch_uuid,password = identity_build_password() 派生
 *      (ts:nonce:hmac_hex)。
 */
#ifndef MQTT_H
#define MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    MQTT_EVT_CONNECTED = 0,
    MQTT_EVT_DISCONNECTED,
    MQTT_EVT_ERROR,
} mqtt_event_e;

typedef void (*mqtt_status_fn)(mqtt_event_e evt, void *user);

/**
 * 入站消息回调。topic 字符串生命周期 = 本次回调内。
 */
typedef void (*mqtt_msg_fn)(const char *topic,
                            const char *payload, int len, void *user);

typedef esp_err_t (*mqtt_password_refresh_fn)(char *out, size_t len, void *user);

typedef struct {
    /* broker */
    const char    *broker_uri;          ///< NULL → BROKER_URI
    const char    *ca_pem;              ///< 仅 mqtts:// 用,NULL → certs default
    const char    *username;            ///< 必填 — batch_uuid
    const char    *password;            ///< 可选 — 静态密码(不刷新时使用)
    mqtt_password_refresh_fn refresh_password; ///< 可选 — 每次 CONNECT 前刷新 password
    void          *refresh_user;
    uint16_t       keepalive_s;
    uint32_t       reconnect_ms;

    /* 身份 */
    const char    *device_id;           ///< 必填 — 12 hex MAC
    const char    *dept;                ///< 可为 NULL/"" — 决定是否订阅 broadcast/dept/*/cmd
    const char    *online_payload;      ///< NULL → 运行时 {"online":true,"fw":FW_VERSION,"ts":<unix>}

    /* 回调 */
    mqtt_status_fn on_status;
    void          *status_user;
    mqtt_msg_fn    on_msg;
    void          *msg_user;
} mqtt_config_t;

typedef struct mqtt_dev_s mqtt_dev_t;

esp_err_t mqtt_init  (mqtt_dev_t **dev, const mqtt_config_t *config);
esp_err_t mqtt_deinit(mqtt_dev_t **dev);

bool      mqtt_is_connected(mqtt_dev_t *dev);

/** 通用发布。topic / payload 由调用方拼好;NUL 终止。 */
esp_err_t mqtt_publish(mqtt_dev_t *dev, const char *topic,
                       const char *payload, int qos, bool retain);

/** 项目固定通道便捷封装 */
esp_err_t mqtt_publish_ack    (mqtt_dev_t *dev, const char *payload, int qos);
esp_err_t mqtt_publish_event  (mqtt_dev_t *dev, const char *payload);  // QoS1
esp_err_t mqtt_publish_health (mqtt_dev_t *dev, const char *payload);  // QoS0
esp_err_t mqtt_publish_profile(mqtt_dev_t *dev, const char *payload);  // QoS1

#ifdef __cplusplus
}
#endif

#endif /* MQTT_H */
