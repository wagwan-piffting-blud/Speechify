#!/usr/bin/env bash
# Developer ID sign + notarize a bare macOS executable, then prove it runs
# QUARANTINED -- the state a browser download is in.
#
#   bash spfy/ci/macos_sign.sh <binary>
#
# Signs IN PLACE. Run from the repo root (the reference voice paths below are
# relative to it, same as build_unix.sh).
#
# Environment:
#   SPFY_SIGN_IDENTITY   codesign identity: SHA-1 or full name. Optional with
#                        SPFY_P12: the one Developer ID Application identity
#                        in it is used (anything but exactly one is an error)
#   SPFY_P12 + SPFY_P12_PASSWORD
#                        Developer ID .p12 to import into a throwaway keychain
#                        that is deleted on exit. CI always uses this; so can
#                        an ssh session, where the login keychain is locked
#                        and codesign fails with errSecInternalComponent.
#                        Unset = sign from the default search list.
#   Notary credentials, ONE of:
#     SPFY_NOTARY_PROFILE                      notarytool keychain profile
#     SPFY_NOTARY_KEY + SPFY_NOTARY_KEY_ID + SPFY_NOTARY_ISSUER
#                                              App Store Connect API key
#                                              (.p8 path, key id, issuer uuid)
#   SPFY_QUARANTINE_TIMEOUT  seconds before a quarantined run counts as
#                            blocked (default 90)
#
# ⚠ A bare Mach-O cannot carry a stapled ticket -- stapler only takes .app,
# .pkg and .dmg. Gatekeeper finds the ticket ONLINE on first launch instead,
# so an offline Mac with a quarantined download still refuses it. That is the
# trade for shipping one file.
#
# ⚠ Gatekeeper does not FAIL a blocked command-line launch, it HANGS it
# (measured on macOS 26.6.2 over ssh: an ad-hoc binary with a Safari
# quarantine xattr never returned). Hence the timeout, and hence the control:
# the unsigned copy must be blocked too, or the quarantined run proves nothing
# on this host (Gatekeeper off, or quarantine not enforced).

set -euo pipefail

BIN="${1:?usage: macos_sign.sh <binary>}"
IDENTITY="${SPFY_SIGN_IDENTITY:-}"
[ -n "$IDENTITY" ] || [ -n "${SPFY_P12:-}" ] \
    || { echo "ERROR: set SPFY_SIGN_IDENTITY or SPFY_P12" >&2; exit 1; }
QTIMEOUT="${SPFY_QUARANTINE_TIMEOUT:-90}"

REF_TEXT="The quick brown fox jumps over the lazy dog."
REF_SHA="86dde7edb10eb9246ae997f70742cc2f1320de30f5fcb87412a052596bae0bdb"
REF_VIN="$(pwd)/en-US/tom/tom.vin"
REF_VDB="$(pwd)/en-US/tom/tom8.vdb"
REF_VCF="$(pwd)/en-US/tom/tom.vcf"

notary_args=()
if [ -n "${SPFY_NOTARY_PROFILE:-}" ]; then
    notary_args=(--keychain-profile "$SPFY_NOTARY_PROFILE")
elif [ -n "${SPFY_NOTARY_KEY:-}" ]; then
    notary_args=(--key "$SPFY_NOTARY_KEY"
                 --key-id "${SPFY_NOTARY_KEY_ID:?SPFY_NOTARY_KEY_ID is required with SPFY_NOTARY_KEY}"
                 --issuer "${SPFY_NOTARY_ISSUER:?SPFY_NOTARY_ISSUER is required with SPFY_NOTARY_KEY}")
else
    echo "ERROR: no notary credentials (SPFY_NOTARY_PROFILE or SPFY_NOTARY_KEY)" >&2
    exit 1
