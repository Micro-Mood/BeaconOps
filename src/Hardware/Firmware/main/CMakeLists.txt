idf_component_register(
    SRCS         "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_lcd nvs_flash
        config I2C sensor battery audio fs settings
        ST7789 lv_port lv_v9.3
        net certs json msg tx backlight
        notify ui profile error
        identity event health
)
