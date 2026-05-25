set(SRC "./spiffs.c"  "./nvslib.c")
set(INCLUDE_DIRS "./")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver spiffs nvs_flash config
                    )
