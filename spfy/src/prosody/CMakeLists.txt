add_library(spfy_prosody STATIC
    pmarks.c
    psola_unit.c
    contour.c
    reselect.c
)
# Relative to THIS directory, not CMAKE_SOURCE_DIR: the wasm build adds this
# subdirectory from spfy/wasm, where CMAKE_SOURCE_DIR is the wasm tree and
# these paths would not exist. contour.c and psola_unit.c include "env.h" from
# src/common, so a silently-missing include dir there is a build failure, not a
# harmless no-op.
target_include_directories(spfy_prosody
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
if(NOT MSVC)
    target_link_libraries(spfy_prosody PUBLIC m)
endif()
