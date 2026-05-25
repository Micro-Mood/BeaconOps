idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES
        config
        driver
        esp_pm
)