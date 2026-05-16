#!/usr/bin/env bash
# build_linux.sh — first Linux build of the spfy core.
#
# Mirrors build32.bat (the Windows mingw32 path). Linux gcc with
# multilib support is required because the FE DLL we host is 32-bit
# Windows PE — even on Linux the loader must run in a 32-bit address
# space to map the PE sections.
#
# Setup (Ubuntu/Debian):
#   sudo apt install -y build-essential cmake ninja-build \
#                       gcc-multilib g++-multilib python3
#
# Setup (Fedora/RHEL):
#   sudo dnf install -y gcc cmake ninja-build glibc-devel.i686 \
#                       libstdc++-devel.i686 python3
#
# The SAPI subdirectory is gated `if(WIN32)` in spfy/CMakeLists.txt
# and is skipped automatically here.
#
# Usage:
#   ./build_linux.sh             # configure + build
#   ./build_linux.sh configure   # configure only
#   ./build_linux.sh clean       # rm build dir

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)/spfy"
BUILD_DIR="${BUILD_DIR:-/tmp/spfy_build_linux32}"
SWITTSFE_DLL="${SWITTSFE_DLL:-$(dirname "$SRC_DIR")/bin/SWIttsFe-en-US.dll}"

case "${1:-all}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "removed $BUILD_DIR"
        exit 0
        ;;
    configure|all)
        ;;
    *)
        echo "usage: $0 [configure|all|clean]" >&2
        exit 2
        ;;
esac

if [[ ! -f "$SWITTSFE_DLL" ]]; then
    echo "FE DLL not found at $SWITTSFE_DLL — set SWITTSFE_DLL=<path>" >&2
    exit 1
fi

# 32-bit flags applied to compile + link via init-cache. These reach the
# linker so libgcc / libc are picked from the multilib paths. Strict-FP
# uses x87 80-bit on i386 (default for -m32 on x86_64 hosts).
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-m32 -D_GNU_SOURCE=1" \
    -DCMAKE_EXE_LINKER_FLAGS="-m32" \
    -DCMAKE_SHARED_LINKER_FLAGS="-m32" \
    -DSPFY_STRICT_FP=ON \
    -DSPFY_BUILD_TESTS=OFF \
    -DSPFY_FE_HOSTED=ON \
    -DSWITTSFE_DLL="$SWITTSFE_DLL"

if [[ "${1:-all}" == "configure" ]]; then
    echo "configured at $BUILD_DIR"
    exit 0
fi

cmake --build "$BUILD_DIR"
echo
echo "built: $BUILD_DIR"
echo "try:   $BUILD_DIR/src/cli/spfy_synth en-US/tom/tom.vin en-US/tom/tom8.vdb en-US/tom/tom.vcf \\"
echo "         spfy/data/tom_hpclass.bin spfy/build/fe_symbol_table.json \\"
echo "         spfy/data/fe_tables_a spfy/data/fe_tables \\"
echo "         \"The quick brown fox jumps over the lazy dog.\" /tmp/_test.wav"
