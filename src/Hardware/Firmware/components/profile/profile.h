/**
 * @file profile.h
 * @brief 60 秒滚动行为聚合器 — 周期发布 profile_delta 心跳数据
 *
 * 设计说明:
 *  - 1Hz 收 behavior_state + intensity 累加;到达窗口结束时打包 JSON
 *    通过注入的 publish_fn 发出,本组件不知道 mqtt 是否存在。
 *  - publish_fn 返回 ESP_ERR_INVALID_STATE 视为"上游暂未就绪"(如 MQTT
 *    未连接),不计入 fail_count;其它非 OK 才计 fail。
 *  - 内部 1Hz timer 任务 + 倒计数;退出用 task notify 唤醒 + stop flag。
 *  - JSON 字段严格遵循 §6 profile_delta spec。
 *
 * 线程安全:
 *  - on_behavior / on_shake 可被任何任务调用,内部 mutex 短临界区。
 */

#ifndef PROFILE_H
#define PROFILE_H

#include <stdint.h>
#include "esp_err.h"
#include "sensor_task.h"
#include "spiffs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一个聚合窗口的快照(供 publish_fn 调用方参考;实际通过 JSON 字符串传出)
 */
typedef struct {
    int64_t  ts_unix;          ///< 窗口结束时刻(秒)
    uint32_t window_s;         ///< 窗口长度
    uint32_t static_s;
    uint32_t walk_slow_s;
    uint32_t walk_fast_s;
    uint32_t run_s;
    uint32_t shake_or_fall_s;
    uint32_t shake_n;
    uint32_t intensity_avg;    ///< 1..10 均值;无样本 → 0
    int32_t  steps_today;      ///< 当日累计步数(LSM6 内置计步);<0 表示未知/不上报
} profile_delta_t;

/**
 * @brief 发布回调
 *
 * @param json_payload NUL 结尾 JSON 字符串(profile 内分配,回调返回后释放)
 * @param user         init 时的 publish_user
 * @return ESP_OK / ESP_ERR_INVALID_STATE(暂跳过,不算错) / 其它(计 fail)
 */
typedef esp_err_t (*profile_publish_fn)(const char *json_payload, void *user);

/**
 * @brief profile 配置
 */
typedef struct {
    uint32_t           window_s;        ///< 0 → PROFILE_WINDOW_S 宏
    profile_publish_fn publish_fn;      ///< 必填
    void              *publish_user;
    uint8_t            task_prio;       ///< 0 → tskIDLE_PRIORITY + 1
    uint16_t           task_stack;      ///< 0 → 3072

    /* store-and-forward 队列 —— NULL 表示不启用持久化,publish 失败丢弃 */
    spiffs_dev_t      *spiffs;          ///< 可选;传入后启用离线队列
    uint16_t           queue_max;       ///< 0 → 200 条上限(超出删最老)
    uint8_t            drain_per_tick;  ///< 0 → 8 每次 publish 成功后补发几条
} profile_config_t;

typedef struct profile_dev_s profile_dev_t;

esp_err_t profile_init  (profile_dev_t **dev, const profile_config_t *config);
esp_err_t profile_deinit(profile_dev_t **dev);

/** 1Hz 行为采样(由 sensor_task on_behavior 转发) */
esp_err_t profile_on_behavior(profile_dev_t *dev,
                              behavior_state_e st, int intensity_1_10);

/** 摇晃事件(由 sensor_task on_shake 转发) */
esp_err_t profile_on_shake(profile_dev_t *dev);

/** 上报当日累计步数(由主循环调;负值则下次 publish 省略 steps 字段) */
esp_err_t profile_set_steps_today(profile_dev_t *dev, int32_t steps);

/** publish 失败计数(ESP_ERR_INVALID_STATE 不计) */
uint32_t  profile_get_fail_count(profile_dev_t *dev);

/** 当前离线队列中积压的条数(spiffs 未启用返回 0) */
uint32_t  profile_queue_count(profile_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_H */
