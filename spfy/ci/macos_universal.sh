#!/usr/bin/env bash
# Fuse the two per-arch macOS spfy_synth builds into one universal binary,
# then run EACH slice against the reference WAV.
#
#   bash spfy/ci/macos_universal.sh <arm64-binary> <x86_64-binary> <out>
#
# Run from the repo root on Apple Silicon. The output is AD-HOC signed, not
# Developer ID: macos_sign.sh signs it next, and its quarantine control is a
# copy of whatever it is handed -- a Developer ID input would make the control
# launch too and the gate would report itself inert.
#
# ⚠ Each input was already verified natively in its own build leg. What this
# adds is proof that the FUSED file still carries two working slices: lipo
# can take a wrong-arch input without complaint, and the x86_64 slice is
# otherwise never executed on the arm64 host that ships it. It runs under
# Rosetta, installed here if the host lacks it (measured byte-exact under
# Rosetta on macOS 26.6.2, M4).

set -euo pipefail

ARM="${1:?usage: macos_universal.sh <arm64> <x86_64> <out>}"
X86="${2:?usage: macos_universal.sh <arm64> <x86_64> <out>}"
OUT="${3:?usage: macos_universal.sh <arm64> <x86_64> <out>}"

REF_TEXT="The quick brown fox jumps over the lazy dog."
REF_SHA="86dde7edb10eb9246ae997f70742cc2f1320de30f5fcb87412a052596bae0bdb"
REF_VIN="$(pwd)/en-US/tom/tom.vin"
REF_VDB="$(pwd)/en-US/tom/tom8.vdb"
REF_VCF="$(pwd)/en-US/tom/tom.vcf"

[ "$(lipo -archs "$ARM")" = "arm64" ] \
    || { echo "::error::$ARM is '$(lipo -archs "$ARM")', expected arm64" >&2; exit 1; }
[ "$(lipo -archs "$X86")" = "x86_64" ] \
    || { echo "::error::$X86 is '$(lipo -archs "$X86")', expected x86_64" >&2; exit 1; }

echo "=== lipo ==="
lipo -create "$ARM" "$X86" -output "$OUT"
# Absolute from here on: the slice runs below cd into a scratch dir.
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
# Order-independent: lipo does not promise which slice it lists first.
got_archs="$(lipo -archs "$OUT" | tr ' ' '\n' | sort | xargs)"
[ "$got_archs" = "arm64 x86_64" ] \
    || { echo "::error::universal carries '$got_archs'" >&2; exit 1; }
file "$OUT"

# The inputs arrive Developer-ID signed from their legs; see the header.
codesign --remove-signature "$OUT"
codesign --force --sign - "$OUT"
chmod +x "$OUT"

if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
    echo "=== installing Rosetta ==="
    sudo softwareupdate --install-rosetta --agree-to-license
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail=0
for a in arm64 x86_64; do
    echo "=== slice $a ==="
    ( cd "$work" && arch "-$a" "$OUT" "$REF_VIN" "$REF_VDB" "$REF_VCF" \
        "$REF_TEXT" "$work/$a.wav" )
    got="$(shasum -a 256 "$work/$a.wav" | cut -d' ' -f1)"
    echo "  got:      $got"
    if [ "$got" != "$REF_SHA" ]; then
        echo "::error title=Fidelity regression::universal $a slice does not match the reference WAV" >&2
        fail=1
    else
        echo "  BYTE-EXACT"
    fi
done
exit "$fail"
