add_library(spfy_synth_lib STATIC
    spfy_synth_lib.c
)
target_include_directories(spfy_synth_lib
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
            ${CMAKE_SOURCE_DIR}/src/fe
            ${CMAKE_SOURCE_DIR}/src/fe_host
            ${CMAKE_SOURCE_DIR}/src/cart
)
target_link_libraries(spfy_synth_lib
    PUBLIC spfy_voice spfy_usel spfy_cart spfy_wsola spfy_fe spfy_common)
