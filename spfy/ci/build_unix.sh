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
#   SPFY_VERSION       calver stamped into the binary and reported by
#                      `spfy_synth --version`. Left unset locally, where
#                      CMake falls back to dev-<sha> and the update checker
#                      then refuses to compare against a release.
#   SPFY_OSX_DEPLOYMENT_TARGET
#                      macOS floor, e.g. 11.0. UNSET MEANS THE HOST'S OWN
#                      VERSION, which is how the shipped x86_64 build came to
#                      demand macOS 15 -- a floor that excludes most of the
#                      Intel Macs the build exists for. Checked after linking,
#                      not assumed.
#   SPFY_MAX_GLIBC     highest glibc symbol version the Linux binaries are
#                      allowed to reference, e.g. 2.36. A ceiling, enforced
#                      below: the runner image decides this, so it can rise
#                      without anyone touching the code (Ubuntu 24.04's
#                      glibc 2.39 headers redirect strtol to
#                      __isoc23_strtol@GLIBC_2.38, which is exactly how the
#                      tarballs quietly stopped running on Debian 12).
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
REF_SHA="86dde7edb10eb9246ae997f70742cc2f1320de30f5fcb87412a052596bae0bdb"
# tom8.vdb, never tom16.vdb: the 16k file is real PCM and the mu-law
# decoder emits garbage from it.
REF_VIN="en-US/tom/tom.vin"
REF_VDB="en-US/tom/tom8.vdb"
REF_VCF="en-US/tom/tom.vcf"

if [ "$INSTALL_DEPS" = "1" ]; then
    echo "=== installing toolchain ==="
    if command -v apk >/dev/null 2>&1; then
        # Alpine, i.e. the musl legs.
        #
        # bash is deliberately NOT installed here: this script IS bash, so by
        # the time it runs the shell already had to exist. The workflow's
        # `sh -c` wrapper adds it before handing over.
        if [ "$MULTILIB" = "1" ]; then
            echo "ERROR: SPFY_MULTILIB=1 on Alpine -- there is no gcc-multilib" >&2
            echo "       there. Build 32-bit against glibc instead." >&2
            exit 1
        fi
        # shellcheck disable=SC2086
        apk add --no-cache build-base cmake ninja python3 file ca-certificates
    else
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
fi

echo "=== host: $(uname -m), $(nproc) cpu(s) ==="

# Passed only when CI set it. An empty -DSPFY_VERSION= would ALSO fall back
# to dev-<sha> (CMake treats "" as false), so this is belt and braces -- but
# it keeps `cmake ..` invocations readable in the log.
VERSION_ARG=""
if [ -n "${SPFY_VERSION:-}" ]; then
    VERSION_ARG="-DSPFY_VERSION=${SPFY_VERSION}"
fi

# CMAKE_OSX_DEPLOYMENT_TARGET rather than -mmacosx-version-min in CFLAGS:
# CMake injects its own copy of that flag on Apple, and two of them on one
# command line is a coin toss over which the driver honours.
OSX_ARG=""
if [ -n "${SPFY_OSX_DEPLOYMENT_TARGET:-}" ]; then
    OSX_ARG="-DCMAKE_OSX_DEPLOYMENT_TARGET=${SPFY_OSX_DEPLOYMENT_TARGET}"
fi

# shellcheck disable=SC2086
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$EXTRA_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    -DSPFY_STRICT_FP=ON \
    -DSPFY_BUILD_TESTS=OFF \
    -DSPFY_FE_HOSTED=ON \
    $VERSION_ARG $OSX_ARG

cmake --build "$BUILD_DIR"

SYNTH="$BUILD_DIR/src/cli/spfy_synth"
echo "=== build outputs ==="
ls -la "$SYNTH"
file "$SYNTH"

# ------------------------------------------------------------------
# How OLD a system will run what we just built.
#
# Both floors are decided by the BUILD HOST, not by anything in the source,
# so neither can be reasoned about from the code and both drift silently when
# a runner image is updated. On 2026.08.23 that had already happened twice:
# the Linux tarballs demanded glibc 2.38 (excluding Debian 12 and every
# Raspberry Pi OS bookworm install) and the Intel macOS build demanded
# macOS 15 (excluding most of the Intel Macs it exists to serve). Nothing
# failed; the binaries were simply unusable where they mattered.
#
# So the README's compatibility claim is a GATE here rather than a sentence
# someone remembers to re-check.
# ------------------------------------------------------------------
echo "=== compatibility floor ==="
case "$(uname -s)" in
Linux)
    # No `strings` in a slim container; grep -a over the binary finds the
    # same versioned-symbol references.
    #
    # ⚠ `|| true` is load-bearing. A binary with NO GLIBC_ references is a
    # perfectly good outcome -- musl, or a fully static link -- but grep
    # returns 1 on no match, and this script runs under `set -o pipefail`,
    # so without it the build dies here having printed only the header. That
    # is exactly what an Alpine build did.
    MAX_GLIBC="$( { grep -ao 'GLIBC_[0-9][0-9.]*' "$SYNTH" \
                    | sed 's/^GLIBC_//' | sort -uV | tail -1; } || true )"
    echo "highest glibc symbol referenced: ${MAX_GLIBC:-none}"
    if [ -n "${SPFY_MAX_GLIBC:-}" ] && [ -n "$MAX_GLIBC" ]; then
        worst="$(printf '%s\n%s\n' "$MAX_GLIBC" "$SPFY_MAX_GLIBC" \
                 | sort -V | tail -1)"
        if [ "$worst" != "$SPFY_MAX_GLIBC" ]; then
            echo "ERROR: needs glibc $MAX_GLIBC, ceiling is $SPFY_MAX_GLIBC." >&2
            echo "       This build will not run on the systems README.md" >&2
            echo "       promises. Build in an older container, or raise the" >&2
            echo "       ceiling AND the README together." >&2
            exit 1
        fi
    fi
    ;;
Darwin)
    otool -l "$SYNTH" | grep -E 'minos|version' | head -4 || true
    if [ -n "${SPFY_OSX_DEPLOYMENT_TARGET:-}" ]; then
        GOT="$(otool -l "$SYNTH" | awk '/minos/ {print $2; exit}')"
        echo "declared minimum macOS: ${GOT:-unknown}"
        if [ "$GOT" != "$SPFY_OSX_DEPLOYMENT_TARGET" ]; then
            echo "ERROR: asked for macOS $SPFY_OSX_DEPLOYMENT_TARGET, binary" >&2
            echo "       declares ${GOT:-nothing}. The deployment target did" >&2
            echo "       not take, so this build silently requires whatever" >&2
            echo "       the runner happens to be." >&2
            exit 1
        fi
    fi
    ;;
esac

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
