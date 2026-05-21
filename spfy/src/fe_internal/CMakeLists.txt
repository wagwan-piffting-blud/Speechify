add_library(spfy_fe_internal STATIC
    fe_internal.c
    pos_context.c
)
target_include_directories(spfy_fe_internal
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(spfy_fe_internal PUBLIC spfy_g2p spfy_text_norm spfy_baked_pos)
target_include_directories(spfy_fe_internal PRIVATE ${CMAKE_SOURCE_DIR}/src/fe)
