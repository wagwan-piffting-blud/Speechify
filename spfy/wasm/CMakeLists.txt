# Emscripten build for the in-browser spfy demo.
#
# Build:
#     emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#     emmake  cmake --build build -j
#
# Outputs in build/:
#     spfy_wasm.js       JS loader (Module.cwrap-compatible API surface)
#     spfy_wasm.wasm     WebAssembly binary
#     spfy_wasm.data     Preloaded virtual-FS image (~92 MB, voice + tables)

cmake_minimum_required(VERSION 3.22)
project("spfy_wasm" LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# No hosted FE (no PE-loader), no strict-FP (clang on wasm32 ignores the x87-specific flags anyway), no tests.
set(SPFY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPFY_FE_HOSTED   OFF CACHE BOOL "" FORCE)
set(SPFY_STRICT_FP   OFF CACHE BOOL "" FORCE)

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

# In-house pure-C FE (the actual text→tagged engine).
add_subdirectory(${SPFY_ROOT}/src/fe_internal ${CMAKE_BINARY_DIR}/_fe_internal)

# spfy_fe — the FE backend opaque-handle wrapper.
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

# Voice assets to ship inside the WASM .data file. The on-disk en-US/tom/
# directory contains files spfy_voice_load NEVER touches — most notably
# tom16.vdb (227 MB, 16-bit/16 kHz, but our μ-law decoder assumes 8/8
# and emits silence on it) plus two small XML metadata files. Naively
# preloading the whole dir gave a 317 MB .data sidecar. Stage only what
# the loader actually opens into a build-tree directory and preload
# that instead — drops .data to ~90 MB before HTTP-layer compression.
set(VOICE_SRC ${SPFY_ROOT}/../en-US/tom)
set(DATA_SRC  ${SPFY_ROOT}/data)
set(VOICE_STAGE ${CMAKE_BINARY_DIR}/voice_stage)
file(MAKE_DIRECTORY ${VOICE_STAGE})

set(_voice_staged_files)
foreach(_pair
        "${VOICE_SRC}/tom.vin|tom.vin"
        "${VOICE_SRC}/tom8.vdb|tom8.vdb"
        "${VOICE_SRC}/tom.vcf|tom.vcf"
        "${DATA_SRC}/tom_hpclass.bin|tom_hpclass.bin")
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _src)
    list(GET _parts 1 _name)
    set(_dst "${VOICE_STAGE}/${_name}")
    add_custom_command(
        OUTPUT  "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
        DEPENDS "${_src}"
        COMMENT "Staging voice asset: ${_name}"
        VERBATIM
    )
    list(APPEND _voice_staged_files "${_dst}")
endforeach()
add_custom_target(spfy_wasm_voice_stage DEPENDS ${_voice_staged_files})

# Emscripten linker flags. Notes:
#   MODULARIZE=1 + EXPORT_NAME=createSpfyModule produces an ES-module-
#     friendly factory we can `import()` from webpack.
#   ALLOW_MEMORY_GROWTH=1 lets the int16 PCM buffer reallocate freely
#     without an upper bound pre-declared in the wasm header.
#   INITIAL_MEMORY=128MB up-front so the first ~92 MB of preloaded FS
#     copy doesn't trip 'memory grow' a million times during startup.
#   EXPORTED_FUNCTIONS lists every entry point JS will cwrap.
#   EXPORTED_RUNTIME_METHODS exposes the helpers JS uses to call them
#     (cwrap, ccall, HEAP16, HEAPU8 for typed-array views).
#   --preload-file mounts our voice + table dirs read-only into the
#     virtual FS. The "src@dst" form puts host's `src` at `dst` inside
#     the FS image. The .data sidecar is loaded by the JS shim before
#     main() runs.

target_link_options(spfy_wasm PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=createSpfyModule"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s INITIAL_MEMORY=134217728"
    "SHELL:-s STACK_SIZE=4194304"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_spfy_wasm_init','_spfy_wasm_synth','_spfy_wasm_pcm_ptr','_spfy_wasm_pcm_len','_spfy_wasm_sample_rate','_spfy_wasm_reset','_malloc','_free']"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAP16','HEAPU8','stringToUTF8','UTF8ToString','lengthBytesUTF8']"
    "SHELL:-s ENVIRONMENT=web,worker"
    # Preload from the staged dir (single --preload-file → no flag-dedup
    # surprises). --use-preload-cache stores the unpacked FS in IndexedDB
    # keyed by file packager hash, so repeat visits skip the network
    # fetch entirely (first load ~30 MB compressed, subsequent loads ~0).
    "SHELL:--preload-file ${VOICE_STAGE}@/voice"
    "SHELL:--use-preload-cache"
)
# Ensure assets are staged before linking (which runs file_packager
# against VOICE_STAGE).
add_dependencies(spfy_wasm spfy_wasm_voice_stage)

# Emit into the wasm/dist/ folder so webpack can copy directly.
set_target_properties(spfy_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dist
    SUFFIX ".js"
)
