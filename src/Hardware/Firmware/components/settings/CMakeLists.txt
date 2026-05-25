set(SRC "./settings.c")
set(INCLUDE_DIRS "./")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver nvs_flash config fs
                    )
