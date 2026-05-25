/**
 * @file sensor.c
 * @brief LSM6DS3TR-C传感器驱动实现
 */

#include "sensor.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>

static const char *TAG = "SENSOR";

// 静态函数声明
static int32_t sensor_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len);
static int32_t sensor_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len);

/**
 * @brief I2C写函数
 */
static int32_t sensor_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len)
{
    sensor_dev_t *sensor = (sensor_dev_t *)handle;
    
    // 获取总线访问权
    if (i2c_bus_take_mutex(sensor->config.i2c_bus, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to take I2C bus mutex");
        return -1;
    }
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (sensor->config.i2c_address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, (uint8_t*)bufp, len, true);
    i2c_master_stop(cmd);
    
    esp_err_t ret = i2c_master_cmd_begin(sensor->config.i2c_bus->port, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    // 释放总线访问权
    i2c_bus_give_mutex(sensor->config.i2c_bus);
    
    return (ret == ESP_OK) ? 0 : -1;
}

/**
 * @brief I2C读函数
 */
static int32_t sensor_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len)
{
    sensor_dev_t *sensor = (sensor_dev_t *)handle;
    
    // 获取总线访问权
    if (i2c_bus_take_mutex(sensor->config.i2c_bus, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to take I2C bus mutex");
        return -1;
    }
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (sensor->config.i2c_address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (sensor->config.i2c_address << 1) | I2C_MASTER_READ, true);
    
    if (len > 1) {
        i2c_master_read(cmd, bufp, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, bufp + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    
    esp_err_t ret = i2c_master_cmd_begin(sensor->config.i2c_bus->port, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    // 释放总线访问权
    i2c_bus_give_mutex(sensor->config.i2c_bus);
    
    return (ret == ESP_OK) ? 0 : -1;
}

/**
 * @brief 初始化传感器（分配并返回句柄）
 */
esp_err_t sensor_init(sensor_dev_t **handle, const sensor_config_t *config)
{
    if (handle == NULL || config == NULL || config->i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing sensor");

    sensor_dev_t *dev = (sensor_dev_t *)calloc(1, sizeof(sensor_dev_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sensor_dev_t");
        return ESP_ERR_NO_MEM;
    }

    // 复制配置
    dev->config = *config;

    // 初始化设备上下文
    dev->dev_ctx.write_reg = sensor_write;
    dev->dev_ctx.read_reg = sensor_read;
    dev->dev_ctx.handle = dev;

    // 注册设备到 I2C 总线管理器
    esp_err_t ret = i2c_bus_register_device(dev->config.i2c_bus, dev->config.i2c_address, dev->config.device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register device on I2C bus: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    // 检查设备ID
    uint8_t whoamI;
    ret = sensor_get_device_id(dev, &whoamI);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID");
        i2c_bus_unregister_device(dev->config.i2c_bus, dev->config.i2c_address);
        free(dev);
        return ret;
    }

    if (whoamI != LSM6DS3TR_C_ID) {
        ESP_LOGE(TAG, "Invalid device ID: 0x%02X, expected: 0x%02X", whoamI, LSM6DS3TR_C_ID);
        i2c_bus_unregister_device(dev->config.i2c_bus, dev->config.i2c_address);
        free(dev);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Device ID verified: 0x%02X", whoamI);

    // 软件复位
    ret = sensor_reset(dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset sensor");
        i2c_bus_unregister_device(dev->config.i2c_bus, dev->config.i2c_address);
        free(dev);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // 配置基本参数
    lsm6ds3tr_c_block_data_update_set(&dev->dev_ctx, PROPERTY_ENABLE);

    // 配置加速度计 - 104Hz, ±8g
    lsm6ds3tr_c_xl_data_rate_set(&dev->dev_ctx, LSM6DS3TR_C_XL_ODR_104Hz);
    lsm6ds3tr_c_xl_full_scale_set(&dev->dev_ctx, LSM6DS3TR_C_8g);

    // 配置陀螺仪 - 104Hz, ±500dps
    lsm6ds3tr_c_gy_data_rate_set(&dev->dev_ctx, LSM6DS3TR_C_GY_ODR_104Hz);
    lsm6ds3tr_c_gy_full_scale_set(&dev->dev_ctx, LSM6DS3TR_C_500dps);

    // 配置中断引脚极性（低电平有效）
    lsm6ds3tr_c_pin_mode_set(&dev->dev_ctx, LSM6DS3TR_C_PUSH_PULL);
    lsm6ds3tr_c_pin_polarity_set(&dev->dev_ctx, LSM6DS3TR_C_ACTIVE_LOW);

    // 配置中断通知模式为锁存
    lsm6ds3tr_c_int_notification_set(&dev->dev_ctx, LSM6DS3TR_C_INT_LATCHED);

    /* 启用内置计步器（调用 pedo_sens_set 会同时置位 func_en）。
     * 启用后从寄存器 0x4B/0x4C 读三轴加速计出的总步数，无需软件后处理 */
    if (lsm6ds3tr_c_pedo_sens_set(&dev->dev_ctx, PROPERTY_ENABLE) != 0) {
        ESP_LOGW(TAG, "enable pedometer failed (non-fatal)");
    } else {
        ESP_LOGI(TAG, "pedometer enabled");
    }

    dev->initialized = true;
    *handle = dev;

    ESP_LOGI(TAG, "Sensor initialized successfully");
    return ESP_OK;
}

/**
 * @brief 释放传感器资源
 */
esp_err_t sensor_deinit(sensor_dev_t *handle)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    // 禁用wakeup功能
    sensor_disable_wakeup(handle);

    // 注销设备并释放内存
    i2c_bus_unregister_device(handle->config.i2c_bus, handle->config.i2c_address);

    handle->initialized = false;

    free(handle);

    ESP_LOGI(TAG, "Sensor deinitialized");
    return ESP_OK;
}

/**
 * @brief 读取6轴传感器数据
 */
esp_err_t sensor_read_data(sensor_dev_t *handle, sensor_data_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int16_t raw_accel[3], raw_gyro[3];
    
    // 读取加速度数据
    if (lsm6ds3tr_c_acceleration_raw_get(&handle->dev_ctx, raw_accel) != 0) {
        ESP_LOGE(TAG, "Failed to read acceleration data");
        return ESP_FAIL;
    }
    
    // 读取陀螺仪数据
    if (lsm6ds3tr_c_angular_rate_raw_get(&handle->dev_ctx, raw_gyro) != 0) {
        ESP_LOGE(TAG, "Failed to read gyroscope data");
        return ESP_FAIL;
    }
    
    // 转换数据
    data->accel_x = lsm6ds3tr_c_from_fs4g_to_mg(raw_accel[0]);
    data->accel_y = lsm6ds3tr_c_from_fs4g_to_mg(raw_accel[1]);
    data->accel_z = lsm6ds3tr_c_from_fs4g_to_mg(raw_accel[2]);
    
    data->gyro_x = lsm6ds3tr_c_from_fs500dps_to_mdps(raw_gyro[0]);
    data->gyro_y = lsm6ds3tr_c_from_fs500dps_to_mdps(raw_gyro[1]);
    data->gyro_z = lsm6ds3tr_c_from_fs500dps_to_mdps(raw_gyro[2]);
    
    data->timestamp = xTaskGetTickCount();
    
    return ESP_OK;
}

/**
 * @brief 启用wakeup功能
 */
esp_err_t sensor_enable_wakeup(sensor_dev_t *handle)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 设置wakeup参数
    lsm6ds3tr_c_wkup_threshold_set(&handle->dev_ctx, handle->config.wakeup_threshold);
    lsm6ds3tr_c_wkup_dur_set(&handle->dev_ctx, handle->config.wakeup_duration);
    
    // 配置INT1唤醒中断
    lsm6ds3tr_c_int1_route_t int1_route = {
        .int1_wu = 1  // 使能唤醒中断到INT1
    };
    
    if (lsm6ds3tr_c_pin_int1_route_set(&handle->dev_ctx, int1_route) != 0) {
        ESP_LOGE(TAG, "Failed to enable wakeup");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Wakeup enabled, threshold: 0x%02X, duration: 0x%02X", 
             handle->config.wakeup_threshold, handle->config.wakeup_duration);
    return ESP_OK;
}

/**
 * @brief 禁用wakeup功能
 */
esp_err_t sensor_disable_wakeup(sensor_dev_t *handle)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 禁用INT1唤醒中断
    lsm6ds3tr_c_int1_route_t int1_route = {
        .int1_wu = 0  // 禁用唤醒中断
    };
    
    if (lsm6ds3tr_c_pin_int1_route_set(&handle->dev_ctx, int1_route) != 0) {
        ESP_LOGE(TAG, "Failed to disable wakeup");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Wakeup disabled");
    return ESP_OK;
}

/**
 * @brief 清除中断锁存
 */
esp_err_t sensor_clear_interrupt(sensor_dev_t *handle){
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    lsm6ds3tr_c_all_sources_t all_sources;

    if (lsm6ds3tr_c_all_sources_get(&handle->dev_ctx, &all_sources) != 0) {
        ESP_LOGE(TAG, "Failed to clear interrupt latch");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 检查是否发生wakeup事件
 */
esp_err_t sensor_check_wakeup(sensor_dev_t *handle, bool *wakeup_occurred)
{
    if (handle == NULL || !handle->initialized || wakeup_occurred == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lsm6ds3tr_c_all_sources_t all_sources;
    
    if (lsm6ds3tr_c_all_sources_get(&handle->dev_ctx, &all_sources) == 0) {
        if (all_sources.wake_up_src.wu_ia) {
            ESP_LOGI(TAG, "Wake-up event detected");
            *wakeup_occurred = true;
            return ESP_OK;
        }
    }

    *wakeup_occurred = false;
    return ESP_OK;
}

/**
 * @brief 设置wakeup阈值
 */
esp_err_t sensor_set_wakeup_threshold(sensor_dev_t *handle, uint8_t threshold)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->config.wakeup_threshold = threshold;
    lsm6ds3tr_c_wkup_threshold_set(&handle->dev_ctx, threshold);
    
    ESP_LOGI(TAG, "Wakeup threshold set to: 0x%02X", threshold);
    return ESP_OK;
}

/**
 * @brief 设置wakeup持续时间
 */
esp_err_t sensor_set_wakeup_duration(sensor_dev_t *handle, uint8_t duration)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->config.wakeup_duration = duration;
    lsm6ds3tr_c_wkup_dur_set(&handle->dev_ctx, duration);
    
    ESP_LOGI(TAG, "Wakeup duration set to: 0x%02X", duration);
    return ESP_OK;
}

/**
 * @brief 获取设备ID
 */
esp_err_t sensor_get_device_id(sensor_dev_t *handle, uint8_t *device_id)
{
    if (handle == NULL || device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (lsm6ds3tr_c_device_id_get(&handle->dev_ctx, device_id) != 0) {
        ESP_LOGE(TAG, "Failed to read device ID");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief 软件复位传感器
 */
esp_err_t sensor_reset(sensor_dev_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (lsm6ds3tr_c_reset_set(&handle->dev_ctx, 1) != 0) {
        ESP_LOGE(TAG, "Failed to reset sensor");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief 读取内置 pedometer 步数计数器(16-bit)
 * @note  需在 sensor_init 中已启用 pedometer 。
 *        读寄存器 0x4B(L)/0x4C(H) 拼出总步数;复位可调 sensor_step_reset。
 */
esp_err_t sensor_get_step_count(sensor_dev_t *handle, uint16_t *steps)
{
    if (handle == NULL || !handle->initialized || steps == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2];
    if (lsm6ds3tr_c_read_reg(&handle->dev_ctx,
                             LSM6DS3TR_C_STEP_COUNTER_L, buf, 2) != 0) {
        return ESP_FAIL;
    }
    *steps = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return ESP_OK;
}

/**
 * @brief 复位内置步数计数器为 0
 */
esp_err_t sensor_step_reset(sensor_dev_t *handle)
{
    if (handle == NULL || !handle->initialized) return ESP_ERR_INVALID_ARG;
    if (lsm6ds3tr_c_pedo_step_reset_set(&handle->dev_ctx, 1) != 0) return ESP_FAIL;
    /* CTRL10_C.pedo_rst_step 为自清位,无需手动写回 0 */
    return ESP_OK;
}