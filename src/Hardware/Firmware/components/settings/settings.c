/**
 * @file settings.c
 * @brief Settings 管理库实现文件
 */

#include "settings.h"
#include <string.h>

// 标签用于日志记录
static const char *TAG = "settings";

// 设置键名
#define SETTINGS_KEY "SETTINGS"

/**
 * @brief 保存设置到 NVS
 */
esp_err_t settings_save(nvs_dev_t *nvs_dev, const app_settings_t *settings)
{
    if (nvs_dev == NULL || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!settings_validate(settings)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 使用 blob 保存整个结构体
    esp_err_t err = nvs_set_blob(nvs_dev->handle, SETTINGS_KEY, settings, sizeof(app_settings_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(err));
        return err;
    }

    // 提交更改
    err = nvs_commit(nvs_dev->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit settings: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Settings saved successfully");
    return ESP_OK;
}

/**
 * @brief 从 NVS 加载设置
 */
esp_err_t settings_load(nvs_dev_t *nvs_dev, app_settings_t *settings)
{
    if (nvs_dev == NULL || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required_size = sizeof(app_settings_t);
    size_t actual_size = required_size;
    
    // 读取 blob 数据
    esp_err_t err = nvs_get_blob(nvs_dev->handle, SETTINGS_KEY, settings, &actual_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 设置不存在，使用默认值
        ESP_LOGW(TAG, "Settings not found, using defaults");
        settings_set_defaults(settings);
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load settings: %s", esp_err_to_name(err));
        return err;
    }

    // 检查数据大小是否匹配
    if (actual_size != required_size) {
        ESP_LOGW(TAG, "Settings size mismatch, using defaults");
        settings_set_defaults(settings);
        return ESP_OK;
    }

    // 验证设置数据
    if (!settings_validate(settings)) {
        ESP_LOGW(TAG, "Settings validation failed, using defaults");
        settings_set_defaults(settings);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Settings loaded successfully");
    return ESP_OK;
}

/**
 * @brief 恢复默认设置
 */
void settings_set_defaults(app_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    app_settings_t defaults = SETTINGS_DEFAULT;
    memcpy(settings, &defaults, sizeof(app_settings_t));
}

/**
 * @brief 验证设置是否有效
 */
bool settings_validate(const app_settings_t *settings)
{
    if (settings == NULL) {
        return false;
    }

    // 暂时没有需要验证的字段，直接返回 true

    return true;
}