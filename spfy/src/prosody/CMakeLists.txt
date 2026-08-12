add_library(spfy_prosody STATIC
    pmarks.c
    psola_unit.c
    contour.c
    reselect.c
)
target_include_directories(spfy_prosody
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/common
)
if(NOT MSVC)
    target_link_libraries(spfy_prosody PUBLIC m)
endif()
