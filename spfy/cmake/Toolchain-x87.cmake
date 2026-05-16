# Canonical 1:1 reproduction toolchain.
# Targets: Linux x86_64, GCC or Clang.
#
# Forces x87 FPU + 80-bit long double to match MSVC 7.1 (2003) FP semantics
# from the original Speechify Windows DLLs. See plan: bit-exact FP strategy.

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# These can be overridden by env (CC, CXX) for cross-compilation from Windows.
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER gcc)
endif()

# Forbidden globally: -ffast-math, -funsafe-math-optimizations, -Ofast, FMA.
set(_SPFY_FP_FLAGS
    "-mfpmath=387"
    "-mlong-double-80"
    "-fexcess-precision=standard"
    "-ffloat-store"
    "-fno-fast-math"
    "-mno-fma"
)
string(JOIN " " _SPFY_FP_FLAGS_STR ${_SPFY_FP_FLAGS})

set(CMAKE_C_FLAGS_INIT         "${_SPFY_FP_FLAGS_STR}")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
