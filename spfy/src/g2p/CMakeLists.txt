add_library(spfy_g2p STATIC
    g2p.c
    cmudict_data.c
)
target_include_directories(spfy_g2p
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
