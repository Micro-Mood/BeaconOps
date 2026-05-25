/**
 * @file cw2017.h
 * @brief CW2017电池管理芯片驱动头文件
 */

#ifndef CW2017_H
#define CW2017_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_log.h"
#include "i2c_bus_manager.h"

#define CW2017_I2C_ADDR       0x63    ///< CW2017默认I2C地址(7位)
#define SIZE_BATINFO          80      ///< 电池配置文件大小

// 寄存器地址定义
#define CW2017_REG_VERSION    0x00    ///< 版本寄存器
#define CW2017_REG_BATINFO    0x10    ///< 电池配置信息起始寄存器
#define CW2017_REG_VCELL_H    0x02    ///< 电压高字节寄存器
#define CW2017_REG_VCELL_L    0x03    ///< 电压低字节寄存器
#define CW2017_REG_SOC_H      0x04    ///< 电量高字节寄存器
#define CW2017_REG_SOC_L      0x05    ///< 电量低字节寄存器
#define CW2017_REG_TEMP       0x06    ///< 温度寄存器
#define CW2017_REG_CONFIG     0x08    ///< 配置寄存器
#define CW2017_REG_MODE_CONFIG 0x0A   ///< 模式配置寄存器
#define CW2017_REG_INT_CONF   0x0A    ///< 中断配置寄存器
#define CW2017_REG_SOC_ALERT  0x0B    ///< 电量报警阈值寄存器
#define CW2017_REG_TEMP_MAX   0x0C    ///< 最高温度阈值寄存器
#define CW2017_REG_TEMP_MIN   0x0D    ///< 最低温度阈值寄存器
#define CW2017_REG_VOLT_ID_H  0x0E    ///< ID电压高字节寄存器
#define CW2017_REG_VOLT_ID_L  0x0F    ///< ID电压低字节寄存器
#define CW2017_REG_T_HOST_H   0xA0    ///< 主机温度高字节寄存器
#define CW2017_REG_T_HOST_L   0xA1    ///< 主机温度低字节寄存器

// 模式定义
#define MODE_SLEEP            0x00    ///< 睡眠模式
#define MODE_NORMAL           0x01    ///< 正常模式

/**
 * @brief 电池信息结构体
 */
typedef struct {
    uint8_t version;       ///< 芯片版本号
    uint16_t voltage;      ///< 电池电压 (mV)
    uint16_t soc;          ///< 电池电量 (0.01%单位，10000=100.00%)
    int8_t temperature;    ///< 电池温度 (°C)
    uint16_t id_voltage;   ///< ID引脚电压 (mV)
} cw2017_battery_info_t;

/**
 * @brief 中断配置结构体
 */
typedef struct {
    bool enable_soc_alert;     ///< 使能电量报警中断
    bool enable_temp_alert;    ///< 使能温度报警中断
    uint8_t soc_alert_threshold; ///< 电量报警阈值
    uint8_t temp_max_threshold;  ///< 最高温度阈值
    uint8_t temp_min_threshold;  ///< 最低温度阈值
} cw2017_int_config_t;

/**
 * @brief 设备配置结构体
 */
typedef struct {
    i2c_bus_dev_t *i2c_bus;         ///< I2C总线句柄
    uint8_t i2c_addr;               ///< I2C设备地址
    const char *device_name;        ///< 设备名称
    const uint8_t *battery_profile; ///< 电池配置文件数据指针
    size_t profile_size;            ///< 电池配置文件大小
    bool write_profile;             ///< 是否写入电池配置文件
} cw2017_config_t;

/**
 * @brief 设备结构体
 */
typedef struct {
    i2c_bus_dev_t *i2c_bus;    ///< I2C总线句柄
    uint8_t i2c_addr;          ///< I2C设备地址
    const char *device_name;   ///< 设备名称
    bool initialized;          ///< 初始化标志
    uint16_t last_soc;         ///< 上一次有效的电量值
} cw2017_dev_t;

/**
 * @brief 初始化CW2017设备
 * 
 * @param dev 设备句柄指针
 * @param config 设备配置参数
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_init(cw2017_dev_t **dev, const cw2017_config_t *config);

/**
 * @brief 反初始化CW2017设备
 * 
 * @param dev 设备句柄指针
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_deinit(cw2017_dev_t **dev);

/**
 * @brief 读取电池完整信息
 * 
 * @param dev 设备句柄
 * @param info 电池信息结构体指针
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_read_battery_info(cw2017_dev_t *dev, cw2017_battery_info_t *info);

/**
 * @brief 读取电池电压
 * 
 * @param dev 设备句柄
 * @param voltage 电压值指针 (mV)
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_read_voltage(cw2017_dev_t *dev, uint16_t *voltage);

/**
 * @brief 读取电池电量
 * 
 * @param dev 设备句柄
 * @param soc 电量值指针 (0.01%单位)
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_read_soc(cw2017_dev_t *dev, uint16_t *soc);

/**
 * @brief 读取电池温度
 * 
 * @param dev 设备句柄
 * @param temperature 温度值指针 (°C)
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_read_temperature(cw2017_dev_t *dev, int8_t *temperature);

/**
 * @brief 配置中断参数
 * 
 * @param dev 设备句柄
 * @param config 中断配置参数
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_config_interrupt(cw2017_dev_t *dev, cw2017_int_config_t *config);

/**
 * @brief 写入主机报告的温度值 (仅CW2017BAAD支持)
 * 
 * @param dev 设备句柄
 * @param temperature 温度值
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_write_host_temperature(cw2017_dev_t *dev, int16_t temperature);

/**
 * @brief 读取芯片版本号
 * 
 * @param dev 设备句柄
 * @param version 版本号指针
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_read_version(cw2017_dev_t *dev, uint8_t *version);

/**
 * @brief 复位CW2017芯片
 * 
 * @param dev 设备句柄
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_reset(cw2017_dev_t *dev);

/**
 * @brief 写入电池配置文件
 * 
 * @param dev 设备句柄
 * @param profile 电池配置文件数据指针
 * @param len 配置文件长度
 * @return esp_err_t 执行结果
 */
esp_err_t cw2017_write_battery_profile(cw2017_dev_t *dev, const uint8_t *profile, size_t len);

#ifdef __cplusplus
}
#endif

#endif // CW2017_H