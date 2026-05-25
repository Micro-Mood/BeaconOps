/**
 * @file nvslib.h
 * @brief NVS (Non-Volatile Storage) 管理库头文件
 */

#ifndef NVSLIB_H
#define NVSLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvslib.h"
#include <stdbool.h>
#include "config.h"

/**
 * @brief NVS 操作结果枚举
 */
typedef enum {
    NVS_OK = 0,                  ///< 操作成功
    NVS_ERR_INIT_FAILED,         ///< 初始化失败
    NVS_ERR_OPEN_FAILED,         ///< 打开命名空间失败
    NVS_ERR_INVALID_PARAM,       ///< 参数无效
    NVS_ERR_NO_MEMORY,           ///< 内存不足
    NVS_ERR_UNKNOWN              ///< 未知错误
} nvs_result_t;

/**
 * @brief NVS 设备结构体
 */
typedef struct {
    nvs_handle_t handle;         ///< NVS 句柄
    char namespace_[32];         ///< 命名空间名称
    bool initialized;            ///< 初始化状态
} nvs_dev_t;

/**
 * @brief 初始化 NVS 设备
 * 
 * @param dev 设备句柄指针
 * @param namespace_ 命名空间名称
 * @return nvs_result_t 执行结果
 */
nvs_result_t nvs_init(nvs_dev_t **dev, const char *namespace_);

/**
 * @brief 反初始化 NVS 设备
 * 
 * @param dev 设备句柄指针
 * @return nvs_result_t 执行结果
 */
nvs_result_t nvs_deinit(nvs_dev_t **dev);

/**
 * @brief 获取错误信息字符串
 * 
 * @param result 错误码
 * @return const char* 错误信息字符串
 */
const char* nvs_get_error_string(nvs_result_t result);

#ifdef __cplusplus
}
#endif

#endif // NVSLIB_H