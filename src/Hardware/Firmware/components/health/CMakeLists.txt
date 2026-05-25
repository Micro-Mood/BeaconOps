idf_component_register(
    SRCS "health.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common freertos json net battery config
)
