# SSE bisection toolchain. NOT FOR PRODUCTION.
#
# Used only for differential testing against the x87 canonical build, to
# localise floating-point divergences between MSVC 7.1 and GCC/Clang SSE.
#
# Output produced under this toolchain is NOT expected to be byte-exact with
# the Windows oracle. Use only via test/diff/locate_divergence.py.

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER gcc)
endif()

set(CMAKE_C_FLAGS_INIT         "-msse2 -mfpmath=sse -fno-fast-math -mno-fma")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG -DSPFY_BISECT_SSE=1")
