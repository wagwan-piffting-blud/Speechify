# spfy/src/host_emu/CMakeLists.txt — portable x86 PE32 emulator
#
# Builds a host-agnostic interpreter that runs the unmodified 32-bit
# SWIttsFe-en-US.dll on top of any compiler / pointer size. This is the
# cross-platform alternative to src/host/ (which only works on x86
# Windows). Used by:
#
#   - spfy/src/fe_host/  (the only FE backend)          — phase 2 wiring
#   - spfy/wasm/                                        — phase 4 cross-build
#   - D:/Android/Spfy/app/src/main/cpp/CMakeLists.txt   — phase 4 NDK build
#
# Unlike src/host/, this target has NO 32-bit pointer-size gate. The
# emulator IS the 32-bit-ness; the host build can be 64-bit.
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

# Allow standalone configuration (build.bat in this dir) AND being
# add_subdirectory()'d from spfy/CMakeLists.txt later. `project()` only
# fires for the standalone case — when invoked as a subdir the parent
# already provided one.
if(NOT PROJECT_NAME)
    project(spfy_host_emu C)
endif()

add_library(spfy_host_emu STATIC
    cpu.c
    loader.c
    mem.c
    win32_donor.c
    spfy_extra_shims.c
    host_glue.c
    spfy_dll_boot.c
)

target_include_directories(spfy_host_emu
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Position-independent code: lets this static lib be folded into shared
# libs later (libspfy.so on Android, spfy_fe.dll on Windows, the WASM
# side-module on emscripten — all need PIC).
set_target_properties(spfy_host_emu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # Donor uses fopen/strcpy/_snprintf without the *_s variants. Silence
    # the C4996 noise without rewriting the donor (it's the upstream
    # source of truth and stays close to its origin).
    target_compile_definitions(spfy_host_emu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
    )
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # MATCHES, not STREQUAL: AppleClang (macOS) must be covered too, and
    # it reports its id as "AppleClang".
    #
    # Donor declares some helpers later in the same TU (forward decls in
    # the comments); allow it without -Werror tripping. The donor also
    # casts function pointers across shim_fn signatures intentionally.
    #
    # The conversion-class suppressions below answer the root CMakeLists'
    # -Wconversion / -Wsign-conversion / -Wmissing-prototypes /
    # -Wmisleading-indentation set. They fire in bulk (~250) only on
    # 64-bit hosts, where this file set is compiled for the first time
    # (32-bit x86 builds take the native src/host/ loader instead). The
    # noise is inherent to the job: an x86 decoder mixes uint8/16/32 and
    # signed/unsigned displacements on nearly every line, and narrows
    # between guest 32-bit words and host size_t/pointers constantly.
    # Suppressed deliberately and target-scoped — same precedent as
    # spfy_fe's -Wno-cast-function-type — rather than loosening the
    # warning set for the whole build. Correctness here is held by the
    # byte-exact audit, not by -Wconversion.
    target_compile_options(spfy_host_emu PRIVATE
        -Wno-pointer-sign
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-cast-function-type
        -Wno-conversion
        -Wno-sign-conversion
        -Wno-missing-prototypes
        -Wno-misleading-indentation
    )
endif()

# Math lib for x87 trampolines (sin/cos/exp/pow/etc.) on Linux/Android.
if(NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_libraries(spfy_host_emu PUBLIC m)
endif()

# Phase 1 smoke-test harness — only built when requested so the
# audit/CI build stays focused on the library.
option(SPFY_HOST_EMU_BUILD_TESTS "Build the host_emu smoke test executable" OFF)
if(SPFY_HOST_EMU_BUILD_TESTS)
    add_executable(test_emu_boot test_emu_boot.c)
    target_link_libraries(test_emu_boot PRIVATE spfy_host_emu)
endif()
