add_library(spfy_common STATIC
    obfuscation.c
    riff.c
    riff_write.c
    log.c
    file_io.c
    env.c
    voice_find.c
    ssml.c
)
# ssml.c converts pitch between percent and semitones (log/pow), so the math
# library has to come along on the toolchains that keep it separate from libc.
if(NOT WIN32)
    find_library(SPFY_LIBM m)
    if(SPFY_LIBM)
        target_link_libraries(spfy_common PUBLIC ${SPFY_LIBM})
    endif()
endif()
# CMAKE_CURRENT_SOURCE_DIR is PUBLIC, not PRIVATE: env.h is consumed by the
# usel / wsola / prosody / cli targets now, and log.h + le.h were already
# being reached by relative "../common/..." paths from those files.
target_include_directories(spfy_common
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
)
