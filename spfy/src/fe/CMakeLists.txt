# `spfy_baked_pos` is declared at the top-level spfy/CMakeLists.txt so
# src/fe_internal/ (in-house FE) can link it before src/fe/ is processed.

add_library(spfy_fe STATIC
    vocab.c
    tables.c
    stream.c
    prosody.c
    phoneset.c
    baked_dict.c
    stage_textnorm.c
    stage_morph.c
    stage_syl.c
    stage_lts.c
    stage_prosody.c
    stage_spr.c
    stage_espr.c
    fe.c
)
target_include_directories(spfy_fe
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/voice
)
target_link_libraries(spfy_fe PUBLIC spfy_voice spfy_common spfy_baked_pos)
