/**
 * @file cw2017.c
 * @brief CW2017电池管理芯片驱动实现
 */

#include "cw2017.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *TAG = "CW2017";

// 针对4.2V锂电池的默认配置文件
static const uint8_t default_battery_profile_4_2v[SIZE_BATINFO] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xB7,0xC2,0xB6,0xA9,0x9E,0x99,0xED,0xDF,
    0xD9,0xCB,0xC1,0xAD,0x9D,0x80,0x66,0x55,
    0x4B,0x4B,0x4B,0x85,0x7F,0xD3,0x73,0xFF,
    0xF9,0x5C,0x63,0x81,0xBD,0xEC,0xE0,0xD0,
    0xC7,0xD7,0xD6,0xD9,0xE5,0xDF,0xDA,0xD6,
    0xCD,0xCF,0xD3,0xD3,0xD5,0xF4,0xFF,0x43,
    0x00,0x00,0xAB,0x02,0x00,0x00,0x00,0x00,
    0x00,0x00,0x64,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,
};

// 私有函数声明
static esp_err_t cw2017_read_register(cw2017_dev_t *dev, uint8_t reg, uint8_t *data);
static esp_err_t cw2017_write_register(cw2017_dev_t *dev, uint8_t reg, uint8_t data);
static esp_err_t cw2017_read_registers(cw2017_dev_t *dev, uint8_t start_reg, uint8_t *data, size_t len);
static esp_err_t cw2017_acquire_bus(cw2017_dev_t *dev);
static esp_err_t cw2017_release_bus(cw2017_dev_t *dev);
static esp_err_t cw2017_wake_device(cw2017_dev_t *dev);
static esp_err_t cw2017_write_profile_data(cw2017_dev_t *dev, const uint8_t *profile, size_t len);

