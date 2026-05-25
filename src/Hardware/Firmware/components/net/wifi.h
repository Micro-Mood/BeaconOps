/**
 * @file wifi.h
 * @brief Wi-Fi 管理组件 — 多 SSID 扫描 + RSSI 排序 + 自动重连
 *
 * 设计说明:
 *  - esp_wifi 底层全局唯一,因此多 dev 实例无意义。本组件仍采用
 *    *_dev_t 句柄模式以便集中保管"凭据列表 / 持久化回调 / 状态回调
 *    / 超时配置 / 已连接状态"等实例级状态,代替散落的 static 全局。
 *    重复 wifi_mgr_init() 返回 ESP_ERR_INVALID_STATE。
 *  - 凭据通过 wifi_config_t.cred_list 注入(数组,以 ssid==NULL 终止);
 *    NULL 时回退到 config.h 的 WIFI_CRED_LIST 宏。
 *  - last-good SSID 通过 load/save 回调由调用方持久化,本组件不直接
 *    碰 NVS。
 *
 * 行为:
 *  1. wifi_mgr_connect_blocking 优先尝试 last-good SSID(timeout/2)
 *  2. 主动扫描 → 与凭据列表求交集 → 按 RSSI 降序尝试
 *  3. 连接成功后通过 save_last_good 回调持久化新 SSID
 */

#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Wi-Fi 凭据
 */
typedef struct {
    const char *ssid;       ///< NULL 表示数组终止
    const char *password;   ///< NULL 视作开放网络
} wifi_credential_t;

/**
 * @brief Wi-Fi 状态事件
 */
typedef enum {
    WIFI_EVT_LINKING = 0,       ///< 开始尝试连接
    WIFI_EVT_CONNECTED,         ///< 已关联且取得 IP
    WIFI_EVT_DISCONNECTED,      ///< 断开
    WIFI_EVT_FAILED,            ///< 连接尝试全部失败
} wifi_event_e;

typedef void (*wifi_status_fn)(wifi_event_e evt, void *user);
typedef void (*wifi_load_ssid_fn)(char *out, size_t len, void *user);
typedef void (*wifi_save_ssid_fn)(const char *ssid, void *user);

/**
 * @brief Wi-Fi 配置
 *
 * 注:命名为 wifi_mgr_config_t 而非 wifi_config_t,后者已被 esp_wifi.h
 * 占用(union)。
 */
typedef struct {
    /* 凭据来源 */
    const wifi_credential_t *cred_list;        ///< NULL → WIFI_CRED_LIST 宏
    /* 状态回调(可选) */
    wifi_status_fn           on_status;
    void                    *status_user;
    /* last-good SSID 持久化(可选,二者必须同时给或同时缺) */
    wifi_load_ssid_fn        load_last_good;
    wifi_save_ssid_fn        save_last_good;
    void                    *persist_user;
    /* 时序参数(0 → 用宏) */
    uint32_t                 connect_timeout_ms;  ///< 0 → WIFI_CONNECT_TIMEOUT_MS
    uint32_t                 scan_passive_ms;     ///< 0 → WIFI_SCAN_PASSIVE_MS
} wifi_mgr_config_t;

/**
 * @brief Wi-Fi 设备句柄(不透明)
 */
typedef struct wifi_dev_s wifi_dev_t;

/**
 * @brief 初始化 Wi-Fi 子系统并启动 STA 模式
 *
 * 不阻塞;仅初始化栈和事件,不主动连接(请调用 wifi_mgr_connect_blocking)。
 * 重复调用返回 ESP_ERR_INVALID_STATE。
 *
 * @param dev    输出句柄
 * @param config 配置(必填)
 * @return esp_err_t ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM /
 *                   ESP_ERR_INVALID_STATE / 其它 esp_wifi 错误
 */
esp_err_t wifi_mgr_init(wifi_dev_t **dev, const wifi_mgr_config_t *config);

/**
 * @brief 反初始化 — esp_wifi_stop / deinit / 注销事件 / 释放
 *
 * @param dev 句柄指针(完成后置 NULL)
 * @return esp_err_t ESP_OK
 */
esp_err_t wifi_mgr_deinit(wifi_dev_t **dev);

/**
 * @brief 阻塞执行一次连接尝试 — 扫描 + 排序 + 逐个尝试
 *
 * @param dev        句柄
 * @param timeout_ms 总超时(毫秒);0 → 用 cfg.connect_timeout_ms
 * @return esp_err_t ESP_OK / ESP_ERR_TIMEOUT / ESP_ERR_NOT_FOUND / ESP_FAIL
 */
esp_err_t wifi_mgr_connect_blocking(wifi_dev_t *dev, uint32_t timeout_ms);

/**
 * @brief 当前是否已关联且取得 IP
 *
 * @param dev 句柄
 * @return true / false
 */
bool wifi_mgr_is_connected(wifi_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
