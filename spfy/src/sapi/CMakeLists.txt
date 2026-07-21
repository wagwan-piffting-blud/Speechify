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
# resolves Dll* by their plain names. --enable-stdcall-fixup silences the
# matching ld "resolving _DllCanUnloadNow by linking to _DllCanUnloadNow@0"
# warnings: the .def lists the COM entry points undecorated, so ld matches
# them to the stdcall-decorated symbols via fixup — which is exactly what
# we want here, so do it quietly (keeps the build warning-clean).
set_target_properties(spfy_sapi PROPERTIES
    PREFIX ""
    OUTPUT_NAME "spfy_sapi"
    SUFFIX ".dll"
    LINK_FLAGS "-Wl,--kill-at -Wl,--enable-stdcall-fixup"
)

# ------------------------------------------------------------------
# The 64-bit SAPI shim (spfy_sapi64.dll).
#
# CMake can't drive cross-bitness from a single config, so this target
# only materialises when the configured toolchain is itself 64-bit --
# i.e. under build.bat (msys2 mingw64). build32.bat configures a 32-bit
# compiler and skips it with the status message below.
#
# It does NOT link the synth core: it subprocess-spawns spfy_synth.exe
# (the 32-bit build) to synthesize, and only needs spfy_dsp for the
# pitch_shift / time_stretch post-processing it applies to the returned
# WAV. That is why it can build in a tree whose spfy_synth is 32-bit.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(spfy_sapi64 SHARED
        spfy_sapi64.c
        spfy_sapi64.def
    )
    target_compile_definitions(spfy_sapi64
        PRIVATE COBJMACROS=1
                CINTERFACE=1
                UNICODE=1
                _UNICODE=1
                WIN32_LEAN_AND_MEAN=1
    )
    target_include_directories(spfy_sapi64
        PRIVATE ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/src/dsp
    )
    target_link_libraries(spfy_sapi64
        PRIVATE spfy_dsp
                ole32 oleaut32 advapi32 shell32 uuid)
    # No --kill-at here: x64 has no stdcall '@N' decoration to strip.
    set_target_properties(spfy_sapi64 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "spfy_sapi64"
        SUFFIX ".dll"
    )
else()
    message(STATUS
        "sapi: skipping spfy_sapi64 (needs a 64-bit toolchain; use build.bat)")
endif()
