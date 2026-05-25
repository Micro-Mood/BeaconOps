/**
 * @file i2c_bus_manager.h
 * @brief I2C总线管理器，支持多设备共享同一I2C总线
 * @version 1.0
 */

#ifndef I2C_BUS_MANAGER_H
#define I2C_BUS_MANAGER_H

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2C总线配置结构体
 */
typedef struct {
    i2c_port_t port;           ///< I2C端口号
    gpio_num_t sda_io_num;     ///< SDA引脚
    gpio_num_t scl_io_num;     ///< SCL引脚
    uint32_t clk_speed;        ///< 时钟频率(Hz)
    bool pullup_enable;        ///< 是否启用内部上拉
} i2c_bus_config_t;

/**
 * @brief I2C总线管理器句柄
 */
typedef struct {
    i2c_port_t port;           ///< I2C端口号
    SemaphoreHandle_t mutex;   ///< 互斥锁，确保总线访问的线程安全
    uint32_t device_count;     ///< 挂载的设备数量
    bool initialized;          ///< 初始化标志
} i2c_bus_dev_t;

/**
 * @brief 初始化I2C总线
 * 
 * @param i2c_bus_dev_t* 总线句柄，失败返回NULL
 * @param config 总线配置
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_init(i2c_bus_dev_t** handle, const i2c_bus_config_t *config);

/**
 * @brief 释放I2C总线资源
 * 
 * @param handle 总线句柄
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_deinit(i2c_bus_dev_t *handle);

/**
 * @brief 获取总线访问锁（线程安全）
 * 
 * @param handle 总线句柄
 * @param timeout_ticks 超时时间
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_take_mutex(i2c_bus_dev_t *handle, TickType_t timeout_ticks);

/**
 * @brief 释放总线访问锁
 * 
 * @param handle 总线句柄
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_give_mutex(i2c_bus_dev_t *handle);

/**
 * @brief 设备注册（用于设备计数和管理）
 * 
 * @param handle 总线句柄
 * @param device_addr 设备地址
 * @param device_name 设备名称
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_register_device(i2c_bus_dev_t *handle, uint8_t device_addr, const char *device_name);

/**
 * @brief 设备注销
 * 
 * @param handle 总线句柄
 * @param device_addr 设备地址
 * @return esp_err_t 执行结果
 */
esp_err_t i2c_bus_unregister_device(i2c_bus_dev_t *handle, uint8_t device_addr);

/**
 * @brief 扫描I2C总线上的设备
 * 
 * @param handle 总线句柄
 * @param found_devices 发现的设备地址数组
 * @param max_devices 最大设备数量
 * @return int 发现的设备数量
 */
int i2c_bus_scan(i2c_bus_dev_t *handle, uint8_t *found_devices, int max_devices);

#ifdef __cplusplus
}
#endif

#endif /* I2C_BUS_MANAGER_H */