add_library(spfy_usel STATIC
    cost_d.c
    cost_f0.c
    cost_sp.c
    cost_s.c
    prsl.c
    hash.c
    # ⛔ hash_build.c IS NOT HERE. It builds the S4 join table and every caller
    # is builder-side (vb_stages.c, spfy_vb_hashgen, spfy_hash_roundtrip); the
    # engine never references it. It lives in spfy_vb so it can be compiled
    # with OpenMP -- the packer was 546.6 s of a 579 s build, single-threaded
    # -- WITHOUT putting -fopenmp on a translation unit spfy_synth links, which
    # would put byte-identity at risk for a stage the engine does not run.
    viterbi.c
    build_graph.c
    slot_ctx.c
    syl_span.c
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
