# Non-hosted (SPFY_FE_HOSTED=OFF) FE wiring. Processed only when the
# top-level CMakeLists picks src/fe/ over src/fe_host/ (i.e. any build that
# can't load the 32-bit SWIttsFe-en-US.dll: x86_64 desktop, ARM, etc.).
#
# `spfy_baked_pos` is declared at the top-level spfy/CMakeLists.txt so
# src/fe_internal/ (in-house FE) can link it before src/fe/ is processed.
#
# Two libraries are produced:
#
#   spfy_fe              The drop-in public-FE-API target that spfy_synth /
#                        spfy_sapi / spfy_synth_lib link (same target name the
#                        hosted build's src/fe_host/ produces). Backed by the
#                        in-house FE via the fe_stub.c adapter — it implements
#                        the FULL public API (spfy_fe_synth_text / _synth_tagged
#                        / _get_parsed / _text_to_tagged / _open / _close /
#                        _set_voice_vcf / _phoneset). The hand-written fe.c
#                        below only ever implemented _synth_text, which is why
#                        the non-hosted spfy_synth.exe failed to link.
#
#   spfy_fe_handwritten  The legacy hand-written FE pipeline (fe.c + the
#                        stage_*.c chain). Retired as the synth backend but
#                        kept building for the spfy_fe_test / spfy_fe_synth
#                        diagnostics, which call its stage functions directly.

# ---------------------------------------------------------------------------
# engine ARPAbet -> phone-id table (needed by fe_parse.c). Same codegen the
# hosted src/fe_host/ runs; outputs land in this dir and are pulled in via the
# build-dir include path below.
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

# ---------------------------------------------------------------------------
# spfy_fe — in-house FE adapter (public API). Mirrors the wasm build's
# composition (wasm/CMakeLists.txt) and the hosted src/fe_host/ target name.
# ---------------------------------------------------------------------------
add_library(spfy_fe STATIC
    ${CMAKE_SOURCE_DIR}/wasm/stubs/fe_stub.c      # public API over the in-house FE
    ${CMAKE_SOURCE_DIR}/src/fe_host/fe_parse.c    # tagged-text parser + phone-id lookup
    ${CMAKE_SOURCE_DIR}/src/fe_host/fe_no_refine.c
    phoneset.c
    prosody.c
    "${PHONEID_C}"
)
target_include_directories(spfy_fe
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}            # fe.h / phoneset.h / prosody.h
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe_host        # fe_parse.h / fe_no_refine.h
            ${CMAKE_CURRENT_BINARY_DIR}            # generated engine_phoneid.h
            ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/fe_internal
)
target_link_libraries(spfy_fe
    PUBLIC  spfy_voice spfy_common spfy_baked_pos
    PRIVATE spfy_fe_internal)

# ---------------------------------------------------------------------------
# spfy_fe_handwritten — legacy hand-written FE (kept for the diagnostics CLIs).
# ---------------------------------------------------------------------------
add_library(spfy_fe_handwritten STATIC
    vocab.c
    tables.c
    stream.c
    prosody.c
    phoneset.c
    baked_dict.c
    stage_textnorm.c
    stage_morph.c
    stage_syl.c
    stage_lts.c
    stage_prosody.c
    stage_spr.c
    stage_espr.c
    fe.c
)
target_include_directories(spfy_fe_handwritten
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/voice
)
target_link_libraries(spfy_fe_handwritten PUBLIC spfy_voice spfy_common spfy_baked_pos)
