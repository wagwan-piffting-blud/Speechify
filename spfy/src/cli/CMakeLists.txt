add_executable(spfy_dump_voice spfy_dump_voice.c)
target_include_directories(spfy_dump_voice
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_dump_voice PRIVATE spfy_voice spfy_usel spfy_common)

# M1 acceptance harness: replay captured CART walks against C trees.
add_executable(spfy_cart_replay spfy_cart_replay.c)
target_include_directories(spfy_cart_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/cart
)
target_link_libraries(spfy_cart_replay PRIVATE spfy_cart spfy_voice spfy_common)

# M3.2 integration sanity: score one candidate end-to-end.
add_executable(spfy_score_one spfy_score_one.c)
target_include_directories(spfy_score_one
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_score_one PRIVATE spfy_usel spfy_voice spfy_common)

# M3.3 acceptance harness: replay captured hash probes against C lookup.
add_executable(spfy_hash_replay spfy_hash_replay.c)
target_include_directories(spfy_hash_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_hash_replay PRIVATE spfy_usel spfy_voice spfy_common)

# M3.4 acceptance harness: replay captured wsola_buffer chosen-UID
# sequences through the C-side Viterbi DP and compare paths.
add_executable(spfy_viterbi_replay spfy_viterbi_replay.c)
target_include_directories(spfy_viterbi_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_viterbi_replay PRIVATE spfy_usel spfy_voice spfy_common)

# M3.4r Phase A: run our DAG Viterbi over the engine's captured slot
# graph (from viterbi_dp_hook.js) and compare the chosen-UID path to
# the engine's. Validates DP+join+cost stack on engine-truth topology.
add_executable(spfy_engine_graph_replay spfy_engine_graph_replay.c)
target_include_directories(spfy_engine_graph_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_engine_graph_replay PRIVATE spfy_usel spfy_voice spfy_common)

# M3.4r Phase B2: build the slot tree from a captured fe_tree trace and
# compare slot counts to the engine's viterbi_dp captures.
add_executable(spfy_build_graph_replay spfy_build_graph_replay.c)
target_include_directories(spfy_build_graph_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_build_graph_replay PRIVATE spfy_usel spfy_voice spfy_common)

# M0b first audible synthesis: concat oracle-chosen units to WAV.
add_executable(spfy_concat spfy_concat.c)
target_include_directories(spfy_concat
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/wsola
)
target_link_libraries(spfy_concat PRIVATE spfy_wsola spfy_voice spfy_common)

# M3.4r Phase B4.4 / M5 anchor scorer validation: replay captured engine
# traces through the C anchor_score and compare top-N to engine's kept cands.
add_executable(spfy_anchor_replay spfy_anchor_replay.c)
target_include_directories(spfy_anchor_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_anchor_replay PRIVATE spfy_usel spfy_voice spfy_common)

# Per-HP InnerScorer validation: reproduces engine's cand_totals.
add_executable(spfy_hp_score_test spfy_hp_score_test.c)
target_include_directories(spfy_hp_score_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_hp_score_test PRIVATE spfy_usel spfy_voice spfy_common)

# M5: full text -> WAV pipeline (FE -> USel -> WSOLA streaming).
# Under SPFY_FE_HOSTED the FE-stage chain switches to the public
# spfy_fe_synth_text + parsed_to_fe_utt path (see #ifdef SPFY_FE_HOSTED
# in spfy_synth.c). spfy_fe_synth.c and spfy_fe_test.c still call the
# hand-written FE's stage functions directly and remain excluded.
#
# Embedded assets: spfy/tools/embed_assets.py generates a C source +
# header at configure time that bakes the (otherwise external)
# hpclass + fe_symbol_table.json + fe_tables_a + fe_tables blobs
# straight into spfy_synth.exe. That lets the CLI accept a 5-arg form
# (vin/vdb/vcf/text/out) instead of the legacy 9-arg form — the
# extractor writes the bundled assets to a tempdir on startup.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EMBED_OUT_C "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.c")
set(EMBED_OUT_H "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.h")
add_custom_command(
    OUTPUT  "${EMBED_OUT_C}" "${EMBED_OUT_H}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
            --hpclass  "${CMAKE_SOURCE_DIR}/data/tom_hpclass.bin"
            --vocab    "${CMAKE_SOURCE_DIR}/build/fe_symbol_table.json"
            --tables-a "${CMAKE_SOURCE_DIR}/data/fe_tables_a"
            --tables-b "${CMAKE_SOURCE_DIR}/data/fe_tables"
            --out-c    "${EMBED_OUT_C}"
            --out-h    "${EMBED_OUT_H}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
            "${CMAKE_SOURCE_DIR}/data/tom_hpclass.bin"
            "${CMAKE_SOURCE_DIR}/build/fe_symbol_table.json"
    COMMENT "Embedding hpclass + fe_symbol_table + fe_tables into spfy_synth")

add_executable(spfy_synth spfy_synth.c "${EMBED_OUT_C}")
target_include_directories(spfy_synth
    PRIVATE ${CMAKE_SOURCE_DIR}/src/synth
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
            ${CMAKE_SOURCE_DIR}/src/fe
            ${CMAKE_SOURCE_DIR}/src/fe_host
            ${CMAKE_SOURCE_DIR}/src/fe_internal
            ${CMAKE_SOURCE_DIR}/src/cart
            ${CMAKE_CURRENT_BINARY_DIR}      # for generated embedded_assets.h
)
target_link_libraries(spfy_synth
    PRIVATE spfy_synth_lib spfy_fe spfy_fe_internal spfy_usel spfy_cart spfy_voice spfy_wsola spfy_common)

