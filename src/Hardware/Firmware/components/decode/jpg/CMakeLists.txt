set(SRCS "JPEGDEC.cpp")

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS ".")

component_compile_options(-Wno-unused-but-set-variable -Wno-unused-variable)
