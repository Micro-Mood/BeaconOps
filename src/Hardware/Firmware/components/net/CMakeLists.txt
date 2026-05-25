file(GLOB SOURCES ./*.c)

idf_component_register(
    SRCS         ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        config
        certs
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        lwip
        mqtt
        mbedtls
        esp-tls
        json
)
