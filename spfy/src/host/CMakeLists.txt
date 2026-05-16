# host/CMakeLists.txt — in-process DLL hosting library + smoke test
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The host library and test loader MUST be built as 32-bit x86 because
# the DLL we host (SWIttsFe-en-US.dll) is 32-bit x86 PE. If the parent
# project is 64-bit, this directory is a separate executable target —
# add `-DSPFY_FE_HOSTED=ON` at the top level to enable, and ensure the
# 32-bit toolchain is configured.

cmake_minimum_required(VERSION 3.20)

# Sanity check: i386 only for now. AMD64 hosting requires a 32-bit
# helper process (out of scope here).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(STATUS "host/: skipping — needs 32-bit build (current ptr=${CMAKE_SIZEOF_VOID_P}B). "
                   "Configure with -m32 / TOOLCHAIN_FILE pointing at a 32-bit toolchain.")
    return()
endif()

add_library(spfy_host STATIC
    loader.c
    imports.c
)
target_include_directories(spfy_host PUBLIC .)

if(WIN32)
    target_link_libraries(spfy_host PUBLIC winmm)
endif()

# PE loader inherently casts function pointers <-> object pointers
# (FARPROC from GetProcAddress, vtable slot indexing). ISO C forbids
# these, but the Windows ABI requires them. Suppress -Wpedantic and
# -Wcast-function-type on the loader sources only.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(spfy_host PRIVATE -Wno-pedantic -Wno-cast-function-type)
endif()

# Task 1's runtime probe — not part of the library; standalone tool.
if(WIN32)
    add_executable(probe_vtable probe_vtable.c)
    target_link_libraries(probe_vtable PRIVATE)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(probe_vtable PRIVATE -Wno-pedantic -Wno-cast-function-type)
    endif()
endif()

# Task 2's smoke test
add_executable(test_loader test_loader.c)
target_link_libraries(test_loader PRIVATE spfy_host)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(test_loader PRIVATE -Wno-pedantic -Wno-cast-function-type)
endif()

# Cross-platform: nothing extra needed for the loader itself. The Linux
# build will compile but test_loader's smoke path is Windows-only until
# Task 3's import stubs land (host/imports.c). The test main() reports
# that with a clear stderr message.
