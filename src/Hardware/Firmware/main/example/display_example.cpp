#include <iostream>
#include "st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "sensor.h"
#include "lsm6ds3trc.h"
#include "cw2017.h"
#include "i2c_bus_manager.h"

const char *TAG = "MIOE_APP";

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_GPIO    GPIO_NUM_8
#define I2C_SCL_GPIO    GPIO_NUM_2
#define I2C_FREQ_HZ     100000

void battery_test(){
    // 初始化I2C总线管理器
    i2c_bus_config_t bus_config = {
        .port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_speed = I2C_FREQ_HZ,
        .pullup_enable = true
    };
    
    i2c_bus_handle_t *i2c_bus = i2c_bus_init(&bus_config);
    if (i2c_bus == NULL) {
        ESP_LOGE("EXAMPLE", "I2C总线初始化失败");
        vTaskDelete(NULL);
        return;
    }
    
    // 配置CW2017设备
    cw2017_config_t cw2017_config = {
        .i2c_bus = i2c_bus,
        .i2c_addr = CW2017_I2C_ADDR,
        .device_name = "电池监视器"
    };
    
    cw2017_dev_t cw2017;
    
    // 初始化CW2017
    esp_err_t ret = cw2017_init(&cw2017, &cw2017_config);
    if (ret != ESP_OK) {
        ESP_LOGE("EXAMPLE", "CW2017初始化失败");
        i2c_bus_deinit(i2c_bus);
        vTaskDelete(NULL);
        return;
    }

    cw2017_battery_info_t battery_info;
    ret = cw2017_read_battery_info(&cw2017, &battery_info);
    if (ret == ESP_OK) {
        printf("电池状态:\n");
        printf("  版本号: 0x%02X\n", battery_info.version);
        printf("  电压: %d mV\n", battery_info.voltage);
        printf("  电量: %d.%02d%%\n", battery_info.soc / 100, battery_info.soc % 100);
        printf("  温度: %d°C\n", battery_info.temperature);
        printf("  ID引脚电压: %d mV\n", battery_info.id_voltage);
        printf("---\n");
    } else {
        printf("读取电池信息失败: %s\n", esp_err_to_name(ret));
    }
}

void app(){
    ESP_LOGI(TAG, "MIOE Starting...");
    battery_test();

    // // 初始化LCD和LVGL
    // static const st7789_config_t lcd_config = {
    //     .lvgl_task_size = 1024*4,
    //     .lvgl_task_priority = configMAX_PRIORITIES - 1,
    //     .lvgl_task_affinity = -1, // No affinity
    //     .lvgl_timer_period_ms = 10, // 100Hz
    //     .lcd_height_res = 172,
    //     .lcd_vertical_res = 320,
    //     .lcd_x_offset = 34,
    //     .lcd_y_offset = 0,
    //     .lcd_draw_buffer_height = 10,
    //     .lcd_bits_per_pixel = 16,
    //     .spi_host_device = SPI2_HOST,
    //     .spi_freq = 10 * 1000 * 1000,
    //     .backlight = GPIO_NUM_9,
    //     .sclk = GPIO_NUM_6,
    //     .mosi = GPIO_NUM_7,
    //     .dc = GPIO_NUM_4,
    //     .cs = GPIO_NUM_5,
    //     .rst = GPIO_NUM_10,
    // };
    // lcd_init(lcd_config);               // 初始化LCD屏
    // lvgl_init(lcd_config);              // 初始化LVGL
    // lvgl_port_lock(0);                  // 锁定LVGL

    // lv_obj_t * scr = lv_obj_create(NULL); // 创建一个新的屏幕对象
    // lv_scr_load(scr);                   // 加载屏幕对象为当前屏幕

    // lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0); // 设置屏幕背景色为白色

    // lv_obj_t * label = lv_label_create(scr);
    // lv_label_set_text(label, "Hello, MIOE!");
    // lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // lv_obj_t * btn = lv_btn_create(scr);
    // lv_obj_t * label_btn = lv_label_create(btn);
    // lv_label_set_text(label_btn, "Click Me");
    // lv_obj_align(btn, LV_ALIGN_CENTER, 0, 50);
    // lv_obj_set_size(btn, 30, 20);

    // lvgl_port_unlock();                 // 解锁LVGL 

    while (1) {
        vTaskDelay(100 / portTICK_PERIOD_MS); // 1秒延迟
    }
}

extern "C" {void app_main(void){ app();} }
