idf_component_register(
    SRCS "identity.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls esp_hw_support config
)
