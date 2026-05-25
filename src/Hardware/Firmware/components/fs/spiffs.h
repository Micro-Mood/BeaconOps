/**
 * @file spiffs.h
 * @brief SPIFFS 文件系统管理库头文件
 */

#ifndef SPIFFS_H
#define SPIFFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "sys/stat.h"
#include "dirent.h"
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include "config.h"

/**
 * @brief SPIFFS 操作结果枚举
 */
typedef enum {
    SPIFFS_OK = 0,                  ///< 操作成功
    SPIFFS_ERR_INIT_FAILED,         ///< 初始化失败
    SPIFFS_ERR_MOUNT_FAILED,        ///< 挂载失败
    SPIFFS_ERR_INVALID_PARAM,       ///< 参数无效
    SPIFFS_ERR_NO_MEMORY,           ///< 内存不足
    SPIFFS_ERR_UNKNOWN              ///< 未知错误
} spiffs_result_t;

/**
 * @brief 文件信息结构体
 */
typedef struct {
    char filename[SPIFFS_MAX_PATH_LENGTH];          ///< 文件名
    size_t size;                                    ///< 文件大小
    time_t modified_time;                           ///< 修改时间
    bool is_directory;                              ///< 是否为目录
} spiffs_file_info_t;

/**
 * @brief 目录列表结构体
 */
typedef struct {
    spiffs_file_info_t *files;      ///< 文件信息数组
    size_t count;                   ///< 文件数量
    size_t capacity;                ///< 数组容量
} spiffs_dir_list_t;

/**
 * @brief SPIFFS 配置结构体
 */
typedef struct {
    char base_path[SPIFFS_MAX_PATH_LENGTH];             ///< 挂载点路径
    char partition_label[32];                           ///< 分区标签
    size_t max_files;                                   ///< 最大打开文件数
    bool format_if_mount_failed;                        ///< 挂载失败时是否格式化
} spiffs_config_t;

/**
 * @brief SPIFFS 设备结构体
 */
typedef struct {
    char base_path[SPIFFS_MAX_PATH_LENGTH];             ///< 挂载点路径
    char partition_label[32];                           ///< 分区标签
    size_t max_files;                                   ///< 最大打开文件数
    bool mounted;                                       ///< 挂载状态
    size_t total_size;                                  ///< 总空间大小
    size_t used_size;                                   ///< 已用空间大小
} spiffs_dev_t;

/**
 * @brief 初始化 SPIFFS 设备
 * 
 * @param dev 设备句柄指针
 * @param config 设备配置参数
 * @return spiffs_result_t 执行结果
 */
spiffs_result_t spiffs_init(spiffs_dev_t **dev, const spiffs_config_t *config);

/**
 * @brief 反初始化 SPIFFS 设备
 * 
 * @param dev 设备句柄指针
 * @return spiffs_result_t 执行结果
 */
spiffs_result_t spiffs_deinit(spiffs_dev_t **dev);

/**
 * @brief 列出目录内容
 * 
 * @param dev 设备句柄
 * @param path 目录路径
 * @param list 目录列表结构体指针
 * @return spiffs_result_t 执行结果
 */
spiffs_result_t spiffs_list_directory(spiffs_dev_t *dev, const char *path,
                                     spiffs_dir_list_t *list);

/**
 * @brief 释放目录列表内存
 * 
 * @param list 目录列表结构体指针
 */
void spiffs_free_dir_list(spiffs_dir_list_t *list);

/**
 * @brief 获取文件系统信息
 * 
 * @param dev 设备句柄
 * @param total_size 总空间大小指针
 * @param used_size 已用空间大小指针
 * @return spiffs_result_t 执行结果
 */
spiffs_result_t spiffs_get_fs_info(spiffs_dev_t *dev, size_t *total_size, size_t *used_size);

/**
 * @brief 获取错误信息字符串
 * 
 * @param result 错误码
 * @return const char* 错误信息字符串
 */
const char* spiffs_get_error_string(spiffs_result_t result);

#ifdef __cplusplus
}
#endif

#endif // SPIFFS_H