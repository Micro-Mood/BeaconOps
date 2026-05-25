/**
 * @file nvslib.c
 * @brief NVS (Non-Volatile Storage) 管理库实现文件
 */

#include "nvslib.h"
#include <string.h>
#include <stdlib.h>

// 标签用于日志记录
static const char *TAG = "nvs_manager";

/**
 * @brief 安全字符串拷贝
 */
static void safe_strncpy(char *dest, const char *src, size_t dest_size)
{
    if (dest == NULL || src == NULL || dest_size == 0) {
        return;
    }
    
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/**
 * @brief 获取错误信息字符串
 */
const char* nvs_get_error_string(nvs_result_t result)
{
    switch (result) {
        case NVS_OK: return "Success";
        case NVS_ERR_INIT_FAILED: return "NVS initialization failed";
        case NVS_ERR_OPEN_FAILED: return "Failed to open NVS namespace";
        case NVS_ERR_INVALID_PARAM: return "Invalid parameter";
        case NVS_ERR_NO_MEMORY: return "Out of memory";
        case NVS_ERR_UNKNOWN: return "Unknown error";
        default: return "Undefined error";
    }
}

/**
 * @brief 初始化 NVS 设备
 */
nvs_result_t nvs_init(nvs_dev_t **dev, const char *namespace_)
{
    if (dev == NULL || namespace_ == NULL) {
        return NVS_ERR_INVALID_PARAM;
    }

    // 分配设备内存
    *dev = (nvs_dev_t *)malloc(sizeof(nvs_dev_t));
    if (*dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for NVS device");
        return NVS_ERR_NO_MEMORY;
    }

    memset(*dev, 0, sizeof(nvs_dev_t));
    safe_strncpy((*dev)->namespace_, namespace_, sizeof((*dev)->namespace_));

    ESP_LOGI(TAG, "Initializing NVS with namespace: %s", namespace_);

    // 初始化 NVS 闪存
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区被截断或包含新版本，需要擦除
        ESP_LOGW(TAG, "NVS partition truncated or contains new version, erasing...");
        
        // 擦除 NVS 分区
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(ret));
            free(*dev);
            *dev = NULL;
            return NVS_ERR_INIT_FAILED;
        }
        
        // 重新初始化 NVS
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        free(*dev);
        *dev = NULL;
        return NVS_ERR_INIT_FAILED;
    }

    // 打开命名空间
    ret = nvs_open(namespace_, NVS_READWRITE, &(*dev)->handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        nvs_flash_deinit();
        free(*dev);
        *dev = NULL;
        return NVS_ERR_OPEN_FAILED;
    }

    (*dev)->initialized = true;

    ESP_LOGI(TAG, "NVS initialized successfully with namespace: %s", namespace_);

    return NVS_OK;
}

/**
 * @brief 反初始化 NVS 设备
 */
nvs_result_t nvs_deinit(nvs_dev_t **dev)
{
    if (dev == NULL || *dev == NULL) {
        return NVS_ERR_INVALID_PARAM;
    }

    ESP_LOGI(TAG, "Deinitializing NVS namespace: %s", (*dev)->namespace_);

    if ((*dev)->initialized) {
        // 关闭 NVS 句柄
        nvs_close((*dev)->handle);
        
        // 反初始化 NVS 闪存
        nvs_flash_deinit();
        
        (*dev)->initialized = false;
    }

    free(*dev);
    *dev = NULL;

    ESP_LOGI(TAG, "NVS deinitialized successfully");

    return NVS_OK;
}