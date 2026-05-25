#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /*!< LVGL configuration */
    uint16_t            lvgl_task_size;              /*!< LVGL task stack size */
    uint8_t             lvgl_task_priority;          /*!< LVGL task priority */
    int8_t              lvgl_task_affinity;          /*!< LVGL task pinned to core (-1 is no affinity) */
    uint32_t            lvgl_timer_period_ms;        /*!< LVGL timer tick period in milliseconds */
    /*!< LCD configuration */
    uint16_t            lcd_height_res;             // LCD height
    uint16_t            lcd_vertical_res;           // LCD vertical
    uint16_t            lcd_x_offset;               // LCD x offset
    uint16_t            lcd_y_offset;               // LCD y offset
    uint16_t            lcd_draw_buffer_height;     // LCD draw buffer height
    uint16_t            lcd_bits_per_pixel;         // LCD bits per pixel
    spi_host_device_t   spi_host_device;            // SPI host device
    uint32_t            spi_freq;                   // SPI frequency in Hz
    gpio_num_t          backlight;                  // Backlight GPIO, set to GPIO_NUM_NC if not used
    gpio_num_t          sclk;
    gpio_num_t          mosi;
    gpio_num_t          dc;
    gpio_num_t          cs;
    gpio_num_t          rst;
} st7789_config_t;


extern esp_lcd_panel_io_handle_t lcd_io;
extern esp_lcd_panel_handle_t lcd_panel;
extern lv_display_t *lvgl_disp;

esp_err_t lcd_init(st7789_config_t lcd_config);

esp_err_t lvgl_init(st7789_config_t lvgl_config);

#ifdef __cplusplus
} /*extern "C"*/
#endif
