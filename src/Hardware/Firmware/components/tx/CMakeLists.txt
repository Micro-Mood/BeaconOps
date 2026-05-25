file(GLOB SOURCES ./*.c)

idf_component_register(
    SRCS         ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        config
        net
        nvs_flash
        esp_timer
)
