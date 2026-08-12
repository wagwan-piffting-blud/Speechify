add_library(spfy_wsola STATIC
    ulaw.c
    wav.c
    wsola.c
)
target_include_directories(spfy_wsola
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/dsp
)
# spfy_dsp for the optional whole-utterance time-stretch applied in
# spfy_wav_close() (see spfy_wav_set_stretch).
target_link_libraries(spfy_wsola PUBLIC spfy_voice spfy_usel spfy_dsp)
