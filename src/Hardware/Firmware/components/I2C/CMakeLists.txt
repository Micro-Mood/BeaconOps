set(SRC "./i2c_bus_manager.c")
set(INCLUDE_DIRS "./")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver config
                    )
