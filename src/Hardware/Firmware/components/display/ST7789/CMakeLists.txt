file(GLOB SOURCES
    ./*.c
)

# 注册ESP-IDF组件
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ./
    REQUIRES 
        driver
        esp_lcd
        lv_v9.3
        lv_port
)