add_library(spfy_cart STATIC
    cart_load.c
    cart_eval.c
)
target_include_directories(spfy_cart
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/common
)
target_link_libraries(spfy_cart PUBLIC spfy_voice)
