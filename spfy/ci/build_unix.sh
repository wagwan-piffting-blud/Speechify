#!/usr/bin/env bash
# Shared configure + build + verify for the CI `build-unix` matrix.
#
# Every unix leg runs THIS script, so a native leg and an emulated
# (cross-arch) leg cannot drift apart in flags, build type, or what counts
# as passing. The native legs invoke it directly on the runner; the armv7
# leg invokes it inside a qemu-emulated debian container with the
# workspace bind-mounted.
#
# Configured entirely through the environment so the workflow can pass
# matrix values without this script needing to know about matrices:
#
#   SPFY_SRC_DIR       source dir            (default: spfy)
#   SPFY_BUILD_DIR     build dir             (default: build-out)
#   SPFY_CFLAGS        extra CMAKE_C_FLAGS   (default: empty)
#   SPFY_LDFLAGS       extra linker flags    (default: empty)
#   SPFY_INSTALL_DEPS  1 = apt-get the toolchain first (containers only:
#                      assumes root and a Debian/Ubuntu base)
#   SPFY_MULTILIB      1 = also install gcc-multilib (the i386 leg)
#   SPFY_VERIFY        1 = synthesize and check the reference hash
#                      (default 1; set 0 to build only)
#
# Deps are installed HERE rather than in a separate workflow step because
# a container leg gets one `docker run`; a second run would start from a
# fresh layer with the packages gone. Verification is here for the same
# reason -- an emulated binary needs the target's shared libraries, so it
# can only run inside that same container, not on the host.

set -euo pipefail

SRC_DIR="${SPFY_SRC_DIR:-spfy}"
BUILD_DIR="${SPFY_BUILD_DIR:-build-out}"
EXTRA_CFLAGS="${SPFY_CFLAGS:-}"
EXTRA_LDFLAGS="${SPFY_LDFLAGS:-}"
INSTALL_DEPS="${SPFY_INSTALL_DEPS:-0}"
MULTILIB="${SPFY_MULTILIB:-0}"
VERIFY="${SPFY_VERIFY:-1}"

# "The quick brown fox jumps over the lazy dog." through en-US/tom must
# produce this exact WAV on every target. Byte-exactness IS the ship gate
# for this project, so CI checks the bytes rather than just the exit code
# -- a build-only gate would have gone green through the whole armv7
# SIGBUS, which only ever manifested at runtime.
REF_TEXT="The quick brown fox jumps over the lazy dog."
REF_SHA="9b3f4dfc97c25da7ec21b03fc1f2a30e34eff31cb106706505c636a17be00371"
# tom8.vdb, never tom16.vdb: the 16k file is real PCM and the mu-law
# decoder emits garbage from it.
REF_VIN="en-US/tom/tom.vin"
REF_VDB="en-US/tom/tom8.vdb"
REF_VCF="en-US/tom/tom.vcf"

if [ "$INSTALL_DEPS" = "1" ]; then
    echo "=== installing toolchain ==="
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    pkgs="build-essential cmake ninja-build python3 file ca-certificates"
    if [ "$MULTILIB" = "1" ]; then
        pkgs="$pkgs gcc-multilib g++-multilib"
    fi
    # Unquoted on purpose: $pkgs is a word list, not one argument.
    # shellcheck disable=SC2086
    apt-get install -y -qq --no-install-recommends $pkgs
fi

echo "=== host: $(uname -m), $(nproc) cpu(s) ==="

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$EXTRA_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    -DSPFY_STRICT_FP=ON \
    -DSPFY_BUILD_TESTS=OFF \
    -DSPFY_FE_HOSTED=ON

cmake --build "$BUILD_DIR"

SYNTH="$BUILD_DIR/src/cli/spfy_synth"
echo "=== build outputs ==="
ls -la "$SYNTH"
file "$SYNTH"

if [ "$VERIFY" != "1" ]; then
    echo "=== verification skipped (SPFY_VERIFY=$VERIFY) ==="
    exit 0
fi

if [ ! -f "$REF_VIN" ] || [ ! -f "$REF_VDB" ] || [ ! -f "$REF_VCF" ]; then
    echo "::warning title=Verification skipped::reference voice not present" >&2
    echo "  looked for $REF_VIN / $REF_VDB / $REF_VCF" >&2
    exit 0
fi

echo "=== verify: reference synthesis ==="
out_wav="${BUILD_DIR}/ref_tom.wav"
"$SYNTH" "$REF_VIN" "$REF_VDB" "$REF_VCF" "$REF_TEXT" "$out_wav"

# sha256sum on Linux, shasum on macOS.
if command -v sha256sum >/dev/null 2>&1; then
    got="$(sha256sum "$out_wav" | cut -d' ' -f1)"
else
    got="$(shasum -a 256 "$out_wav" | cut -d' ' -f1)"
fi

echo "  got:      $got"
echo "  expected: $REF_SHA"
if [ "$got" != "$REF_SHA" ]; then
    echo "::error title=Fidelity regression::$(uname -m) output does not match the reference WAV" >&2
    exit 1
fi
echo "  BYTE-EXACT"
