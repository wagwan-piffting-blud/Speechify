add_library(spfy_text_norm STATIC
    text_norm.c
)
target_include_directories(spfy_text_norm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
