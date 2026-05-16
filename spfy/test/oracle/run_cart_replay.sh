#!/usr/bin/env bash
# Replay every captured cart_walks/*.jsonl through spfy_cart_replay and
# aggregate pass / fail counts.
#
# usage: run_cart_replay.sh [VOICE_DIR] [TRACES_DIR] [REPLAY_EXE]
set -euo pipefail

PROJ_ROOT=${PROJ_ROOT:-$HOME/Documents/Speechify}
VOICE_DIR=${1:-${PROJ_ROOT}/en-US/tom}
TRACES_DIR=${2:-${PROJ_ROOT}/spfy/test/oracle/traces/cart_walks}
REPLAY_EXE=${3:-c:/tmp/spfy_build/src/cli/spfy_cart_replay.exe}
VIN=${VIN:-${VOICE_DIR}/tom.vin}

if [[ ! -x "$REPLAY_EXE" ]]; then
    echo "build first: cd spfy && build.bat" >&2
    exit 2
fi
if [[ ! -d "$TRACES_DIR" ]]; then
    echo "no traces at $TRACES_DIR" >&2
    exit 2
fi

total=0
pass=0
declare -a fails

for f in "$TRACES_DIR"/*.jsonl; do
    [[ -e "$f" ]] || continue
    name=$(basename "$f" .jsonl)
    out=$("$REPLAY_EXE" "$VIN" "$f" 2>&1 || true)
    walks=$(grep -oE 'walks +: [0-9]+' <<<"$out" | grep -oE '[0-9]+' || echo 0)
    p=$(grep -oE 'passed +: [0-9]+' <<<"$out" | grep -oE '[0-9]+' || echo 0)
    total=$((total + walks))
    pass=$((pass + p))
    if [[ "$walks" -gt 0 && "$p" -eq "$walks" ]]; then
        printf '  OK   %-15s  %4d/%-4d\n' "$name" "$p" "$walks"
    else
        printf '  FAIL %-15s  %4d/%-4d\n' "$name" "$p" "$walks"
        fails+=("$name")
    fi
done

printf '\ntotal walks    : %d\n' "$total"
printf 'total passed   : %d  (%.1f%%)\n' "$pass" "$(awk -v p=$pass -v t=$total 'BEGIN{print (t? p*100.0/t : 0)}')"
if [[ ${#fails[@]} -gt 0 ]]; then
    printf 'failed entries : %s\n' "${fails[*]}"
    exit 1
fi
