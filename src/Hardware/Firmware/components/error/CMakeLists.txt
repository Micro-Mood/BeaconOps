set(SRC "./error.c")
set(INCLUDE_DIRS "./")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES esp_timer fs
                    )
