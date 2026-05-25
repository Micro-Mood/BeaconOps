set(SRC "./decode.cpp" "./_fileIO.c")
set(INCLUDE_DIRS "./" "./png" "./jpg")


idf_component_register(SRCS ${SRC}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver config lv_v9.3 ST7789 lv_port png jpg
                    )
