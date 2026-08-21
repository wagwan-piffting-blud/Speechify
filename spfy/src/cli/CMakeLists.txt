add_executable(spfy_dump_voice spfy_dump_voice.c)
target_include_directories(spfy_dump_voice
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/fe      # phoneset.h, for --phones
)
# spfy_fe supplies phoneset.c under BOTH configs (src/fe_host/ pulls in
# src/fe/phoneset.c; src/fe/ compiles it directly).
target_link_libraries(spfy_dump_voice
    PRIVATE spfy_voice spfy_usel spfy_fe spfy_common)

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
# fe_symbol_table.json + fe_tables_a + fe_tables blobs straight into
# spfy_synth.exe. That lets the CLI accept a 5-arg form
# (vin/vdb/vcf/text/out) instead of the legacy 9-arg form — the
# extractor writes the bundled assets to a tempdir on startup.
#
# The hp_class table is NOT embedded: it is per-voice, so baking Tom's
# copy in made the 5-arg form reject every other voice on the unit-count
# check. It is derived from the VIN at load time instead. The on-disk
# data/tom_hpclass.bin is kept as the reference that derivation is
# checked against (spfy_dump_voice --hpclass), not as a build input.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EMBED_OUT_C "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.c")
set(EMBED_OUT_H "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.h")