fi
work="$(mktemp -d)"
KC=""
orig_search=""
cleanup() {
    if [ -n "$KC" ]; then
        # shellcheck disable=SC2086
        [ -n "$orig_search" ] && security list-keychains -d user -s $orig_search || true
        security delete-keychain "$KC" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

kc_args=()
if [ -n "${SPFY_P12:-}" ]; then
    KC="$work/spfy-sign.keychain-db"
    kc_pw="$(uuidgen)"
    security create-keychain -p "$kc_pw" "$KC"
    security set-keychain-settings -lut 3600 "$KC"
    security unlock-keychain -p "$kc_pw" "$KC"
    security import "$SPFY_P12" -k "$KC" -P "${SPFY_P12_PASSWORD:?SPFY_P12_PASSWORD is required with SPFY_P12}" \
        -f pkcs12 -T /usr/bin/codesign
    security set-key-partition-list -S apple-tool:,apple:,codesign: \
        -s -k "$kc_pw" "$KC" >/dev/null
    # codesign resolves the identity's chain through the user search list, so
    # the keychain goes ON it, not just on the command line. Restored on exit.
    orig_search="$(security list-keychains -d user | tr -d '"' | xargs)"
    # shellcheck disable=SC2086
    security list-keychains -d user -s "$KC" $orig_search
    kc_args=(--keychain "$KC")
    if [ -z "$IDENTITY" ]; then
        ids="$(security find-identity -v -p codesigning "$KC" \
               | awk '/"Developer ID Application: / {print $2}')"
        n="$(printf '%s' "$ids" | grep -c . || true)"
        [ "$n" = "1" ] \
            || { echo "ERROR: $SPFY_P12 holds $n Developer ID Application identities, need exactly 1" >&2; exit 1; }
        IDENTITY="$ids"
    fi
    echo "  identity: $IDENTITY"
fi

# Returns the command's exit code, or 124 if it had to be killed.
run_capped() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$secs" ]; then
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    local rc=0
    wait "$pid" || rc=$?
    return "$rc"
}

quarantine() {
    xattr -w com.apple.quarantine \
        "0081;$(printf %x "$(date +%s)");Safari;$(uuidgen)" "$1"
}

sha_of() {
    shasum -a 256 "$1" | cut -d' ' -f1
}

cp "$BIN" "$work/control"

echo "=== sign ==="
codesign --force --options runtime --timestamp \
    "${kc_args[@]}" --sign "$IDENTITY" "$BIN"
codesign --verify --strict --verbose=2 "$BIN"
codesign -dvv "$BIN" 2>&1 | grep -E '^(Authority|TeamIdentifier|Timestamp|CodeDirectory)' || true

echo "=== notarize ==="
ditto -c -k --keepParent "$BIN" "$work/submit.zip"
xcrun notarytool submit "$work/submit.zip" "${notary_args[@]}" \
    --wait --timeout 30m --output-format json > "$work/notary.json"
cat "$work/notary.json"; echo
status="$(plutil -extract status raw -o - "$work/notary.json" 2>/dev/null || echo unknown)"
sub_id="$(plutil -extract id raw -o - "$work/notary.json" 2>/dev/null || echo "")"
if [ "$status" != "Accepted" ]; then
    echo "::error title=Notarization::status '$status' for $(basename "$BIN")" >&2
    if [ -n "$sub_id" ]; then
        xcrun notarytool log "$sub_id" "${notary_args[@]}" >&2 || true
    fi
    exit 1
fi
echo "  Accepted ($sub_id)"

echo "=== quarantined launch ==="
[ -f "$REF_VIN" ] && [ -f "$REF_VDB" ] && [ -f "$REF_VCF" ] \
    || { echo "ERROR: reference voice not found under $(pwd)/en-US/tom" >&2; exit 1; }

quarantine "$work/control"
crc=0
( cd "$work" && run_capped "$QTIMEOUT" "$work/control" --version >/dev/null 2>&1 ) || crc=$?
pkill -9 -f "$work/control" 2>/dev/null || true

mkdir -p "$work/q"
cp "$BIN" "$work/q/spfy_synth"
quarantine "$work/q/spfy_synth"
src=0
( cd "$work/q" && run_capped "$QTIMEOUT" "$work/q/spfy_synth" \
    "$REF_VIN" "$REF_VDB" "$REF_VCF" "$REF_TEXT" "$work/q/ref.wav" ) || src=$?

if [ "$src" -ne 0 ]; then
    echo "::error title=Quarantine::signed binary did not run quarantined (rc=$src; 124 = blocked/hung)" >&2
    exit 1
fi
got="$(sha_of "$work/q/ref.wav")"
echo "  got:      $got"
echo "  expected: $REF_SHA"
if [ "$got" != "$REF_SHA" ]; then
    echo "::error title=Fidelity regression::signed binary output differs from the reference WAV" >&2
    exit 1
fi

if [ "$crc" -eq 0 ]; then
    # The unsigned control ran too, so this host does not enforce quarantine
    # and the pass above says nothing about Gatekeeper. Notarization status
    # is still a hard gate; only this launch test is inconclusive.
    echo "::warning title=Quarantine gate inert::unsigned control ran quarantined on this host; only notarization status is proven" >&2
else
    echo "  control (unsigned) blocked: rc=$crc"
fi
echo "  SIGNED + NOTARIZED, runs quarantined, BYTE-EXACT"
