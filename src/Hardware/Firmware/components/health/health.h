/**
 * @file health.h
 * @brief 周期健康上报 — protocol v1 §3.5
 *
 * 默认 30 s 一次;battery_soc 变 ≥3% 或 rssi 变 ≥10dB 时立即触发(去抖 5s)。
 */
#ifndef HEALTH_H
#define HEALTH_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct health_dev health_dev_t;

typedef struct {
    mqtt_dev_t *mqtt;
    const char *device_id;
    const char *batch_uuid;
    uint32_t period_s;                 // 0 → 30
    int  (*get_battery_soc)(void);     // -1 if unknown
    bool (*get_charging)(void);
    int  (*get_rssi)(void);            // dBm, 0 if unknown
    const char *(*get_ip)(void);       // "" if none
    uint32_t (*get_uptime_s)(void);
    uint32_t (*get_spiffs_pending)(void);
    uint32_t (*get_ack_pending)(void);
    uint32_t (*get_drop_count)(void);
} health_config_t;

esp_err_t health_init(health_dev_t **out, const health_config_t *cfg);

/** 立即触发一次发送(忽略去抖窗口外)。 */
esp_err_t health_kick(health_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif // HEALTH_H