# ⚠ The .bin files themselves MUST be dependencies. They were not, and the
# generated blob is what the whole FE runs on: with only embed_assets.py and
# fe_symbol_table.json listed, restoring 728 deleted table files left ninja
# convinced the blob was current and it happily linked an EMPTY one. The
# failure was loud here (zero-size array), but a partial change would have
# been silent and would have altered every render.
#
# CONFIGURE_DEPENDS re-globs when the directory contents change, so adding or
# removing a table is picked up without a manual re-configure.
file(GLOB SPFY_FE_TABLE_FILES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/data/fe_tables/*.bin"
     "${CMAKE_SOURCE_DIR}/data/fe_tables_a/*.bin")
list(LENGTH SPFY_FE_TABLE_FILES _spfy_n_fe_tables)
if(_spfy_n_fe_tables LESS 700)
    message(FATAL_ERROR
        "spfy: only ${_spfy_n_fe_tables} FE table .bin files found under "
        "data/fe_tables{,_a} — expected ~728. The embedded blob would be "
        "built from an incomplete set and every render would be wrong. "
        "Restore them (they are tracked in git) before building.")
endif()
add_custom_command(
    OUTPUT  "${EMBED_OUT_C}" "${EMBED_OUT_H}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
            --vocab    "${CMAKE_SOURCE_DIR}/build/fe_symbol_table.json"
            --tables-a "${CMAKE_SOURCE_DIR}/data/fe_tables_a"
            --tables-b "${CMAKE_SOURCE_DIR}/data/fe_tables"
            --out-c    "${EMBED_OUT_C}"
            --out-h    "${EMBED_OUT_H}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
            "${CMAKE_SOURCE_DIR}/build/fe_symbol_table.json"
            ${SPFY_FE_TABLE_FILES}
    COMMENT "Embedding fe_symbol_table + fe_tables into spfy_synth")

# Wrapped in a library rather than compiled into each consumer, because
# src/sapi needs it too: a CMake add_custom_command OUTPUT can only be
# consumed by targets in the SAME directory, so spfy_sapi cannot name
# ${EMBED_OUT_C} directly. PUBLIC include dir so consumers get the generated
# embedded_assets.h without repeating the path.
add_library(spfy_embedded_assets STATIC "${EMBED_OUT_C}")
target_include_directories(spfy_embedded_assets
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

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
            ${CMAKE_SOURCE_DIR}/src/dsp      # SPFY_SPFY4_F0_FLATTEN post-process
            ${CMAKE_CURRENT_BINARY_DIR}      # for generated embedded_assets.h
)
target_link_libraries(spfy_synth
    PRIVATE spfy_synth_lib spfy_fe spfy_fe_internal spfy_usel spfy_cart spfy_voice spfy_wsola spfy_dsp spfy_prosody spfy_common)

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
            ${CMAKE_SOURCE_DIR}/src/dsp
            ${CMAKE_CURRENT_BINARY_DIR}      # for generated embedded_assets.h
)
target_link_libraries(spfy_synth_trace
    PRIVATE spfy_synth_lib spfy_fe spfy_fe_internal spfy_usel spfy_cart spfy_voice spfy_wsola spfy_dsp spfy_prosody spfy_common)

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

# A/B tester for the TD-PSOLA F0 retarget (Speechify-4 contour flattening;
# see reveng/spfy4/15_psola_probe.md). Post-processes rendered PCM.
add_executable(spfy_f0_flatten spfy_f0_flatten.c)
target_include_directories(spfy_f0_flatten
    PRIVATE ${CMAKE_SOURCE_DIR}/src/dsp)
target_link_libraries(spfy_f0_flatten PRIVATE spfy_dsp)

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

# ⛔ spfy_fe_text2tagged is NO LONGER BUILT.
#
# It is a default FE with no voice loaded, and it disagreed with the engine on
# 48.7% of words -- `mph` came out as letters where spfy_synth says "miles per
# hour". Every label it produced described audio that does not exist. The
# source stays in the tree; nothing should link it.
#
# Use spfy_synth with SPFY_ETAGS_DUMP=1. vb_fesrc.tagged_bodies() drives it
# across all cores.

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
# ⛔ spfy_fe_host_dump is NO LONGER BUILT.
#
# It gets the WORD sequence right -- measured 0 of 40,556 words differ from
# spfy_synth across the corpus, `mph` included -- but NOT the phones: the
# synth applies a reduction pass it does not (ax->ix, ih->ix, `ax l`->el,
# t->dx). A unit labelled from host_dump carries a phone the audio does not
# have, and mixing the two put the phone stream and the captured sp slots on
# different indices once already (ctx cross-check 42.18% vs 99.70%).
#
# Since it was only ever the fast path to the right answer, and
# vb_fesrc.tagged_bodies() now runs the synth across all cores at ~14/s,
# there is nothing left for it to be faster at.

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

# Gate tests for the prosody stage (reveng/spfy4/PLAN_PROSODY_STAGE.md).
# The identity check here is what the whole stage rests on.
add_executable(spfy_prosody_test spfy_prosody_test.c)
target_link_libraries(spfy_prosody_test PRIVATE spfy_prosody spfy_common)

# Acceptance test for the reconstructed RIFF writer: re-emit a container
# through it and diff byte-for-byte. Everything the voice builder writes
# hangs off this contract being right.
add_executable(spfy_riff_roundtrip spfy_riff_roundtrip.c)
target_link_libraries(spfy_riff_roundtrip PRIVATE spfy_common)

# S4 acceptance gate: rebuild a voice's join-cost table from its own pair set
# and compare, semantically and byte-for-byte. See src/vb/SPEC_S4_hash.md.
# S4 calibration: our join cost against the vendor's cached distribution.
add_executable(spfy_vb_joincost spfy_vb_joincost.c)
target_include_directories(spfy_vb_joincost
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_joincost PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

# S4 cost identification: score the vendor's own cached pairs with a candidate
# spectral representation and correlate PER PAIR, with a floor control. This is
# what separates "the vendor's features" from "features with a similar spread";
# spfy_vb_joincost only ever compared sorted distributions.
add_executable(spfy_vb_jcfit spfy_vb_jcfit.c)
target_include_directories(spfy_vb_jcfit
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_jcfit PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

# Price the S4 cost in picks, not in correlation: rebuild `hash` with
# substituted costs over the vendor's own domain, so selection can be run
# against it. See SPEC_S4_hash.md, "What the cost is worth".
add_executable(spfy_vb_hashgen spfy_vb_hashgen.c)
target_include_directories(spfy_vb_hashgen
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_hashgen PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

# Separate "the table is narrow" from "the table is truncated": relation width
# per right unit against what the shipped table actually stores.
add_executable(spfy_vb_domaudit spfy_vb_domaudit.c)
target_include_directories(spfy_vb_domaudit
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_vb_domaudit PRIVATE spfy_usel spfy_voice spfy_common)

add_executable(spfy_hash_roundtrip spfy_hash_roundtrip.c)
target_include_directories(spfy_hash_roundtrip
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
# spfy_vb for spfy_hash_build: the packer moved out of spfy_usel so it could be
# threaded without putting -fopenmp on anything spfy_synth links.
target_link_libraries(spfy_hash_roundtrip PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

# The voice builder itself: corpus -> .vin/.vdb, all seven stages in order.
# See src/vb/README.md. Replaces vb_build1.py + vb_mean_apply.py +
# vb_trees_apply.py + vb_poolcap.py + vb_hash_cost/pack + vb_hash_apply.py.
add_executable(spfy_vb_build spfy_vb_build.c)
target_include_directories(spfy_vb_build
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_build PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

# ⛔ 2 GB is not enough to build a corpus this size. S4 JOIN returned
# SPFY_E_NOMEM (stderr `join -4`) at a 1,616 MB peak on a 1,293,926-unit
# inventory: a 32-bit image without this flag is capped at 2 GB of user
# address space, and a fragmented heap fails a large allocation well below the
# cap. This sets a PE header bit -- no code, no codegen, no ABI change -- and
# is scoped to the container TOOLS, so the engine binary and its byte-identity
# with vendor Speechify are untouched.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_options(spfy_vb_build PRIVATE -Wl,--large-address-aware)
endif()

add_executable(spfy_vb_ccos spfy_vb_ccos.c)
target_include_directories(spfy_vb_ccos
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_ccos PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

add_executable(spfy_vb_wagon spfy_vb_wagon.c)
target_include_directories(spfy_vb_wagon
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_wagon PRIVATE spfy_vb spfy_usel spfy_voice spfy_common)

add_executable(spfy_vb_verify spfy_vb_verify.c)
target_include_directories(spfy_vb_verify
    PRIVATE ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
            ${CMAKE_SOURCE_DIR}/src/cart
            ${CMAKE_SOURCE_DIR}/src/vb
)
target_link_libraries(spfy_vb_verify PRIVATE spfy_vb spfy_usel spfy_cart spfy_voice spfy_common)

# Same reason as spfy_vb_build above: verify loads the whole container, and a
# VIN carrying a hash chunk for 1.29 M units does not fit 2 GB either.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_options(spfy_vb_verify PRIVATE -Wl,--large-address-aware)
endif()
