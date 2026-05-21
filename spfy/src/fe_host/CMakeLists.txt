# spfy/src/fe_host/CMakeLists.txt — hosted-DLL alternative to src/fe/
#
# Builds the spfy_fe target (drop-in name) backed by the in-process PE
# host (host/) plus the embedded SWIttsFe-en-US.dll bytes.
#
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "SPFY_FE_HOSTED=ON requires a 32-bit build (current ptr=${CMAKE_SIZEOF_VOID_P}B). "
        "Either configure with -m32 / a 32-bit toolchain, or pass -DSPFY_FE_HOSTED=OFF "
        "to fall back to the hand-written src/fe/.")
endif()

# Locate the DLL — looked up relative to the spfy/ project root.
set(SWITTSFE_DLL
    "${CMAKE_SOURCE_DIR}/../bin/SWIttsFe-en-US.dll"
    CACHE FILEPATH "Path to SWIttsFe-en-US.dll to embed at build time")
if(NOT EXISTS "${SWITTSFE_DLL}")
    message(FATAL_ERROR
        "SWIttsFe-en-US.dll not found at ${SWITTSFE_DLL} — set -DSWITTSFE_DLL=<path>")
endif()

# Bring in the loader library from sibling src/host/. host/CMakeLists.txt
# also guards on 32-bit-ness so this stays consistent.
add_subdirectory("${CMAKE_SOURCE_DIR}/src/host" "${CMAKE_BINARY_DIR}/host_build")

# Python is required to run the DLL-embed step. Must be located BEFORE
# the add_custom_command below since ${Python3_EXECUTABLE} is expanded
# at command-generation time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Embed the DLL bytes via a Python helper. The generated .c lives in
# the build directory so the source tree stays clean.
set(EMBED_OUT "${CMAKE_CURRENT_BINARY_DIR}/swittsfe_data.c")
add_custom_command(
    OUTPUT  "${EMBED_OUT}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed_dll.py"
            "${SWITTSFE_DLL}" "${EMBED_OUT}" "swittsfe_dll"
    DEPENDS "${SWITTSFE_DLL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed_dll.py"
    COMMENT "Embedding SWIttsFe-en-US.dll bytes into ${EMBED_OUT}")

# Codegen the engine ARPAbet -> phone-id table from the empirical CSV.
# See spfy/tools/engine_phoneid_to_c.py for context. Outputs go to the
# build dir; fe_parse.c includes engine_phoneid.h via the build-dir
# include path below.
set(PHONEID_CSV "${CMAKE_SOURCE_DIR}/data/en_us_engine_phone_ids.csv")
set(PHONEID_C   "${CMAKE_CURRENT_BINARY_DIR}/engine_phoneid.c")
set(PHONEID_H   "${CMAKE_CURRENT_BINARY_DIR}/engine_phoneid.h")
add_custom_command(
    OUTPUT  "${PHONEID_C}" "${PHONEID_H}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/engine_phoneid_to_c.py"
            --csv  "${PHONEID_CSV}"
            --out-c "${PHONEID_C}"
            --out-h "${PHONEID_H}"
    DEPENDS "${PHONEID_CSV}"
            "${CMAKE_SOURCE_DIR}/tools/engine_phoneid_to_c.py"
    COMMENT "Codegen engine phone-id table from ${PHONEID_CSV}")

add_library(spfy_fe STATIC
    fe_host.c
    fe_parse.c
    fe_no_refine.c
    # Host loader compiled directly into spfy_fe (the test_synth.exe
    # standalone build does the same; using libspfy_host.a transitively
    # produced a non-functional getObject result, possibly due to a
    # static-archive symbol-ordering quirk with the embedded DLL data).
    ${CMAKE_SOURCE_DIR}/src/host/loader.c
    ${CMAKE_SOURCE_DIR}/src/host/imports.c
    # Hand-written FE infrastructure files. `fe.c` (the public-API
    # impl) is the ONE file we replace; the rest are loaders / data
    # containers / utilities that downstream CLIs still reference for
    # historical reasons. Pull them in here so spfy_synth, etc. link.
    ${CMAKE_SOURCE_DIR}/src/fe/phoneset.c
    ${CMAKE_SOURCE_DIR}/src/fe/vocab.c
    ${CMAKE_SOURCE_DIR}/src/fe/tables.c
    ${CMAKE_SOURCE_DIR}/src/fe/stream.c
    ${CMAKE_SOURCE_DIR}/src/fe/prosody.c
    ${CMAKE_SOURCE_DIR}/src/fe/baked_dict.c
    "${EMBED_OUT}"
    "${PHONEID_C}"
)
target_include_directories(spfy_fe
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/fe       # for fe.h (shared with src/fe/)
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}     # for generated engine_phoneid.h
            ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/host)
target_link_libraries(spfy_fe PUBLIC spfy_host spfy_voice spfy_common spfy_baked_pos)

# fe_host.c indexes the hosted DLL's vtable via function-pointer casts
# (vfn0 -> vfn1/vfn3 by slot number). Same Windows-ABI constraint as
# spfy/src/host/ — suppress the cast warnings on this target only.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(spfy_fe PRIVATE -Wno-pedantic -Wno-cast-function-type)
endif()
