#include "i2c_bus_manager.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "I2C_BUS";

// 全局总线句柄数组（支持多个I2C端口）
static i2c_bus_dev_t *bus_handles[I2C_NUM_MAX] = {NULL};

esp_err_t i2c_bus_init(i2c_bus_dev_t **handle, const i2c_bus_config_t *config)
{
    if (handle == NULL) {
        ESP_LOGE(TAG, "Output handle pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        *handle = NULL;
        return ESP_ERR_INVALID_ARG;
    }

    if (config->port >= I2C_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid I2C port: %d", config->port);   
        *handle = NULL;
        return ESP_ERR_INVALID_ARG;
    }

    // 检查是否已经初始化
    if (bus_handles[config->port] != NULL) {
        ESP_LOGE(TAG, "I2C port %d already initialized", config->port);
        *handle = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    // 分配总线句柄
    i2c_bus_dev_t *_handle = malloc(sizeof(i2c_bus_dev_t));
    if (_handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate bus handle");
        *handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(_handle, 0, sizeof(i2c_bus_dev_t));
    _handle->port = config->port;

    // 创建互斥锁
    _handle->mutex = xSemaphoreCreateMutex();
    if (_handle->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(_handle);
        *handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 配置I2C
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = config->sda_io_num,
        .scl_io_num = config->scl_io_num,
        .sda_pullup_en = config->pullup_enable,
        .scl_pullup_en = config->pullup_enable,
        .master.clk_speed = config->clk_speed,
    };

    esp_err_t ret = i2c_param_config(config->port, &i2c_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C parameter config failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(_handle->mutex);
        free(_handle);
        *handle = NULL;
        return ret;
    }

    // 安装I2C驱动
    ret = i2c_driver_install(config->port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(_handle->mutex);
        free(_handle);
        *handle = NULL;
        return ret;
    }

    *handle = _handle;
    (*handle)->initialized = true;
    bus_handles[config->port] = _handle;

    ESP_LOGI(TAG, "I2C bus %d initialized: SDA=%d, SCL=%d, Speed=%ldHz", 
             config->port, config->sda_io_num, config->scl_io_num, config->clk_speed);

    return ESP_OK;
}

esp_err_t i2c_bus_deinit(i2c_bus_dev_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->device_count > 0) {
        ESP_LOGW(TAG, "Deinitializing bus with %ld registered devices", handle->device_count);
    }

    // 卸载I2C驱动
    i2c_driver_delete(handle->port);

    // 删除互斥锁
    if (handle->mutex != NULL) {
        vSemaphoreDelete(handle->mutex);
    }

    // 从全局句柄数组中移除
    if (bus_handles[handle->port] == handle) {
        bus_handles[handle->port] = NULL;
    }

    free(handle);
    return ESP_OK;
}

esp_err_t i2c_bus_take_mutex(i2c_bus_dev_t *handle, TickType_t timeout_ticks)
{
    if (handle == NULL || handle->mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->mutex, timeout_ticks) == pdTRUE) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to take I2C bus mutex (timeout)");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t i2c_bus_give_mutex(i2c_bus_dev_t *handle)
{
    if (handle == NULL || handle->mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t i2c_bus_register_device(i2c_bus_dev_t *handle, uint8_t device_addr, const char *device_name)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->device_count++;
    ESP_LOGI(TAG, "Device registered: %s (0x%02X) on bus %d, total devices: %ld", 
             device_name ? device_name : "Unknown", device_addr, handle->port, handle->device_count);
    
    return ESP_OK;
}

esp_err_t i2c_bus_unregister_device(i2c_bus_dev_t *handle, uint8_t device_addr)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->device_count > 0) {
        handle->device_count--;
        ESP_LOGI(TAG, "Device unregistered: 0x%02X from bus %d, remaining devices: %ld", 
                 device_addr, handle->port, handle->device_count);
    }
    
    return ESP_OK;
}

int i2c_bus_scan(i2c_bus_dev_t *handle, uint8_t *found_devices, int max_devices)
{
    if (handle == NULL || !handle->initialized) {
        return 0;
    }

    int found_count = 0;
    esp_err_t ret;
    i2c_cmd_handle_t cmd;

    ESP_LOGI(TAG, "Scanning I2C bus %d...", handle->port);

    // 获取总线锁
    if (i2c_bus_take_mutex(handle, pdMS_TO_TICKS(1000)) != ESP_OK) {
        return 0;
    }

    for (int addr = 0x08; addr < 0x78; addr++) {
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        ret = i2c_master_cmd_begin(handle->port, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found device at address: 0x%02X", addr);
            if (found_devices != NULL && found_count < max_devices) {
                found_devices[found_count++] = addr;
            } else {
                found_count++;
            }
        }
    }

    i2c_bus_give_mutex(handle);

    ESP_LOGI(TAG, "Scan completed, found %d devices", found_count);
    return found_count;
}