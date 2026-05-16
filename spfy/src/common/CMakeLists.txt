add_library(spfy_common STATIC
    obfuscation.c
    riff.c
    log.c
    file_io.c
)
target_include_directories(spfy_common
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
