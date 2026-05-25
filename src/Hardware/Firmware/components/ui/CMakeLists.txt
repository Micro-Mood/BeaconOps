set(SOURCES
    ui.c
)

set(GENERATED_FONT_SOURCE lv_font_misans_bold_16_gb2312.c)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${GENERATED_FONT_SOURCE}")
    list(APPEND SOURCES ${GENERATED_FONT_SOURCE})
else()
    message(FATAL_ERROR "Missing generated UI font source: ${CMAKE_CURRENT_LIST_DIR}/${GENERATED_FONT_SOURCE}")
endif()

idf_component_register(
    SRCS         ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES
        config
        lv_v9.3
        lv_port
)
