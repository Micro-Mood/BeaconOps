/**
 * @file sensor.h
 * @brief LSM6DS3TR-C传感器驱动库
 * @version 1.0
 */

#ifndef SENSOR_H
#define SENSOR_H

#include "driver/i2c.h"
#include "lsm6ds3trc.h"
#include "i2c_bus_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 6轴传感器数据结构
 */
typedef struct {
    float accel_x;          ///< 加速度X轴 (mg)
    float accel_y;          ///< 加速度Y轴 (mg)
    float accel_z;          ///< 加速度Z轴 (mg)
    float gyro_x;           ///< 陀螺仪X轴 (mdps)
    float gyro_y;           ///< 陀螺仪Y轴 (mdps)
    float gyro_z;           ///< 陀螺仪Z轴 (mdps)
    uint32_t timestamp;     ///< 时间戳
} sensor_data_t;

/**
 * @brief 传感器配置结构
 */
typedef struct {
    i2c_bus_dev_t *i2c_bus;         ///< I2C总线管理器句柄
    uint8_t i2c_address;            ///< I2C地址
    uint8_t wakeup_threshold;       ///< 唤醒阈值
    uint8_t wakeup_duration;        ///< 唤醒持续时间
    const char *device_name;        ///< 设备名称（用于总线管理器注册）
} sensor_config_t;

/**
 * @brief 传感器句柄
 */
typedef struct {
    stmdev_ctx_t dev_ctx;           ///< 设备上下文
    sensor_config_t config;         ///< 配置信息
    bool initialized;               ///< 初始化标志
} sensor_dev_t;

/**
 * @brief 初始化传感器（分配并返回句柄）
 * 
 * @param handle 传感器句柄指针的指针（输出，内部会分配）
 * @param config 传感器配置
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_init(sensor_dev_t **handle, const sensor_config_t *config);

/**
 * @brief 释放传感器资源
 * 
 * @param handle 传感器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_deinit(sensor_dev_t *handle);

/**
 * @brief 读取6轴传感器数据
 * 
 * @param handle 传感器句柄
 * @param data 数据存储指针
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_read_data(sensor_dev_t *handle, sensor_data_t *data);

/**
 * @brief 启用wakeup功能
 * 
 * @param handle 传感器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_enable_wakeup(sensor_dev_t *handle);

/**
 * @brief 禁用wakeup功能
 * 
 * @param handle 传感器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_disable_wakeup(sensor_dev_t *handle);

/**
 * @brief 清除中断锁存
 * @note  在检测到中断后调用此函数以清除中断状态,否则中断可会一直保持。
 * 
 * @param handle 传感器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_clear_interrupt(sensor_dev_t *handle);

/**
 * @brief 检查是否发生wakeup事件
 * 
 * @param handle 传感器句柄
 * @param wakeup_occurred 是否发生wakeup事件
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_check_wakeup(sensor_dev_t *handle, bool *wakeup_occurred);

/**
 * @brief 设置wakeup阈值
 * 
 * @param handle 传感器句柄
 * @param threshold 阈值
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_set_wakeup_threshold(sensor_dev_t *handle, uint8_t threshold);

/**
 * @brief 设置wakeup持续时间
 * 
 * @param handle 传感器句柄
 * @param duration 持续时间
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_set_wakeup_duration(sensor_dev_t *handle, uint8_t duration);

/**
 * @brief 获取设备ID
 * 
 * @param handle 传感器句柄
 * @param device_id 设备ID存储指针
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_get_device_id(sensor_dev_t *handle, uint8_t *device_id);

/**
 * @brief 软件复位传感器
 * 
 * @param handle 传感器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t sensor_reset(sensor_dev_t *handle);

/**
 * @brief 读取内置 pedometer 16-bit 总步数
 * @note  sensor_init 已启用 pedometer。步数为上电以来(或上次 reset 以来)
 *        累积值,越界会回卷到 0。需要“今日步数”请在上层取差。
 */
esp_err_t sensor_get_step_count(sensor_dev_t *handle, uint16_t *steps);

/** 复位内置步数计数器为 0 */
esp_err_t sensor_step_reset(sensor_dev_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_H */