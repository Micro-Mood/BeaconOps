set(SRCS
    "PNGdec.cpp"
    "adler32.c"
    "crc32.c"
    "infback.c"
    "inffast.c"
    "inflate.c"
    "inftrees.c"
    "zutil.c"
)

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS ".")

# PNGdec checks for __LINUX__/__MACH__/__MCUXPRESSO to skip Arduino.h.
# ESP-IDF is none of those, so define __LINUX__ for this component only.
component_compile_options(-D__LINUX__ -Wno-unused-but-set-variable -Wno-unused-variable)
