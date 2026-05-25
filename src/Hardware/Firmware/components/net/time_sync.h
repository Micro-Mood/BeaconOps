/**
 * @file time_sync.h
 * @brief SNTP 时间同步组件 — 设置系统时间 + 时区
 *
 * 设计说明:
 *  - ESP-IDF 的 esp_sntp_* 是全局单例(底层 lwip sntp 持全局状态),
 *    无法跑多份,因此本组件不提供 *_dev_t 句柄,只暴露 start/stop/wait
 *    一组无状态 API,语义对齐 backlight_mgr(硬件单例)。
 *  - 通过 sntp_config_t 注入服务器与时区,字段为 NULL 时回退到
 *    config.h 的 SNTP_PRIMARY_SERVER / SNTP_FALLBACK_SERVER / TZ_STRING。
 *  - 同步成功通过 on_sync 回调通知调用方,避免轮询。
 */

#ifndef SNTP_H
#define SNTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"

/**
 * @brief SNTP 同步成功回调
 *
 * @param tv   本次同步得到的 UTC 时间(指针仅在回调期间有效)
 * @param user 用户上下文(sntp_config_t.user)
 */
typedef void (*time_sync_on_sync_fn)(struct timeval *tv, void *user);

/**
 * @brief SNTP 配置
 */
typedef struct {
    const char           *primary_server;    ///< NULL → SNTP_PRIMARY_SERVER 宏
    const char           *fallback_server;   ///< NULL → SNTP_FALLBACK_SERVER 宏(可禁用为 NULL)
    const char           *tz_string;         ///< NULL → TZ_STRING 宏
    time_sync_on_sync_fn  on_sync;           ///< 可选;每次同步成功回调一次
    void                 *user;
} time_sync_config_t;

/**
 * @brief 启动 SNTP — 立即返回,同步在后台进行
 *
 * 重复调用幂等(已启动则返回 ESP_OK 不重复 init)。
 *
 * @param config 可为 NULL,等价于全部字段使用默认值
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t time_sync_start(const time_sync_config_t *config);

/**
 * @brief 停止 SNTP — 释放底层 lwip sntp 状态
 *
 * 未启动时安全返回 ESP_OK。
 */
esp_err_t time_sync_stop(void);

/**
 * @brief 是否已完成至少一次成功同步
 */
bool time_sync_is_synced(void);

/**
 * @brief 阻塞等待首次同步完成
 *
 * @param timeout_ms 超时(毫秒);0 表示仅查询当前状态立即返回
 */
esp_err_t time_sync_wait(uint32_t timeout_ms);

/**
 * @brief 把当前本地时间格式化成 "HH:MM" 字符串
 */
void time_sync_get_hhmm(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_H */
