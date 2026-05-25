idf_component_register(
    SRCS "event.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common freertos json net config
)
