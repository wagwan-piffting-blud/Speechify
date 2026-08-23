add_library(spfy_common STATIC
    obfuscation.c
    riff.c
    riff_write.c
    log.c
    file_io.c
    env.c
    voice_find.c
)
# CMAKE_CURRENT_SOURCE_DIR is PUBLIC, not PRIVATE: env.h is consumed by the
# usel / wsola / prosody / cli targets now, and log.h + le.h were already
# being reached by relative "../common/..." paths from those files.
target_include_directories(spfy_common
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
)
