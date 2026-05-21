# SAPI 5 voice DLL — wraps the spfy synth library into a Windows TTS voice.
# Built as a 32-bit COM in-proc server. Both the DLL itself and the synth
# core (spfy_synth.c minus main(), gated by SPFY_SYNTH_NO_MAIN) are linked
# in so SAPI's Speak() call goes straight into spfy_synth_to_sink.

add_library(spfy_sapi SHARED
    spfy_sapi.c
    ${CMAKE_SOURCE_DIR}/src/cli/spfy_synth.c
    spfy_sapi.def
)
target_compile_definitions(spfy_sapi
    PRIVATE SPFY_SYNTH_NO_MAIN=1
            COBJMACROS=1
            CINTERFACE=1
            UNICODE=1
            _UNICODE=1
            WIN32_LEAN_AND_MEAN=1
)
target_include_directories(spfy_sapi
    PRIVATE ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/synth
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
            ${CMAKE_SOURCE_DIR}/src/fe
            ${CMAKE_SOURCE_DIR}/src/fe_host
            ${CMAKE_SOURCE_DIR}/src/fe_internal
            ${CMAKE_SOURCE_DIR}/src/cart
)
target_link_libraries(spfy_sapi
    PRIVATE spfy_synth_lib spfy_fe spfy_fe_internal spfy_usel spfy_cart
            spfy_voice spfy_wsola spfy_dsp spfy_common
            ole32 oleaut32 advapi32 shell32 uuid)
target_include_directories(spfy_sapi
    PRIVATE ${CMAKE_SOURCE_DIR}/src/dsp)
# Strip stdcall '@N' decoration from exported symbols so SAPI/regsvr32
# resolves Dll* by their plain names.
set_target_properties(spfy_sapi PROPERTIES
    PREFIX ""
    OUTPUT_NAME "spfy_sapi"
    SUFFIX ".dll"
    LINK_FLAGS "-Wl,--kill-at"
)

# The 64-bit SAPI shim (spfy_sapi64.dll) is built separately by
# build_sapi64.bat using the mingw64 toolchain — CMake can't drive
# cross-bitness from a single config. The shim subprocess-spawns
# spfy_synth.exe (this 32-bit build) for actual synthesis.
