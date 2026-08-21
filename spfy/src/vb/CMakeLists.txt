# Voice builder. See README.md for the seven-stage spine.
#
#   vb_io      containers, audio, TextGrid, FE, .seg, .sp
#   vb_chunk   the chunk encoders (feat, unit, prsl, ckls, cklx, mean, indx)
#   vb_corpus  S1
#   vb_stages  S3 norm, S6 durt leaves, S4 join
#   vb_wagon   CART growing, ported from Festival's wagon (durt / f0tr)
#   vb_lang_enus  GENERATED -- the en-US tables that let a build run with no
#                 donor voice. Regenerate with
#                 reveng/spfy4/tools/voicebuild/vb_genlang.py
#   join_cost / edge_frames   the S4 cost, recovered from SWIttsUSel.dll
add_library(spfy_vb STATIC
    join_cost.c
    edge_frames.c
    vb_io.c
    vb_chunk.c
    vb_corpus.c
    vb_stages.c
    vb_wagon.c
    vb_lang_enus.c
    vb_vcf.c
    vb_vcf_enus.c
    # ⭐ Lives HERE, not in spfy_usel, so the S4 packer can be threaded without
    # putting -fopenmp on anything spfy_synth links. Every caller is
    # builder-side; the engine never references spfy_hash_build.
    ../usel/hash_build.c
)
target_include_directories(spfy_vb
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
            ${CMAKE_SOURCE_DIR}/src/voice
            ${CMAKE_SOURCE_DIR}/src/usel
)
target_link_libraries(spfy_vb PUBLIC spfy_usel spfy_voice spfy_common)

# ⭐ S4's K-best pass is the builder's dominant stage once prsl backoff is on
# (2.4 BILLION candidate joins costed at --prsl-backoff 24, against 158 M with
# backoff off) and it is embarrassingly parallel over right units.
#
# OPTIONAL by design: the code is guarded with #ifdef _OPENMP and is correct
# serially, so a toolchain without OpenMP still builds and still produces the
# same bytes. Only the builder gets it -- the synthesis path stays untouched,
# because parity there is byte-exact and threads have no business near it.
find_package(OpenMP COMPONENTS C)
if(OpenMP_C_FOUND)
    # PUBLIC so the CLIs also get omp_get_wtime() for their stage clock.
    #
    # ⚠ On MinGW, OpenMP::OpenMP_C resolves to the IMPORT library
    # libgomp.dll.a by absolute path, which the top-level `-static` cannot
    # override -- the exe then needs libgomp-1.dll on PATH and dies with
    # 0xC0000139 (entrypoint not found) without it. Link the static archive
    # instead so the builder stays self-contained like every other binary here.
    # ⚠ Deliberately NOT OpenMP::OpenMP_C. That target names the IMPORT library
    # (libgomp.dll.a) by absolute path, which the top-level `-static` cannot
    # override, so the exe ends up needing libgomp-1.dll on PATH and dies with
    # 0xC0000139 without it. Passing the plain -fopenmp flag at link time lets
    # gcc's own spec add -lgomp, which `-static` then resolves to libgomp.a.
    target_compile_options(spfy_vb PUBLIC ${OpenMP_C_FLAGS})
    target_link_options(spfy_vb PUBLIC ${OpenMP_C_FLAGS})
    message(STATUS "spfy_vb: OpenMP for S4 via ${OpenMP_C_FLAGS}")
else()
    message(STATUS "spfy_vb: no OpenMP -- S4 join will run single-threaded")
endif()
find_library(SPFY_VB_M m)
if(SPFY_VB_M)
    target_link_libraries(spfy_vb PUBLIC ${SPFY_VB_M})
endif()
