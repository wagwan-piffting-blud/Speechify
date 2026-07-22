#!/usr/bin/env bash
# build_macos.sh — macOS build of the spfy core (Apple Silicon or Intel).
#
# No 32-bit anything: macOS dropped i386 entirely, and it isn't needed.
# The 32-bit Windows FE DLL runs through the portable host_emu x86
# interpreter, which fe_host/CMakeLists.txt auto-selects for any target
# that isn't 32-bit x86. The SAPI subdirectory is gated `if(WIN32)` and
# is skipped automatically.
#
# Verified byte-exact elsewhere: Linux x86_64 and Linux arm64 both emit a
# WAV identical to the Windows i386 reference build (see REF_SHA256
# below), so Apple Silicon is expected to match too. `verify` checks it.
#
# Setup:
#   brew install cmake ninja        # python3 ships with macOS / brew
#
# Usage:
#   ./spfy/build_macos.sh             # configure + build
#   ./spfy/build_macos.sh configure   # configure only
#   ./spfy/build_macos.sh verify      # build, synth Tom, compare sha256
#   ./spfy/build_macos.sh clean       # rm build dir
#
# Environment:
#   BUILD_DIR      where to build   (default: $TMPDIR/spfy_build_macos)
#   SPFY_OUT_WAV   verify's output  (default: $TMPDIR/spfy_macos_tom.wav)
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

# This script lives in <repo>/spfy/, so SRC_DIR is its own directory and
# the repo root is one level up.
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SRC_DIR/.." && pwd)"

# macOS sets TMPDIR to a per-user /var/folders/... path WITH a trailing
# slash, so naive concatenation yields '...T//spfy_build_macos'. Strip it.
# Note that dir is also periodically reaped by the OS — set BUILD_DIR to
# somewhere durable (e.g. BUILD_DIR=~/spfy_build) if you want it to persist.
SPFY_TMP="${TMPDIR:-/tmp}"
SPFY_TMP="${SPFY_TMP%/}"
BUILD_DIR="${BUILD_DIR:-$SPFY_TMP/spfy_build_macos}"

# Reference WAV hash: "The quick brown fox jumps over the lazy dog."
# through Tom, produced by the Windows i386 native-PE-loader build and
# reproduced bit-for-bit by Linux x86_64 and Linux arm64.
REF_SHA256="9b3f4dfc97c25da7ec21b03fc1f2a30e34eff31cb106706505c636a17be00371"

case "${1:-all}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "removed $BUILD_DIR"
        exit 0
        ;;
    configure|all|verify) ;;
    *)
        echo "usage: $0 [configure|all|verify|clean]" >&2
        exit 2
        ;;
esac

if [[ ! -f "$REPO_DIR/bin/SWIttsFe-en-US.dll" ]]; then
    echo "FE DLL not found at $REPO_DIR/bin/SWIttsFe-en-US.dll" >&2
    exit 1
fi

# No -m32 and no -D_GNU_SOURCE (that's a glibc thing). SPFY_STRICT_FP
# stays ON: on Intel it pins x87 semantics where the compiler supports
# the flags, and on Apple Silicon CMake reports that there is no x87 to
# pin and skips it. Flags AppleClang rejects (e.g. -mfpmath=387) are
# dropped by the probe with a warning rather than failing the build.
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPFY_STRICT_FP=ON \
    -DSPFY_BUILD_TESTS=OFF \
    -DSPFY_FE_HOSTED=ON

if [[ "${1:-all}" == "configure" ]]; then
    echo "configured at $BUILD_DIR"
    exit 0
fi

cmake --build "$BUILD_DIR"

BIN="$BUILD_DIR/src/cli/spfy_synth"
echo
echo "built: $BIN"
file "$BIN"

if [[ "${1:-all}" != "verify" ]]; then
    echo
    echo "try:  $BIN \\"
    echo "        $REPO_DIR/en-US/tom/tom.vin \\"
    echo "        $REPO_DIR/en-US/tom/tom8.vdb \\"
    echo "        $REPO_DIR/en-US/tom/tom.vcf \\"
    echo "        \"The quick brown fox jumps over the lazy dog.\" /tmp/_test.wav"
    echo
    echo "or:   $0 verify     # synth + compare against the reference hash"
    exit 0
fi

# ---- verify: synth Tom and compare the WAV against the reference ----
OUT="${SPFY_OUT_WAV:-$SPFY_TMP/spfy_macos_tom.wav}"
echo
echo "=== synthesizing Tom ==="
"$BIN" \
    "$REPO_DIR/en-US/tom/tom.vin" \
    "$REPO_DIR/en-US/tom/tom8.vdb" \
    "$REPO_DIR/en-US/tom/tom.vcf" \
    "The quick brown fox jumps over the lazy dog." \
    "$OUT"

got="$(shasum -a 256 "$OUT" | cut -d' ' -f1)"
echo
echo "  got:      $got"
echo "  expected: $REF_SHA256"
if [[ "$got" == "$REF_SHA256" ]]; then
    echo "  RESULT: BYTE-EXACT against the i386 reference ✅"
else
    echo "  RESULT: DIFFERS from the i386 reference ⚠"
    echo "  Unit selection may still match exactly (it is integer work);"
    echo "  the divergence would be in the float DSP tail. Compare a"
    echo "  spfy_synth_trace run to see whether UIDs agree."
    exit 1
fi
