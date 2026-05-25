set(SRC "./cw2017.c")
set(INCLUDE_DIRS "./")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver I2C
                    )
