file(GLOB SOURCES ./*.c)

idf_component_register(
    SRCS         ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        config
        json
        esp_timer
        spi_flash
)