# Live-trace fork of spfy_synth: the SAME engine source compiled with
# -DSPFY_TRACE=1, so the spfy_trace_eventf() instrumentation in spfy_synth.c
# emits an NDJSON event stream (activated by --trace-stream) instead of
# compiling away. Single-sourcing with spfy_synth guarantees the traced and
# shipped engines can never diverge. Consumed by the viz live tracer's SSE
# relay (viz/app.py). Shares the embedded-assets blob so it accepts the same
# 5-arg form as spfy_synth.
add_executable(spfy_synth_trace spfy_synth.c "${EMBED_OUT_C}")
target_compile_definitions(spfy_synth_trace PRIVATE SPFY_TRACE=1)
target_include_directories(spfy_synth_trace
    PRIVATE ${CMAKE_SOURCE_DIR}/src/synth
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
            ${CMAKE_SOURCE_DIR}/src/fe
            ${CMAKE_SOURCE_DIR}/src/fe_host
            ${CMAKE_SOURCE_DIR}/src/fe_internal
            ${CMAKE_SOURCE_DIR}/src/cart
            ${CMAKE_CURRENT_BINARY_DIR}      # for generated embedded_assets.h
)
target_link_libraries(spfy_synth_trace
    PRIVATE spfy_synth_lib spfy_fe spfy_fe_internal spfy_usel spfy_cart spfy_voice spfy_wsola spfy_common)

# A/B test tool for the TD-PSOLA pitch shifter (used to tune quality
# before wiring into the SAPI shim's <prosody pitch=...> path).
add_executable(spfy_pitch_shift spfy_pitch_shift.c)
target_include_directories(spfy_pitch_shift
    PRIVATE ${CMAKE_SOURCE_DIR}/src/dsp)
target_link_libraries(spfy_pitch_shift PRIVATE spfy_dsp)

add_executable(spfy_time_stretch spfy_time_stretch.c)
target_include_directories(spfy_time_stretch
    PRIVATE ${CMAKE_SOURCE_DIR}/src/dsp)
target_link_libraries(spfy_time_stretch PRIVATE spfy_dsp)

# G2P module smoke test — CMU dict ARPAbet lookup. Foundation of the
# in-house FE that replaces SWIttsFe-en-US.dll on ARM targets.
add_executable(spfy_g2p_dump spfy_g2p_dump.c)
target_include_directories(spfy_g2p_dump
    PRIVATE ${CMAKE_SOURCE_DIR}/src/g2p)
target_link_libraries(spfy_g2p_dump PRIVATE spfy_g2p)

# Phase 3 text normalizer smoke test — pre-G2P tokenizer.
add_executable(spfy_text_norm_dump spfy_text_norm_dump.c)
target_include_directories(spfy_text_norm_dump
    PRIVATE ${CMAKE_SOURCE_DIR}/src/text_norm)
target_link_libraries(spfy_text_norm_dump PRIVATE spfy_text_norm)

# Phase 4 in-house FE assembler — text → tagged-output (DLL-compatible).
add_executable(spfy_fe_text2tagged spfy_fe_text2tagged.c)
target_include_directories(spfy_fe_text2tagged
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe_internal)
target_link_libraries(spfy_fe_text2tagged PRIVATE spfy_fe_internal)

# One-shot diagnostic: print Tom's unit corpus F0 byte distribution to
# size the selection-only pitch range before falling back to PSOLA.
add_executable(spfy_dump_f0 spfy_dump_f0.c)
target_include_directories(spfy_dump_f0 PRIVATE ${CMAKE_SOURCE_DIR}/src/voice)
target_link_libraries(spfy_dump_f0 PRIVATE spfy_voice spfy_common)
if(NOT WIN32)
    target_link_libraries(spfy_dump_f0 PRIVATE m)
endif()

# Phase C: end-to-end synth via FE traces (no FE port required). Per-cand
# scoring uses the validated spfy_hp_innerscorer (99% bit-exact); chosen
# UIDs run through the same pair-aware concat as spfy_concat to produce WAV.
add_executable(spfy_synth_replay spfy_synth_replay.c)
target_include_directories(spfy_synth_replay
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
)
target_link_libraries(spfy_synth_replay
    PRIVATE spfy_usel spfy_voice spfy_wsola spfy_common)

# Path B FE skeleton sanity test: load vocab + 726 tables, decode
# t000 (TLDs) and t001 (units of measure) to validate vocabulary
# wiring before any pipeline stage is implemented.
# Hosted-FE integration smoke test (built only when SPFY_FE_HOSTED=ON).
if(SPFY_FE_HOSTED)
    add_executable(spfy_fe_host_dump spfy_fe_host_dump.c)
    target_include_directories(spfy_fe_host_dump
        PRIVATE ${CMAKE_SOURCE_DIR}/src/fe
                ${CMAKE_SOURCE_DIR}/src/voice
    )
    target_link_libraries(spfy_fe_host_dump
        PRIVATE spfy_fe spfy_voice spfy_common)
endif()

if(NOT SPFY_FE_HOSTED)
# These two exercise the legacy hand-written FE stage functions directly, so
# they link spfy_fe_handwritten (the public `spfy_fe` target is now the
# in-house adapter — see src/fe/CMakeLists.txt).
add_executable(spfy_fe_test spfy_fe_test.c)
target_include_directories(spfy_fe_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe
)
target_link_libraries(spfy_fe_test PRIVATE spfy_fe_handwritten spfy_voice spfy_common)

# Path B M8: full text-or-SSML -> per-slot dump CLI. End-to-end exercise
# of the entire FE pipeline (M1..M7).
add_executable(spfy_fe_synth spfy_fe_synth.c)
target_include_directories(spfy_fe_synth
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe
)
target_link_libraries(spfy_fe_synth PRIVATE spfy_fe_handwritten spfy_voice spfy_common)
endif()
