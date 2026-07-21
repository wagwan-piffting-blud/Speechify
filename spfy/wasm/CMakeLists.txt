# Emscripten build for the in-browser spfy demo.
#
# Ships an emulator-backed FE by default: the unmodified
# SWIttsFe-en-US.dll (32-bit Windows PE) is embedded into the WASM
# module and run through spfy/src/host_emu/ — the same portable x86
# interpreter that audits 100% engine PATH UID on the desktop and
# ships in the Android APK. wasm32 executes it identically.
#
# Build:
#     emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#     emmake  cmake --build build -j
#
# Outputs in dist/:
#     spfy_wasm.js       JS loader (Module.cwrap-compatible API surface)
#     spfy_wasm.wasm     WebAssembly binary (code + the small SWIttsFe DLLs)
#     voices/            Lazy voice assets + manifest.json, fetched on
#                        demand at runtime (no preloaded .data sidecar)
#
# Fallback: -DSPFY_WASM_INHOUSE_FE=ON drops the emulator and links the
# stubbed in-house FE from stubs/fe_stub.c (91.2% audit).

cmake_minimum_required(VERSION 3.22)
project("spfy_wasm" LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# No strict-FP (clang on wasm32 ignores the x87-specific flags anyway), no tests.
set(SPFY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPFY_FE_HOSTED   OFF CACHE BOOL "" FORCE)
set(SPFY_STRICT_FP   OFF CACHE BOOL "" FORCE)

option(SPFY_WASM_INHOUSE_FE
       "Fall back to the in-house pure-C FE (stubs/fe_stub.c) instead of the emulator-backed FE"
       OFF)

# Many TUs call libm functions. emscripten links libm into libc by
# default, so no explicit -lm needed.

# spfy/ root is two levels up from spfy/wasm/.
set(SPFY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

include_directories(${SPFY_ROOT}/include)

# ---------------------------------------------------------------------
# Core libraries.
# Each subdir emits a STATIC lib; we re-add them by path so the build
# tree stays inside spfy/wasm/build/ instead of polluting spfy/.
# ---------------------------------------------------------------------

add_subdirectory(${SPFY_ROOT}/src/common      ${CMAKE_BINARY_DIR}/_common)
add_subdirectory(${SPFY_ROOT}/src/voice       ${CMAKE_BINARY_DIR}/_voice)
add_subdirectory(${SPFY_ROOT}/src/cart        ${CMAKE_BINARY_DIR}/_cart)
add_subdirectory(${SPFY_ROOT}/src/usel        ${CMAKE_BINARY_DIR}/_usel)
add_subdirectory(${SPFY_ROOT}/src/wsola       ${CMAKE_BINARY_DIR}/_wsola)
add_subdirectory(${SPFY_ROOT}/src/dsp         ${CMAKE_BINARY_DIR}/_dsp)
add_subdirectory(${SPFY_ROOT}/src/g2p         ${CMAKE_BINARY_DIR}/_g2p)
add_subdirectory(${SPFY_ROOT}/src/text_norm   ${CMAKE_BINARY_DIR}/_text_norm)

# Baked POS dictionary (~1.7 MB codegen, used by the in-house FE).
add_library(spfy_baked_pos STATIC ${SPFY_ROOT}/src/fe/baked_pos.c)
target_include_directories(spfy_baked_pos PUBLIC ${SPFY_ROOT}/src/fe)

# spfy_fe_types — minimal subset of src/fe/ (phoneset + prosody only).
add_library(spfy_fe_types STATIC
    ${SPFY_ROOT}/src/fe/phoneset.c
    ${SPFY_ROOT}/src/fe/prosody.c
)
target_include_directories(spfy_fe_types
    PUBLIC  ${SPFY_ROOT}/src/fe
    PRIVATE ${SPFY_ROOT}/src/voice
            ${SPFY_ROOT}/src/common
)
target_link_libraries(spfy_fe_types PUBLIC
    spfy_voice spfy_common spfy_baked_pos
)

# In-house pure-C FE — pulled in either way. When SPFY_WASM_INHOUSE_FE
# is on it drives synth directly; when the emulator path is on it's a
# no-op dependency of spfy_fe_types (baked_pos).
add_subdirectory(${SPFY_ROOT}/src/fe_internal ${CMAKE_BINARY_DIR}/_fe_internal)

if(SPFY_WASM_INHOUSE_FE)
    # -------- fallback path (in-house pure-C FE) --------
    add_library(spfy_fe STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/fe_stub.c
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/engine_phoneid.c
        ${SPFY_ROOT}/src/fe_host/fe_parse.c
        ${SPFY_ROOT}/src/fe_host/fe_no_refine.c
    )
    target_include_directories(spfy_fe
        PUBLIC  ${SPFY_ROOT}/src/fe
                ${SPFY_ROOT}/src/fe_host
                ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        PRIVATE ${SPFY_ROOT}/src/common
                ${SPFY_ROOT}/src/voice
                ${SPFY_ROOT}/src/fe_internal
    )
    target_link_libraries(spfy_fe
        PUBLIC  spfy_fe_types spfy_voice spfy_common
        PRIVATE spfy_fe_internal
    )
    message(STATUS "wasm FE: IN-HOUSE pure-C (91.2% audit)")

else()
    # -------- default: emulator-backed 100%-engine FE --------

    # host_emu portable x86 interpreter.
    add_subdirectory(${SPFY_ROOT}/src/host_emu ${CMAKE_BINARY_DIR}/_host_emu)

    # FE DLL registry. fe_host_emu.c resolves the SWIttsFe-<lang>.dll for a
    # voice's VCF language tag via spfy_fe_dll_for_lang() over the
    # spfy_fe_dlls[] table (src/fe_host/swittsfe_registry.c). Generate that
    # table here exactly like the native fe_host build does so the emulator
    # links and adding a language is a one-line list edit.
    #
    # Each embedded DLL is baked into the .wasm as a C byte array. These are
    # small (en-US 7.5 MB, fr-CA 1.5 MB, es-MX 0.9 MB) — ~10 MB total, a
    # one-time HTTP-cached download — so we bake ALL languages the shipped
    # voices use into a single module. The big per-voice payload is the
    # voice DATA (vin/vdb), which is lazy-loaded at runtime, NOT baked in.
    # en-US must stay first: it is the fallback when a voice's VCF names a
    # language we did not build in.
    set(SPFY_WASM_FE_LANGS "en-US;fr-CA;es-MX"
        CACHE STRING "SWIttsFe languages to embed into the WASM module (first is the fallback)")
    set(SWITTSFE_DLL_DIR "${SPFY_ROOT}/../bin"
        CACHE PATH "Directory holding the SWIttsFe-<lang>.dll images")

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # Embed each language's DLL bytes (one generated .c per language) and
    # build a registry .c mapping VCF language tag -> blob. All generated
    # into the build dir so the source tree stays clean.
    set(EMBED_OUTS "")
    set(_reg_externs "")
    set(_reg_rows "")
    # A literal ';' cannot be written inline: CMake treats it as a list
    # separator and `\;` survives into the generated C as a stray backslash.
    set(_SEMI ";")
    foreach(_lang IN LISTS SPFY_WASM_FE_LANGS)
        string(REPLACE "-" "_" _sym "${_lang}")
        string(TOLOWER "${_sym}" _sym)
        set(_dll "${SWITTSFE_DLL_DIR}/SWIttsFe-${_lang}.dll")
        if(NOT EXISTS "${_dll}")
            message(FATAL_ERROR
                "SWIttsFe-${_lang}.dll not found at ${_dll} — fix "
                "SPFY_WASM_FE_LANGS / SWITTSFE_DLL_DIR, or pass "
                "-DSPFY_WASM_INHOUSE_FE=ON to fall back to the in-house FE.")
        endif()
        set(_out "${CMAKE_BINARY_DIR}/swittsfe_${_sym}_data.c")
        add_custom_command(
            OUTPUT  "${_out}"
            COMMAND "${Python3_EXECUTABLE}"
                    "${SPFY_ROOT}/src/fe_host/embed_dll.py"
                    "${_dll}" "${_out}" "swittsfe_${_sym}_dll"
            DEPENDS "${_dll}" "${SPFY_ROOT}/src/fe_host/embed_dll.py"
            COMMENT "Embedding SWIttsFe-${_lang}.dll bytes for WASM module")
        list(APPEND EMBED_OUTS "${_out}")
        string(APPEND _reg_externs
            "extern const unsigned char swittsfe_${_sym}_dll_data[]${_SEMI}\n"
            "extern const size_t        swittsfe_${_sym}_dll_size${_SEMI}\n")
        string(APPEND _reg_rows
            "    { \"${_lang}\", swittsfe_${_sym}_dll_data, &swittsfe_${_sym}_dll_size },\n")
    endforeach()

    set(REGISTRY_OUT "${CMAKE_BINARY_DIR}/swittsfe_registry_table.c")
    file(WRITE "${REGISTRY_OUT}"
"/* Auto-generated by spfy/wasm/CMakeLists.txt from SPFY_WASM_FE_LANGS. DO NOT EDIT. */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include \"swittsfe_registry.h\"

${_reg_externs}
const spfy_fe_dll_entry_t spfy_fe_dlls[] = {
${_reg_rows}};
const size_t spfy_fe_n_dlls = sizeof(spfy_fe_dlls) / sizeof(spfy_fe_dlls[0]);
")

    # spfy_fe — emulator-backed. Same target name so downstream link is
    # unchanged.
    add_library(spfy_fe STATIC
        ${SPFY_ROOT}/src/fe_host/fe_host_emu.c
        ${SPFY_ROOT}/src/fe_host/fe_parse.c
        ${SPFY_ROOT}/src/fe_host/fe_no_refine.c
        ${SPFY_ROOT}/src/fe_host/swittsfe_registry.c
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/engine_phoneid.c
        ${EMBED_OUTS}
        "${REGISTRY_OUT}"
    )
    target_include_directories(spfy_fe
        PUBLIC  ${SPFY_ROOT}/src/fe
                ${SPFY_ROOT}/src/fe_host
                ${CMAKE_CURRENT_SOURCE_DIR}/stubs   # for engine_phoneid.h
        PRIVATE ${SPFY_ROOT}/src/common
                ${SPFY_ROOT}/src/voice
                ${SPFY_ROOT}/src/host_emu
    )
    target_link_libraries(spfy_fe
        PUBLIC  spfy_fe_types spfy_voice spfy_common
        PRIVATE spfy_host_emu
    )
    target_compile_definitions(spfy_fe PRIVATE SPFY_FE_EMU=1)
    message(STATUS "wasm FE: EMULATOR-backed (100% audit), embedding: ${SPFY_WASM_FE_LANGS}")
endif()

# spfy_synth_lib — declared inline rather than via add_subdirectory
# because the desktop src/synth/CMakeLists.txt builds only the
# voice-load half (spfy_synth_lib.c); the per-call pipeline
# (spfy_synth_to_sink) lives in src/cli/spfy_synth.c, gated by
# SPFY_SYNTH_NO_MAIN to drop its main().
add_library(spfy_synth_lib STATIC
    ${SPFY_ROOT}/src/synth/spfy_synth_lib.c
    ${SPFY_ROOT}/src/cli/spfy_synth.c
)
target_compile_definitions(spfy_synth_lib PRIVATE SPFY_SYNTH_NO_MAIN=1)
target_include_directories(spfy_synth_lib
    PUBLIC  ${SPFY_ROOT}/include
            ${SPFY_ROOT}/src/synth
    PRIVATE ${SPFY_ROOT}/src/voice
            ${SPFY_ROOT}/src/usel
            ${SPFY_ROOT}/src/wsola
            ${SPFY_ROOT}/src/dsp
            ${SPFY_ROOT}/src/fe
            ${SPFY_ROOT}/src/fe_host
            ${SPFY_ROOT}/src/fe_internal
            ${SPFY_ROOT}/src/cart
)
target_link_libraries(spfy_synth_lib
    PUBLIC spfy_voice spfy_usel spfy_cart spfy_wsola spfy_dsp
           spfy_fe spfy_fe_internal spfy_common
)
# spfy_synth.c prints a one-time "[spfy] FE backend: ..." banner keyed off
# SPFY_FE_EMU / SPFY_FE_HOSTED. The macro drives only that string — the
# actual spfy_fe_synth_text() call links to whatever FE is in spfy_fe — but
# without it here the emulator build mislabels itself "IN-HOUSE pure-C".
# Propagate the flag so the banner reports the truth (EMULATED DLL).
if(NOT SPFY_WASM_INHOUSE_FE)
    target_compile_definitions(spfy_synth_lib PRIVATE SPFY_FE_EMU=1)
endif()

# ---------------------------------------------------------------------
# WASM entry executable
# ---------------------------------------------------------------------

add_executable(spfy_wasm src/spfy_wasm.c)

target_include_directories(spfy_wasm PRIVATE
    ${SPFY_ROOT}/src
)

target_link_libraries(spfy_wasm PRIVATE
    spfy_synth_lib
    spfy_fe
    spfy_fe_types
    spfy_voice
    spfy_common
    spfy_cart
    spfy_usel
    spfy_wsola
    spfy_dsp
    spfy_g2p
    spfy_text_norm
    spfy_fe_internal
    spfy_baked_pos
)

# ---------------------------------------------------------------------
# Lazy-loaded voice assets.
#
# Nothing is baked into the module anymore (no --preload-file, no .data
# sidecar). stage_voices.py copies each shipped voice's runtime triplet
# (<prefix>.vin / <prefix>8.vdb / <prefix>.vcf) into
# dist/voices/<lang>/<voice>/, splitting any file >90 MiB into <100 MB
# parts (GitHub Pages' hard per-file limit), and writes
# dist/voices/manifest.json. At runtime the browser fetches ONLY the
# selected voice, stitches any split parts, and writes them into the
# emscripten FS before calling spfy_wasm_init — so the page starts with
# just the ~14 MB module and pulls a voice on demand.
#
# Voices whose source files are absent are skipped (CI checkouts don't
# carry the gitignored large voices, e.g. Paulina), so the manifest
# reflects exactly what shipped. Incremental: unchanged files/chunks are
# left alone on rebuild.
# ---------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VOICES_OUT ${CMAKE_CURRENT_SOURCE_DIR}/dist/voices)
add_custom_target(spfy_wasm_voice_stage
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_voices.py
            --root ${SPFY_ROOT}/..
            --out  ${VOICES_OUT}
    COMMENT "Staging lazy voice assets + manifest into dist/voices/"
    VERBATIM
)

# Emscripten linker flags. Notes:
#   MODULARIZE=1 + EXPORT_NAME=createSpfyModule produces an ES-module-
#     friendly factory we can `import()` from webpack.
#   ALLOW_MEMORY_GROWTH=1 lets the FS-resident voice data (vin+vdb, up to
#     ~300 MB for Paulina) and the int16 PCM buffer grow the heap freely.
#   INITIAL_MEMORY=128MB up-front trims early 'memory grow' churn while a
#     voice's files stream into MEMFS.
#   EXPORTED_FUNCTIONS lists every entry point JS will cwrap.
#   EXPORTED_RUNTIME_METHODS exposes the helpers JS uses. FS is included
#     so index.js can mkdir/writeFile the fetched voice bytes into the
#     virtual filesystem before init.
target_link_options(spfy_wasm PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=createSpfyModule"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s INITIAL_MEMORY=134217728"
    "SHELL:-s STACK_SIZE=4194304"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_spfy_wasm_init','_spfy_wasm_free_voice','_spfy_wasm_synth','_spfy_wasm_pcm_ptr','_spfy_wasm_pcm_len','_spfy_wasm_sample_rate','_spfy_wasm_reset','_malloc','_free']"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['cwrap','ccall','FS','HEAP16','HEAPU8','stringToUTF8','UTF8ToString','lengthBytesUTF8']"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s FORCE_FILESYSTEM=1"
)
# Stage voices (+ manifest) as part of the build.
add_dependencies(spfy_wasm spfy_wasm_voice_stage)

# Emit into the wasm/dist/ folder so webpack can copy directly.
set_target_properties(spfy_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dist
    SUFFIX ".js"
)
