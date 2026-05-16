add_library(spfy_dsp STATIC
    pitch_shift.c
    time_stretch.c
)
target_include_directories(spfy_dsp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
if(NOT MSVC)
    target_link_libraries(spfy_dsp PUBLIC m)
endif()
