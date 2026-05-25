/**
 * @file notify.h
 * @brief 提示音组件 — 按消息等级播放手写合成提示音
 *
 * 设计说明:
 *  - 4 个等级(INFO/NOTICE/WARN/EMERG)和确认音均由固定频率短音组合生成。
 *  - 非阻塞:notify_play 仅入队,worker task 串行播放;持
 *    audio_session_acquire 整段时间,与 TTS 等其它音源在 audio
 *    层互斥。
 *  - 不持有 audio 设备的所有权 — 由调用方 init 时通过 audio 字段注入。
 *  - scratch 缓冲堆分配,deinit 释放;尺寸 = sample_rate * scratch_ms / 1000。
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "audio.h"

/**
 * @brief 提示音等级 — 与消息卡片等级一一对应
 */
typedef enum {
    NOTIFY_LV_INFO   = 0,    ///< 普通提示音
    NOTIFY_LV_NOTICE = 1,    ///< 注意提示音
    NOTIFY_LV_WARN   = 2,    ///< 警告提示音
    NOTIFY_LV_EMERG  = 3,    ///< 紧急提示音
    NOTIFY_LV_MAX
} notify_level_e;

/**
 * @brief 提示音组件配置
 */
typedef struct {
    audio_dev_t *audio;                   ///< 必填,已初始化的 audio 句柄
    uint32_t     sample_rate;             ///< 0 → AUDIO_SAMPLE_RATE 宏
    uint16_t     amplitude;               ///< 0 → 50000;合成音峰值幅度
    uint16_t     scratch_ms;              ///< 0 → 50;scratch 缓冲对应毫秒
    uint8_t      queue_depth;             ///< 0 → 8
    uint8_t      task_prio;               ///< 0 → tskIDLE_PRIORITY + 2
    uint16_t     task_stack;              ///< 0 → 3072
    uint32_t     session_lock_timeout_ms; ///< 0 → 1000;UINT32_MAX → portMAX_DELAY
} notify_config_t;

/**
 * @brief 提示音设备句柄(不透明)
 */
typedef struct notify_dev_s notify_dev_t;

/**
 * @brief 初始化提示音组件
 *
 * @param dev    输出句柄
 * @param config 配置(必填)
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / ESP_FAIL
 */
esp_err_t notify_init(notify_dev_t **dev, const notify_config_t *config);

/**
 * @brief 反初始化 — 停止 worker,释放队列/scratch
 *
 * @param dev 句柄指针(完成后置 NULL)
 * @return esp_err_t ESP_OK
 */
esp_err_t notify_deinit(notify_dev_t **dev);

/**
 * @brief 触发一次提示音(非阻塞,入队)
 *
 * 队列满时返回 ESP_ERR_NO_MEM,本次丢弃。
 *
 * @param dev   句柄
 * @param level 提示等级
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_ARG /
 *                   ESP_ERR_NO_MEM
 */
esp_err_t notify_play(notify_dev_t *dev, notify_level_e level);

/** 播放一段短确认音;用于摇晃确认当前消息成功。 */
esp_err_t notify_play_confirm(notify_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_H */
