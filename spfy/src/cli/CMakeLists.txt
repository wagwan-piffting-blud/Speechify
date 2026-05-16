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
add_executable(spfy_synth spfy_synth.c)
target_include_directories(spfy_synth
    PRIVATE ${CMAKE_SOURCE_DIR}/src/synth
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/wsola
            ${CMAKE_SOURCE_DIR}/src/fe
            ${CMAKE_SOURCE_DIR}/src/fe_host
            ${CMAKE_SOURCE_DIR}/src/cart
)
target_link_libraries(spfy_synth
    PRIVATE spfy_synth_lib spfy_fe spfy_usel spfy_cart spfy_voice spfy_wsola spfy_common)

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
add_executable(spfy_fe_test spfy_fe_test.c)
target_include_directories(spfy_fe_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe
)
target_link_libraries(spfy_fe_test PRIVATE spfy_fe spfy_voice spfy_common)

# Path B M8: full text-or-SSML -> per-slot dump CLI. End-to-end exercise
# of the entire FE pipeline (M1..M7).
add_executable(spfy_fe_synth spfy_fe_synth.c)
target_include_directories(spfy_fe_synth
    PRIVATE ${CMAKE_SOURCE_DIR}/src/fe
)
target_link_libraries(spfy_fe_synth PRIVATE spfy_fe spfy_voice spfy_common)
endif()
