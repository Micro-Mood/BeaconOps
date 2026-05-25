/**
 * @file settings.h
 * @brief Settings 管理库头文件
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nvs_flash.h"
#include "nvslib.h"
#include "esp_err.h"
#include <stdbool.h>

// 定义你的设置结构体
typedef struct {
    uint32_t version;           // 版本号
    char device_name[32];       // 设备名称
    uint32_t device_id;         // 设备ID
    bool cw2017_profile_loaded; // 是否已加载CW2017电池配置
    char last_good_ssid[33];    // 上次成功连接的Wi-Fi SSID(WIFI_CRED_LIST 中的一项,空串=无)
} app_settings_t;

/**
 * @brief 保存设置到 NVS
 * 
 * @param nvs_dev NVS 设备句柄
 * @param settings 设置结构体指针
 * @return esp_err_t ESP错误码
 */
esp_err_t settings_save(nvs_dev_t *nvs_dev, const app_settings_t *settings);

/**
 * @brief 从 NVS 加载设置
 * 
 * @param nvs_dev NVS 设备句柄
 * @param settings 设置结构体指针
 * @return esp_err_t ESP错误码
 */
esp_err_t settings_load(nvs_dev_t *nvs_dev, app_settings_t *settings);

/**
 * @brief 恢复默认设置
 * 
 * @param settings 设置结构体指针
 */
void settings_set_defaults(app_settings_t *settings);

/**
 * @brief 验证设置是否有效
 * 
 * @param settings 设置结构体指针
 * @return true 有效
 * @return false 无效
 */
bool settings_validate(const app_settings_t *settings);

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_H