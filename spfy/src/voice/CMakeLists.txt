add_library(spfy_voice STATIC
    voice.c
    vin_loader.c
    vdb_loader.c
    vcf_loader.c
    vcf_matrix.c
    unit_table.c
    ccos.c
    voice_runtime.c
    feat_table.c
    vdb_lookup.c
    chunk_table.c
    api.c
)
target_include_directories(spfy_voice
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/common
)
target_link_libraries(spfy_voice PUBLIC spfy_common)
