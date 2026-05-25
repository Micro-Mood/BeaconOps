/**
 * @file spiffs.c
 * @brief SPIFFS 文件系统管理库实现文件
 */

#include "spiffs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

// 标签用于日志记录
static const char *TAG = "spiffs_manager";

// 默认配置
static const spiffs_config_t spiffs_default_config = {
    .base_path = SPIFFS_DEFAULT_BASE_PATH,
    .partition_label = SPIFFS_DEFAULT_PARTITION_LABEL,
    .max_files = SPIFFS_DEFAULT_MAX_FILES,
    .format_if_mount_failed = true
};

/**
 * @brief 创建完整的文件路径
 */
static esp_err_t make_full_path(spiffs_dev_t *dev, const char *filename, char *full_path, size_t max_len)
{
    if (dev == NULL || filename == NULL || full_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(full_path, max_len, "%s/%s", dev->base_path, filename);
    if (ret < 0 || ret >= max_len) {
        ESP_LOGE(TAG, "Path too long: %s/%s", dev->base_path, filename);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief 检查分区是否存在且可访问
 */
static spiffs_result_t check_partition_exists(const char *partition_label)
{
    if (partition_label == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_label);
    
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition not found: %s", partition_label);
        return SPIFFS_ERR_INIT_FAILED;
    }

    return SPIFFS_OK;
}

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
const char* spiffs_get_error_string(spiffs_result_t result)
{
    switch (result) {
        case SPIFFS_OK: return "Success";
        case SPIFFS_ERR_INIT_FAILED: return "Initialization failed";
        case SPIFFS_ERR_MOUNT_FAILED: return "Mount failed";
        case SPIFFS_ERR_INVALID_PARAM: return "Invalid parameter";
        case SPIFFS_ERR_NO_MEMORY: return "Out of memory";
        case SPIFFS_ERR_UNKNOWN: return "Unknown error";
        default: return "Undefined error";
    }
}

/**
 * @brief 初始化 SPIFFS 设备
 */
spiffs_result_t spiffs_init(spiffs_dev_t **dev, const spiffs_config_t *config)
{
    if (dev == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    // 分配设备内存
    *dev = (spiffs_dev_t *)malloc(sizeof(spiffs_dev_t));
    if (*dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for SPIFFS device");
        return SPIFFS_ERR_NO_MEMORY;
    }

    memset(*dev, 0, sizeof(spiffs_dev_t));

    // 使用默认配置或用户配置
    spiffs_config_t actual_config;
    if (config != NULL) {
        actual_config = *config;
    } else {
        actual_config = spiffs_default_config;
    }

    // 检查分区是否存在
    spiffs_result_t part_result = check_partition_exists(actual_config.partition_label);
    if (part_result != SPIFFS_OK) {
        free(*dev);
        *dev = NULL;
        return part_result;
    }

    // 复制配置参数
    safe_strncpy((*dev)->base_path, actual_config.base_path, sizeof((*dev)->base_path));
    safe_strncpy((*dev)->partition_label, actual_config.partition_label, sizeof((*dev)->partition_label));
    (*dev)->max_files = actual_config.max_files;

    ESP_LOGI(TAG, "Initializing SPIFFS at %s (partition: %s)", 
             (*dev)->base_path, (*dev)->partition_label);

    // 配置 SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = (*dev)->base_path,
        .partition_label = (*dev)->partition_label,
        .max_files = (*dev)->max_files,
        .format_if_mount_failed = actual_config.format_if_mount_failed
    };

    // 挂载 SPIFFS
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        free(*dev);
        *dev = NULL;
        return SPIFFS_ERR_MOUNT_FAILED;
    }

    (*dev)->mounted = true;

    // 获取文件系统信息
    spiffs_get_fs_info(*dev, &(*dev)->total_size, &(*dev)->used_size);

    ESP_LOGI(TAG, "SPIFFS mounted successfully, total: %d KB, used: %d KB", 
             (*dev)->total_size / 1024, (*dev)->used_size / 1024);

    return SPIFFS_OK;
}

/**
 * @brief 反初始化 SPIFFS 设备
 */
spiffs_result_t spiffs_deinit(spiffs_dev_t **dev)
{
    if (dev == NULL || *dev == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    ESP_LOGI(TAG, "Unmounting SPIFFS from %s", (*dev)->base_path);

    // 卸载 SPIFFS
    esp_err_t ret = esp_vfs_spiffs_unregister((*dev)->partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SPIFFS (%s)", esp_err_to_name(ret));
        // 继续执行清理，即使卸载失败
    }

    (*dev)->mounted = false;
    free(*dev);
    *dev = NULL;

    ESP_LOGI(TAG, "SPIFFS unmounted successfully");

    return SPIFFS_OK;
}

/**
 * @brief 获取文件信息
 */
static spiffs_result_t get_file_info(spiffs_dev_t *dev, const char *filename,
                                    spiffs_file_info_t *info)
{
    if (dev == NULL || filename == NULL || info == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    if (!dev->mounted) {
        return SPIFFS_ERR_INIT_FAILED;
    }

    char full_path[SPIFFS_MAX_PATH_LENGTH];
    esp_err_t ret = make_full_path(dev, filename, full_path, sizeof(full_path));
    if (ret != ESP_OK) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        return SPIFFS_ERR_UNKNOWN;
    }

    // 填充文件信息
    safe_strncpy(info->filename, filename, sizeof(info->filename));
    info->size = st.st_size;
    info->modified_time = st.st_mtime;
    info->is_directory = S_ISDIR(st.st_mode);

    return SPIFFS_OK;
}

/**
 * @brief 列出目录内容
 */
spiffs_result_t spiffs_list_directory(spiffs_dev_t *dev, const char *path,
                                     spiffs_dir_list_t *list)
{
    if (dev == NULL || path == NULL || list == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    if (!dev->mounted) {
        return SPIFFS_ERR_INIT_FAILED;
    }

    char full_path[SPIFFS_MAX_PATH_LENGTH];
    esp_err_t ret = make_full_path(dev, path, full_path, sizeof(full_path));
    if (ret != ESP_OK) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    // 打开目录
    DIR *dir = opendir(full_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return SPIFFS_ERR_UNKNOWN;
    }

    // 初始化列表
    list->count = 0;
    list->capacity = 16; // 初始容量
    list->files = (spiffs_file_info_t *)malloc(list->capacity * sizeof(spiffs_file_info_t));
    if (list->files == NULL) {
        closedir(dir);
        return SPIFFS_ERR_NO_MEMORY;
    }

    // 读取目录项
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 "." 和 ".." 目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // 如果列表已满，扩容
        if (list->count >= list->capacity) {
            list->capacity *= 2;
            spiffs_file_info_t *new_files = (spiffs_file_info_t *)realloc(
                list->files, list->capacity * sizeof(spiffs_file_info_t));
            if (new_files == NULL) {
                free(list->files);
                closedir(dir);
                return SPIFFS_ERR_NO_MEMORY;
            }
            list->files = new_files;
        }

        // 构建完整路径获取文件信息
        char item_path[SPIFFS_MAX_PATH_LENGTH * 2];
        if (strlen(path) == 0 || strcmp(path, "/") == 0) {
            snprintf(item_path, sizeof(item_path), "%s", entry->d_name);
        } else {
            snprintf(item_path, sizeof(item_path), "%s/%s", path, entry->d_name);
        }

        spiffs_file_info_t *file_info = &list->files[list->count];
        spiffs_result_t info_ret = get_file_info(dev, item_path, file_info);
        if (info_ret == SPIFFS_OK) {
            list->count++;
        }
    }

    closedir(dir);

    ESP_LOGD(TAG, "Listed directory: %s (%d items)", path, list->count);

    return SPIFFS_OK;
}

/**
 * @brief 释放目录列表内存
 */
void spiffs_free_dir_list(spiffs_dir_list_t *list)
{
    if (list != NULL && list->files != NULL) {
        free(list->files);
        list->files = NULL;
        list->count = 0;
        list->capacity = 0;
    }
}

/**
 * @brief 获取文件系统信息
 */
spiffs_result_t spiffs_get_fs_info(spiffs_dev_t *dev, size_t *total_size, size_t *used_size)
{
    if (dev == NULL || total_size == NULL || used_size == NULL) {
        return SPIFFS_ERR_INVALID_PARAM;
    }

    if (!dev->mounted) {
        return SPIFFS_ERR_INIT_FAILED;
    }

    esp_err_t ret = esp_spiffs_info(dev->partition_label, total_size, used_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get filesystem info (%s)", esp_err_to_name(ret));
        return SPIFFS_ERR_UNKNOWN;
    }

    return SPIFFS_OK;
}