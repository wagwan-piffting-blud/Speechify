add_library(spfy_usel STATIC
    cost_d.c
    cost_f0.c
    cost_sp.c
    cost_s.c
    prsl.c
    hash.c
    viterbi.c
    build_graph.c
    slot_ctx.c
    anchor_score.c
)
target_include_directories(spfy_usel
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/cart
            ${CMAKE_SOURCE_DIR}/src/common
)
target_link_libraries(spfy_usel PUBLIC spfy_voice spfy_cart)
