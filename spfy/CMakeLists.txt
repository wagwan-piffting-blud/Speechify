cmake_minimum_required(VERSION 3.20)
project(spfy LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPFY_TRACE        "Emit JSONL stage traces from synth core" OFF)
option(SPFY_BUILD_TESTS  "Build unit tests"                         ON)
option(SPFY_STRICT_FP    "Enforce x87 long-double FP discipline"    ON)
# OFF by default — measured, not assumed. -ffloat-store forces every float
# expression through memory; with LTO on, removing it is -13.2% on a bulletin
# (15/15 paired rounds) with parity still BYTE-IDENTICAL 235/235, and the
# REMAINING strict-FP flags then add nothing measurable (+1.1%, 6/15 = noise).
# So the determinism that matters — -fexcess-precision=standard, -mno-fma,
# -fno-fast-math — is free, and this one flag was the whole cost.
#
# Turning it ON is still the more conservative choice across UNKNOWN compilers,
# which is why it remains a switch rather than being deleted.
option(SPFY_FLOAT_STORE  "Add -ffloat-store under SPFY_STRICT_FP (costly)" OFF)
# LTO. The engine is ~10 static libs with hot calls crossing them
# (anchor_score -> cart -> voice), so there is real cross-TU inlining to win.
# Guarded by CMake's own support probe, so a toolchain that cannot do it just
# builds without. Costs ~2.5x build time; set OFF if a CI leg ever chokes.
option(SPFY_LTO          "Link-time optimisation where supported"    ON)
option(SPFY_FE_HOSTED    "Use in-process SWIttsFe-en-US.dll host instead of hand-written src/fe/"
                                                                    ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Strict FP — pin x87 80-bit semantics so results stay bit-identical to the
# Windows/i386 reference.
#
# Only meaningful on x86: AArch64 (Apple Silicon, arm64 Linux) has no x87,
# so the block is skipped there and low-order audio bits may differ from the
# i386 reference. The unit-selection path is integer decision-making and is
# NOT affected, so the UID audit stays comparable across arches.
#
# Each flag is feature-tested rather than assumed. These are GCC spellings;
# Clang/AppleClang reject some outright (notably `-mfpmath=387`, a hard
# error) and merely warn-and-ignore others (`-ffloat-store`), which would
# either break the build or spam warnings on every TU. -Werror during the
# probe turns "unsupported, ignoring" warnings into probe failures so we
# only add flags that genuinely apply.
if(SPFY_STRICT_FP AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
                  AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i.86|amd64|AMD64|X86|i686")
    include(CheckCCompilerFlag)

    # -ffloat-store is separable, and it is the expensive one. It forces EVERY
    # float expression through memory; -fexcess-precision=standard already
    # gives C99-defined x87 semantics without that cost. Measured on this
    # toolchain: with LTO on, dropping -ffloat-store is -8.9% (15/15 paired
    # rounds) while parity stays BYTE-IDENTICAL 235/235 — the two flags
    # interact, because LTO exposes more cross-TU FP that then gets spilled.
    #
    # Kept ON by default anyway: it is reproducibility insurance ACROSS
    # compilers, not a correctness requirement on any one of them, and this
    # project's whole claim is byte-exactness. Turn it off knowingly.
    set(_spfy_fp_want
        -fno-fast-math
        -fexcess-precision=standard
        -mno-fma
    )
    if(SPFY_FLOAT_STORE)
        list(APPEND _spfy_fp_want -ffloat-store)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        list(APPEND _spfy_fp_want -mfpmath=387 -mlong-double-80)
    endif()

    set(_spfy_fp_saved "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-Werror")
    set(_spfy_fp_missing "")
    foreach(_f IN LISTS _spfy_fp_want)
        string(MAKE_C_IDENTIFIER "SPFY_HAS${_f}" _spfy_fp_var)
        check_c_compiler_flag("${_f}" ${_spfy_fp_var})
        if(${_spfy_fp_var})
            add_compile_options("${_f}")
        else()
            list(APPEND _spfy_fp_missing "${_f}")
        endif()
    endforeach()
    set(CMAKE_REQUIRED_FLAGS "${_spfy_fp_saved}")

    if(_spfy_fp_missing)
        message(WARNING
            "SPFY_STRICT_FP: this compiler rejected ${_spfy_fp_missing} — x87 "
            "semantics are not fully pinned, so synthesized audio bytes may "
            "differ in low bits from the i386 reference build. Unit-selection "
            "UIDs are unaffected.")
    endif()
elseif(SPFY_STRICT_FP AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS
        "SPFY_STRICT_FP: no x87 on ${CMAKE_SYSTEM_PROCESSOR} — skipping x87 "
        "pinning (UID path is integer and unaffected).")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion
                        -Wstrict-prototypes -Wmissing-prototypes)
endif()

# LTO, probed rather than assumed: CheckIPOSupported reports per-toolchain, so
# an emulated armv7 leg or an odd Clang simply builds without it instead of
# failing to configure. Release-only — a Debug build wants fast iteration and
# readable backtraces far more than it wants cross-TU inlining.
if(SPFY_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _spfy_ipo_ok OUTPUT _spfy_ipo_why)
    if(_spfy_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        message(STATUS "spfy: LTO enabled for Release")
    else()
        message(STATUS "spfy: LTO unavailable — ${_spfy_ipo_why}")
    endif()
endif()

# Apple's linker (Xcode 15+) warns "ignoring duplicate libraries" on nearly
# every link here, for two benign reasons:
#   * CMake repeats static libs to satisfy the circular deps between
#     spfy_voice / spfy_common / spfy_usel — intentional, and required by
#     GNU ld's single-pass archive resolution.
#   * libm arrives twice: from the global link_libraries(m) below AND from
#     host_emu's own `PUBLIC m`, which it must keep for standalone /
#     Android / wasm builds that never see this file.
# The linker resolves both correctly; the warnings just bury real
# diagnostics. Feature-tested because the flag is Xcode 15+ only.
if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(C "-Wl,-no_warn_duplicate_libraries"
                      SPFY_HAS_NO_WARN_DUP_LIBS)
    if(SPFY_HAS_NO_WARN_DUP_LIBS)
        add_link_options(-Wl,-no_warn_duplicate_libraries)
    endif()
endif()

if(SPFY_TRACE)
    add_compile_definitions(SPFY_TRACE=1)
endif()

if(SPFY_FE_HOSTED)
    # Surface the option to C code so CLIs can compile-switch their FE path.
    add_compile_definitions(SPFY_FE_HOSTED=1)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# Linux: many TUs (anchor_score, wsola, dsp, dump_f0, ...) call libm.
# On MinGW these come from libgcc; on glibc they live in libm and must
# be explicit. Adding to link_libraries() applies to all subsequent
# targets which is what we want.
if(NOT WIN32)
    link_libraries(m)
endif()

# MinGW: static-link libgcc + libstdc++ + libwinpthread so the produced
# binaries don't depend on mingw runtime DLLs (libgcc_s_dw2-1.dll,
# libwinpthread-1.dll, libstdc++-6.dll) that aren't present on stock
# Windows. Without this, regsvr32 on a fresh VM returns exit code 3
# ("failed to load module") when the SAPI DLL tries to load.
# add_link_options applies to every executable and shared library
# created in subsequent add_subdirectory() calls.
if(MINGW)
    add_link_options(-static-libgcc -static)
endif()

add_subdirectory(src/common)
add_subdirectory(src/voice)
add_subdirectory(src/cart)
add_subdirectory(src/usel)
add_subdirectory(src/vb)
add_subdirectory(src/wsola)
add_subdirectory(src/dsp)
add_subdirectory(src/prosody)
add_subdirectory(src/g2p)
add_subdirectory(src/text_norm)
# Baked POS dictionary lives in src/fe/ but is needed by src/fe_internal/
# regardless of FE config. Declared at the top level so subdir order doesn't matter.
add_library(spfy_baked_pos STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/fe/baked_pos.c)
target_include_directories(spfy_baked_pos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/fe)

add_subdirectory(src/fe_internal)
if(SPFY_FE_HOSTED)
    add_subdirectory(src/fe_host)
    # The hosted FE is always the emulated DLL now (the native PE loader
    # was retired 2026-07-22). Surface that globally so downstream CLIs
    # (spfy_synth.c) print the right startup banner.
    add_compile_definitions(SPFY_FE_EMU=1)
else()
    add_subdirectory(src/fe)
endif()
add_subdirectory(src/synth)
add_subdirectory(src/cli)
if(WIN32)
    add_subdirectory(src/sapi)
endif()

if(SPFY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/unit)
endif()
