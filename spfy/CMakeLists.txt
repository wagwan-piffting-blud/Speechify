cmake_minimum_required(VERSION 3.20)
project(spfy LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPFY_TRACE        "Emit JSONL stage traces from synth core" OFF)
option(SPFY_BUILD_TESTS  "Build unit tests"                         ON)
option(SPFY_STRICT_FP    "Enforce x87 long-double FP discipline"    ON)
option(SPFY_FE_HOSTED    "Use in-process SWIttsFe-en-US.dll host instead of hand-written src/fe/"
                                                                    ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(SPFY_STRICT_FP AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
                  AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i.86|amd64|AMD64")
    add_compile_options(
        -fno-fast-math
        -fexcess-precision=standard
        -ffloat-store
        -mno-fma
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        add_compile_options(-mfpmath=387 -mlong-double-80)
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion
                        -Wstrict-prototypes -Wmissing-prototypes)
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
add_subdirectory(src/wsola)
add_subdirectory(src/dsp)
add_subdirectory(src/g2p)
add_subdirectory(src/text_norm)
# Baked POS dictionary lives in src/fe/ but is needed by src/fe_internal/
# (in-house pure-C FE for ARM Android) regardless of FE config. Declared
# at the top level so subdir order doesn't matter.
add_library(spfy_baked_pos STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/fe/baked_pos.c)
target_include_directories(spfy_baked_pos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/fe)

add_subdirectory(src/fe_internal)
if(SPFY_FE_HOSTED)
    add_subdirectory(src/fe_host)
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
