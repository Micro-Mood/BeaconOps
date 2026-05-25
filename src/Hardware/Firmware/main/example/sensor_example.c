#include "esp_log.h"
#include "sensor.h"
#include "i2c_bus_manager.h"

const char *TAG = "MAIN";

void IRAM_ATTR interrupt_task(void* arg) {
    ESP_EARLY_LOGI(TAG, "Interrupt detected on INT1 pin");
}

void app_main(void)
{
    esp_err_t ret = ESP_OK;
    
    // 配置INT1引脚为输入，默认高电平，低电平有效
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_10),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 默认高电平
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE    // 下降沿触发（低电平有效）
    };
    
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure INT1 GPIO");
    }

    // 添加中断处理
    gpio_install_isr_service(0);
    ret = gpio_isr_handler_add(GPIO_NUM_10, interrupt_task, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler");
    }

    // 1. 初始化I2C总线管理器
    i2c_bus_config_t bus_config = {
        .port = I2C_NUM_0,
        .sda_io_num = 8,
        .scl_io_num = 2,
        .clk_speed = 400000,
        .pullup_enable = true
    };
    
    i2c_bus_handle_t *i2c_bus = i2c_bus_init(&bus_config);
    if (i2c_bus == NULL) {
        ESP_LOGE("MAIN", "Failed to initialize I2C bus");
        return;
    }
    
    // 2. 配置传感器
    sensor_handle_t sensor;
    sensor_config_t sensor_config = {
        .i2c_bus = i2c_bus,
        .i2c_address = 0x6A,
        .wakeup_threshold = 0x08,
        .wakeup_duration = 0x50
    };
    
    // 3. 初始化传感器
    if (sensor_init(&sensor, &sensor_config) == ESP_OK) {
        // 4. 启用wakeup功能
        sensor_enable_wakeup(&sensor);
        
        while (1) {
            sensor_data_t data;
            bool wakeup_occurred;
            
            // // 读取6轴数据
            // if (sensor_read_data(&sensor, &data) == ESP_OK) {
            //     printf("Accel: X=%.2f, Y=%.2f, Z=%.2f mg\n", 
            //            data.accel_x, data.accel_y, data.accel_z);
            //     printf("Gyro: X=%.2f, Y=%.2f, Z=%.2f mdps\n", 
            //            data.gyro_x, data.gyro_y, data.gyro_z);
            // }
            
            // 检查wakeup事件
            if (sensor_check_wakeup(&sensor, &wakeup_occurred) == ESP_OK) {
                if (wakeup_occurred) {
                    printf("Wakeup event detected!\n");
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 5. 清理资源
        sensor_deinit(&sensor);
    }
    
    // 6. 释放I2C总线
    i2c_bus_deinit(i2c_bus);
}