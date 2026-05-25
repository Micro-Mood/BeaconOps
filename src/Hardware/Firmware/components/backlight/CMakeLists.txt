file(GLOB SOURCES ./*.c)

idf_component_register(
    SRCS         ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        esp_pm
)
