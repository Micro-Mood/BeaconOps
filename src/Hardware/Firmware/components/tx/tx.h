/**
 * @file tx.h
 * @brief 出站 ack 服务 — 持久 pending 环 + 后台重发
 *
 * 设计说明:
 *  - 所有 ack 事件经此组件:msg.on_ack → tx_emit_ack;先持久化到 NVS,
 *    再尽力发布;后台 drain 任务在 MQTT 可用时分批重试。
 *  - NVS 用专属 namespace(默认 "tx_pending"),不经 settings — 该环
 *    写频高且与用户配置无关。
 *  - QoS 映射:ACKED → 2;DISPLAYED / EXPIRED → 1(spec §6 §10.1)。
 *  - tx_ack_kind_e 与 msg 组件的 ack kind 解耦,装配层做 1 行映射。
 *
 * 持久 blob 布局:
 *   { uint32 version; uint32 count; tx_pending_entry_t entries[count] }
 *   version=1 时 entry = packed { id[37], kind, _pad[3], ts(uint32) }
 */

#ifndef TX_H
#define TX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief tx 自有 ack 类型(与 msg 解耦)
 */
typedef enum {
    TX_ACK_RECEIVED  = 0,
    TX_ACK_DISPLAYED = 1,
    TX_ACK_ACKED     = 2,
    TX_ACK_EXPIRED   = 3,
} tx_ack_kind_e;

typedef void (*tx_ack_result_fn)(const char *msg_id, tx_ack_kind_e kind,
                                 bool ok, void *user);

/* forward declare,避免本头 include mqtt.h */
struct mqtt_dev_s;
typedef struct mqtt_dev_s mqtt_dev_t;

/**
 * @brief tx 配置
 */
typedef struct {
    mqtt_dev_t *mqtt;                  ///< 必填,用于 publish + 连接判断
    const char *nvs_namespace;         ///< NULL → "tx_pending"
    uint16_t    max_pending;           ///< 0 → 32
    uint16_t    max_publish_per_tick;  ///< 0 → 4
    uint32_t    drain_period_ms;       ///< 0 → 5000
    uint8_t     max_attempts;          ///< 0 → 10
    tx_ack_result_fn on_result;        ///< publish 成功 / 超限失败 回调(NULL 允许)
    void       *result_user;
    uint8_t     task_prio;             ///< 0 → tskIDLE_PRIORITY + 2
    uint16_t    task_stack;            ///< 0 → 3072
} tx_config_t;

typedef struct tx_dev_s tx_dev_t;

/**
 * @brief 初始化 — 加载持久 ring + 启动 drain 任务
 */
esp_err_t tx_init  (tx_dev_t **dev, const tx_config_t *config);

/**
 * @brief 反初始化 — 停止 drain,持久化 ring,释放资源
 */
esp_err_t tx_deinit(tx_dev_t **dev);

/**
 * @brief 入队一条 ack(总是先持久化,再尝试立即发送)
 *
 * @param dev    句柄
 * @param msg_id NUL 终止 UUID
 * @param kind   ack 种类
 * @return ESP_OK / ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_ARG / ESP_ERR_TIMEOUT
 */
esp_err_t tx_emit_ack(tx_dev_t *dev, const char *msg_id, tx_ack_kind_e kind);

/**
 * @brief 主动尝试 drain 一次(drain_task 也会周期调用)
 */
esp_err_t tx_flush(tx_dev_t *dev);

/**
 * @brief 当前 pending 条目数(原子读取)
 */
size_t    tx_pending_count(tx_dev_t *dev);

/**
 * @brief publish 失败累计(MQTT 未连接不计)
 */
uint32_t  tx_get_fail_count(tx_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* TX_H */