esp_err_t cw2017_init(cw2017_dev_t **dev, const cw2017_config_t *config) {
    esp_err_t ret;

    if (dev == NULL || config == NULL || config->i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cw2017_dev_t *d = calloc(1, sizeof(cw2017_dev_t));
    if (d == NULL) {
        return ESP_ERR_NO_MEM;
    }

    d->i2c_bus = config->i2c_bus;
    uint8_t addr = (config->i2c_addr != 0) ? config->i2c_addr : CW2017_I2C_ADDR;
    if (addr > 0x7F) {
        addr = addr >> 1;
    }
    d->i2c_addr = addr & 0x7F;
    d->device_name = (config->device_name != NULL) ? config->device_name : "CW2017";
    d->initialized = true;
    d->last_soc = 0;

    ret = i2c_bus_register_device(d->i2c_bus, d->i2c_addr, d->device_name);
    if (ret != ESP_OK) {
        free(d);
        return ret;
    }

    // 强制复位设备
    cw2017_write_register(d, CW2017_REG_CONFIG, 0xF0);
    vTaskDelay(pdMS_TO_TICKS(100));

    if(config->write_profile) {
        // 写入电池配置文件：
        // - 如果用户提供了 profile，则写入用户配置
        // - 否则自动写入内置的 4.2V 默认配置（现在 CW2017_REG_BATINFO 已设为安全地址）
        if (config->battery_profile != NULL && config->profile_size > 0) {
            ESP_LOGI(TAG, "User provided battery profile, writing to device (size=%d)", config->profile_size);
            ret = cw2017_write_battery_profile(d, config->battery_profile, config->profile_size);
        } else {
            ESP_LOGI(TAG, "No battery_profile provided: writing built-in 4.2V default profile (size=%d)", SIZE_BATINFO);
            ret = cw2017_write_battery_profile(d, default_battery_profile_4_2v, SIZE_BATINFO);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write battery profile");
            i2c_bus_unregister_device(d->i2c_bus, d->i2c_addr);
            free(d);
            return ret;
        }
        // 等待设备完成初始化
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 再次唤醒/重启，确保 ADC 启动（有些固件在写 profile 后需要显式重启）
    if (cw2017_wake_device(d) == ESP_OK) {
        ESP_LOGI(TAG, "Post-profile wake OK, waiting 200ms for ADC updates");
        vTaskDelay(pdMS_TO_TICKS(200));
    } else {
        ESP_LOGW(TAG, "Post-profile wake failed");
    }
    
    uint8_t version;
    ret = cw2017_read_version(d, &version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read version");
        i2c_bus_unregister_device(d->i2c_bus, d->i2c_addr);
        free(d);
        return ret;
    }

    *dev = d;
    ESP_LOGI(TAG, "CW2017 initialized successfully, version: 0x%02X", version);
    return ESP_OK;
}

esp_err_t cw2017_deinit(cw2017_dev_t **dev) {
    if (dev == NULL || *dev == NULL || !(*dev)->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_bus_unregister_device((*dev)->i2c_bus, (*dev)->i2c_addr);
    if (ret != ESP_OK) {
        return ret;
    }

    (*dev)->initialized = false;
    free(*dev);
    *dev = NULL;

    ESP_LOGI(TAG, "CW2017 deinitialized");
    return ESP_OK;
}

esp_err_t cw2017_read_battery_info(cw2017_dev_t *dev, cw2017_battery_info_t *info) {
    if (dev == NULL || !dev->initialized || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // 读取版本号
    ret = cw2017_read_version(dev, &info->version);
    if (ret != ESP_OK) {
        return ret;
    }

    // 读取电压值（14位ADC）
    uint8_t vcell_data[2];
    ret = cw2017_read_registers(dev, CW2017_REG_VCELL_H, vcell_data, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取 VCELL 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    // VCELL 为 UNSIGNED 14bit，寄存器 0x02 的高6位为 MSB
    uint16_t vcell_raw = ((vcell_data[0] & 0x3F) << 8) | vcell_data[1];

    // 如果首次读取到 VCELL 为 0（且 SOC/TEMP 可能也为0），尝试唤醒并有限重试
    if (vcell_raw == 0) {
        const int max_attempts = 5;
        for (int attempt = 0; attempt < max_attempts && vcell_raw == 0; ++attempt) {
            ESP_LOGW(TAG, "VCELL==0，尝试唤醒并重试读取 (%d/%d)", attempt + 1, max_attempts);
            if (cw2017_wake_device(dev) != ESP_OK) {
                ESP_LOGW(TAG, "唤醒失败，停止重试");
                break;
            }
            // 等待更长时间以保证 ADC 多次更新
            vTaskDelay(pdMS_TO_TICKS(500));

            // 先按常规方式读取 H,L
            ret = cw2017_read_registers(dev, CW2017_REG_VCELL_H, vcell_data, 2);
            if (ret == ESP_OK) {
                vcell_raw = ((vcell_data[0] & 0x3F) << 8) | vcell_data[1];
            } else {
                ESP_LOGW(TAG, "VCELL retry(H,L) read failed: %s", esp_err_to_name(ret));
            }

            // 若仍为0，尝试先读取 L 再读 H（部分设备在指针/时序上有差异）
            if (vcell_raw == 0) {
                uint8_t low_only[1] = {0}, high_only[1] = {0};
                if (cw2017_read_register(dev, CW2017_REG_VCELL_L, low_only) == ESP_OK &&
                    cw2017_read_register(dev, CW2017_REG_VCELL_H, high_only) == ESP_OK) {
                    uint16_t raw_alt = ((high_only[0] & 0x3F) << 8) | low_only[0];
                    if (raw_alt != 0) {
                        vcell_raw = raw_alt;
                    }
                }
            }
        }
        if (vcell_raw == 0) {
            ESP_LOGW(TAG, "多次重试后 VCELL 仍为 0，可能为硬件/电源/引脚连接或芯片未采样");
        }
    }

    // CW2017 VCELL LSB = 312.5 µV. 计算 mV = round(vcell_raw * 312.5µV / 1000)
    // 使用整数运算： mV = (vcell_raw * 3125 + 5000) / 10000
    info->voltage = (uint16_t)((((uint32_t)vcell_raw * 3125U) + 5000U) / 10000U);

    // 读取电量值（16位）
    uint8_t soc_data[2];
    ret = cw2017_read_registers(dev, CW2017_REG_SOC_H, soc_data, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取 SOC 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 根据手册：SOC 16bit, HIGH 为整数百分比 (0~100)，LOW 为小数 (LSB = 1/256 %)
    uint16_t soc_high = soc_data[0];
    uint16_t soc_low  = soc_data[1];
    // 转换为 0.01% 单位： soc100 = high*100 + round(low * 100 / 256)
    uint16_t soc100 = (uint16_t)(soc_high * 100U + ((soc_low * 100U + 128U) / 256U));

    // 校验并回退异常值：
    // - 如果高字节大于 100（明显不合理），或等于 0xFF/0xFE（可能为错误/占位），则视为无效读取
    if (soc_high > 100 || soc_high == 0xFF || soc_high == 0xFE || (soc_high == 0x00 && soc_low == 0x00)) {
        if (dev->last_soc != 0) {
            info->soc = dev->last_soc;
        } else {
            // 未有历史值则限制到 100.00%
            info->soc = 10000U;
        }
    } else {
        // 合法值，保存并返回
        if (soc100 > 10000U) soc100 = 10000U; // 额外保护，最大到 100.00%
        info->soc = soc100;
        dev->last_soc = soc100;
    }

    // 读取温度
    uint8_t temp_reg;
    ret = cw2017_read_register(dev, CW2017_REG_TEMP, &temp_reg);
    if (ret != ESP_OK) {
        return ret;
    }
    // TEMP reg: value -> Temp(°C) = -40 + value/2  (LSB = 0.5°C)
    info->temperature = (int8_t)(-40 + (temp_reg / 2));

    // 读取ID引脚电压（14位ADC）
    uint8_t vid_data[2];
    ret = cw2017_read_registers(dev, CW2017_REG_VOLT_ID_H, vid_data, 2);
    if (ret != ESP_OK) {
        return ret;
    }
    uint16_t vid_raw = ((vid_data[0] & 0x3F) << 8) | vid_data[1];
    // VOLT_ID LSB = 312.5 µV（与 VCELL 相同），换算为 mV
    info->id_voltage = (uint16_t)((((uint32_t)vid_raw * 3125U) + 5000U) / 10000U); // mV

    return ESP_OK;
}

esp_err_t cw2017_read_voltage(cw2017_dev_t *dev, uint16_t *voltage) {
    if (dev == NULL || !dev->initialized || voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t vcell_data[2];
    esp_err_t ret = cw2017_read_registers(dev, CW2017_REG_VCELL_H, vcell_data, 2);
    if (ret != ESP_OK) return ret;

    // VCELL 为 UNSIGNED 14bit，寄存器 0x02 的高6位为 MSB（与 read_battery_info 保持一致）
    uint16_t vcell_raw = ((vcell_data[0] & 0x3F) << 8) | vcell_data[1];
    // CW2017 VCELL LSB = 312.5 µV. 使用整数运算换算为 mV（四舍五入）
    *voltage = (uint16_t)((((uint32_t)vcell_raw * 3125U) + 5000U) / 10000U);
    
    return ESP_OK;
}

esp_err_t cw2017_read_soc(cw2017_dev_t *dev, uint16_t *soc) {
    if (dev == NULL || !dev->initialized || soc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t soc_data[2];
    esp_err_t ret = cw2017_read_registers(dev, CW2017_REG_SOC_H, soc_data, 2);
    if (ret != ESP_OK) return ret;

    uint16_t soc_high = soc_data[0];
    uint16_t soc_low = soc_data[1];
    
    // 修正SOC计算：按照示例代码 soc_h + soc_low/256
    uint16_t soc_percent = soc_high + (soc_low / 256);
    *soc = soc_percent * 100; // 转换为0.01%单位

    // 处理异常SOC值
    if (soc_high > 100 || soc_high == 0xFF || soc_high == 0xFE || (soc_high == 0x00 && soc_low == 0x00)) {
        if (dev->last_soc != 0) {
            *soc = dev->last_soc;
            ESP_LOGW(TAG, "Using last SOC: %u.%02u%%", dev->last_soc / 100, dev->last_soc % 100);
        } else {
            *soc = 10000U; // 默认100%
            ESP_LOGW(TAG, "Using default SOC: 100%%");
        }
    } else {
        if (*soc > 10000U) *soc = 10000U;
        dev->last_soc = *soc;
    }

    return ESP_OK;
}

esp_err_t cw2017_read_temperature(cw2017_dev_t *dev, int8_t *temperature) {
    if (dev == NULL || !dev->initialized || temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t temp_reg;
    esp_err_t ret = cw2017_read_register(dev, CW2017_REG_TEMP, &temp_reg);
    if (ret != ESP_OK) return ret;

    *temperature = (int8_t)(-40 + (temp_reg / 2));
    ESP_LOGI(TAG, "Temperature raw: 0x%02X -> %dC", temp_reg, *temperature);
    return ESP_OK;
}

esp_err_t cw2017_config_interrupt(cw2017_dev_t *dev, cw2017_int_config_t *config) {
    if (dev == NULL || !dev->initialized || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    ret = cw2017_write_register(dev, CW2017_REG_SOC_ALERT, config->soc_alert_threshold);
    if (ret != ESP_OK) return ret;

    ret = cw2017_write_register(dev, CW2017_REG_TEMP_MAX, config->temp_max_threshold);
    if (ret != ESP_OK) return ret;

    ret = cw2017_write_register(dev, CW2017_REG_TEMP_MIN, config->temp_min_threshold);
    if (ret != ESP_OK) return ret;

    uint8_t int_conf = 0x40;
    if (config->enable_soc_alert) {
        int_conf |= 0x10;
    }
    if (config->enable_temp_alert) {
        int_conf |= 0x06;
    }

    ret = cw2017_write_register(dev, CW2017_REG_INT_CONF, int_conf);
    return ret;
}

esp_err_t cw2017_write_host_temperature(cw2017_dev_t *dev, int16_t temperature) {
    if (dev == NULL || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    ret = cw2017_write_register(dev, CW2017_REG_T_HOST_H, (temperature >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;

    ret = cw2017_write_register(dev, CW2017_REG_T_HOST_L, temperature & 0xFF);
    return ret;
}

esp_err_t cw2017_read_version(cw2017_dev_t *dev, uint8_t *version) {
    if (dev == NULL || !dev->initialized || version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return cw2017_read_register(dev, CW2017_REG_VERSION, version);
}

esp_err_t cw2017_reset(cw2017_dev_t *dev) {
    if (dev == NULL || !dev->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = cw2017_write_register(dev, CW2017_REG_CONFIG, 0xF0);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(10));
    return cw2017_wake_device(dev);
}

esp_err_t cw2017_write_battery_profile(cw2017_dev_t *dev, const uint8_t *profile, size_t len) {
    if (dev == NULL || !dev->initialized || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Writing battery profile, size: %d", len);
    
    // 写入电池配置信息
    esp_err_t ret = cw2017_write_profile_data(dev, profile, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write profile data");
        return ret;
    }

    // 设置睡眠模式
    ret = cw2017_write_register(dev, CW2017_REG_MODE_CONFIG, MODE_SLEEP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set sleep mode");
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));

    // 设置正常模式唤醒设备
    ret = cw2017_write_register(dev, CW2017_REG_MODE_CONFIG, MODE_NORMAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set normal mode");
        return ret;
    }
    
    ESP_LOGI(TAG, "Battery profile written successfully");
    return ESP_OK;
}

// 私有函数实现
static esp_err_t cw2017_acquire_bus(cw2017_dev_t *dev) {
    return i2c_bus_take_mutex(dev->i2c_bus, pdMS_TO_TICKS(1000));
}

static esp_err_t cw2017_release_bus(cw2017_dev_t *dev) {
    return i2c_bus_give_mutex(dev->i2c_bus);
}

static esp_err_t cw2017_read_register(cw2017_dev_t *dev, uint8_t reg, uint8_t *data) {
    return cw2017_read_registers(dev, reg, data, 1);
}

static esp_err_t cw2017_write_register(cw2017_dev_t *dev, uint8_t reg, uint8_t data) {
    esp_err_t ret;

    ret = cw2017_acquire_bus(dev);
    if (ret != ESP_OK) return ret;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(dev->i2c_bus->port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    cw2017_release_bus(dev);
    return ret;
}

static esp_err_t cw2017_read_registers(cw2017_dev_t *dev, uint8_t start_reg, uint8_t *data, size_t len) {
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret;

    // 获取I2C总线访问权
    ret = cw2017_acquire_bus(dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取I2C总线访问权失败");
        return ret;
    }

    // 写入寄存器地址
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, start_reg, true);

    // 重复起始条件用于读取
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev->i2c_addr << 1) | I2C_MASTER_READ, true);

    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(dev->i2c_bus->port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    // 释放I2C总线访问权
    cw2017_release_bus(dev);

    return ret;
}

static esp_err_t cw2017_wake_device(cw2017_dev_t *dev) {
    if (dev == NULL || !dev->initialized) return ESP_ERR_INVALID_ARG;
    
    esp_err_t ret = cw2017_write_register(dev, CW2017_REG_CONFIG, 0x30);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(5));
    
    ret = cw2017_write_register(dev, CW2017_REG_CONFIG, 0x00);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(15));
    return ESP_OK;
}

static esp_err_t cw2017_write_profile_data(cw2017_dev_t *dev, const uint8_t *profile, size_t len) {
    esp_err_t ret;
    
    // 安全检查：如果 BATINFO 起始地址定义为 0x00（VERSION 寄存器），拒绝写入以防覆盖关键寄存器
    if (CW2017_REG_BATINFO == CW2017_REG_VERSION) {
        ESP_LOGW(TAG, "CW2017_REG_BATINFO == 0x00 (VERSION register). Refuse to write profile to avoid overwriting runtime registers.");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Writing profile data to registers 0x%02X-0x%02X", CW2017_REG_BATINFO, CW2017_REG_BATINFO + len - 1);
    
    for (size_t i = 0; i < len; i++) {
        ret = cw2017_write_register(dev, CW2017_REG_BATINFO + i, profile[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write profile data at register 0x%02X", CW2017_REG_BATINFO + i);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    // 等待配置生效
    vTaskDelay(pdMS_TO_TICKS(10));
    
    return ESP_OK;
}