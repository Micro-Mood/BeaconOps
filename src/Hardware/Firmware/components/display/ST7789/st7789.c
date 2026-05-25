#include <stdio.h>
#include "st7789.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "ST7789";

esp_lcd_panel_io_handle_t lcd_io = NULL;
esp_lcd_panel_handle_t lcd_panel = NULL;
lv_display_t *lvgl_disp = NULL;

esp_err_t lcd_init(st7789_config_t lcd_config)
{
    esp_err_t ret = ESP_FAIL;

    ESP_LOGI(TAG, "Initialize LCD");
    /*!< backlight */
    if(lcd_config.backlight == GPIO_NUM_NC)
    {
        ESP_LOGW(TAG, "Backlight GPIO is not set, backlight will not be controlled");
    }
    else
    {
        gpio_config_t bk_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << lcd_config.backlight,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&bk_gpio_config), err, TAG, "Configure backlight GPIO failed");
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num = lcd_config.sclk,
        .mosi_io_num = lcd_config.mosi,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        // .max_transfer_sz = lcd_config.lcd_height_res * lcd_config.lcd_draw_buffer_height * sizeof(uint16_t),
    };

    ESP_GOTO_ON_ERROR(spi_bus_initialize(lcd_config.spi_host_device, &buscfg, SPI_DMA_CH_AUTO), err, TAG, "SPI init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = lcd_config.dc,
        .cs_gpio_num = lcd_config.cs,
        .pclk_hz = lcd_config.spi_freq,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 6,
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)lcd_config.spi_host_device, &io_config, &lcd_io), err, TAG, "New panel IO failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = lcd_config.rst,
        .color_space = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = lcd_config.lcd_bits_per_pixel,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG, "Panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG, "Panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, false, false), err, TAG, "Panel mirror failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(lcd_panel, lcd_config.lcd_x_offset , lcd_config.lcd_y_offset), err, TAG, "Panel set gap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), err, TAG, "Panel display on failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel,true), err, TAG, "Panel invert color failed");

    if (lcd_config.backlight != GPIO_NUM_NC) {
        ESP_GOTO_ON_ERROR(gpio_set_level(lcd_config.backlight, 1), err, TAG, "Set backlight level failed");
    }

    if (lcd_config.rst != GPIO_NUM_NC) {
        ESP_GOTO_ON_ERROR(gpio_set_level(lcd_config.rst, 1), err, TAG, "Set reset level failed");
    }

    ret = ESP_OK;
    return ret;

err:
    if (lcd_panel)
    {
        esp_lcd_panel_del(lcd_panel);
    }
    if (lcd_io)
    {
        esp_lcd_panel_io_del(lcd_io);
    }
    spi_bus_free(lcd_config.spi_host_device);
    return ret;
}

esp_err_t lvgl_init(st7789_config_t lvgl_config)
{
    esp_err_t ret = ESP_FAIL;

    ESP_LOGI(TAG, "Initialize LVGL");
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = lvgl_config.lvgl_task_priority,         /* LVGL task priority */
        .task_stack = lvgl_config.lvgl_task_size,                /* LVGL task stack size */
        .task_affinity = lvgl_config.lvgl_task_affinity,         /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,                                /* Maximum sleep in LVGL task */
        .timer_period_ms = lvgl_config.lvgl_timer_period_ms      /* LVGL timer tick period in ms */
    };
    ESP_GOTO_ON_ERROR(lvgl_port_init(&lvgl_cfg), err, TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = lvgl_config.lcd_height_res * lvgl_config.lcd_draw_buffer_height * sizeof(uint16_t),
        .double_buffer = 1,
        .hres = lvgl_config.lcd_height_res,
        .vres = lvgl_config.lcd_vertical_res,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_spiram = false,
            .buff_dma = true,
        }};

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if(lvgl_disp == NULL) {
        ESP_LOGE(TAG, "LVGL add display failed");
        ret = ESP_FAIL;
        goto err;
    } 

    ret = ESP_OK;
    return ret;
err:
    lvgl_port_deinit();
    return ret;
}