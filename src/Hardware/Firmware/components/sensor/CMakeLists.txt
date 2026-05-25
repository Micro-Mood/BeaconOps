set(SRC "./src/lsm6ds3trc.c" "./sensor.c" "./sensor_task.c")
set(INCLUDE_DIRS "./" "./src")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver esp_timer I2C config
                    )
