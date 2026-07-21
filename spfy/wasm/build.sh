#!/usr/bin/env bash
# Emscripten build driver for the in-browser spfy demo.
#
# Prereqs:
#   - emsdk installed and activated (`source ~/emsdk/emsdk_env.sh`).
#   - Run from spfy/wasm/ (or anywhere — the script cds to its own dir).
#
# Outputs (in dist/):
#   spfy_wasm.js        ES-module factory (createSpfyModule).
#   spfy_wasm.wasm      WebAssembly bytecode.
#   voices/             Lazy voice assets + manifest.json (fetched on
#                       demand by the browser; no .data sidecar).
#
# After build, `npm run dev` serves the demo with hot reload.

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "error: emcmake not in PATH. Activate emsdk first:" >&2
    echo "  source ~/emsdk/emsdk_env.sh" >&2
    exit 1
fi

BUILD_DIR=build
BUILD_TYPE=${BUILD_TYPE:-Release}

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "$BUILD_DIR" dist
    echo "cleaned $BUILD_DIR/ and dist/"
    exit 0
fi

mkdir -p "$BUILD_DIR" dist

echo "==> emcmake configure ($BUILD_TYPE)"
emcmake cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -G "Unix Makefiles"

echo "==> emmake build"
emmake cmake --build "$BUILD_DIR" -j

echo
echo "Built artifacts:"
ls -lh dist/spfy_wasm.* 2>/dev/null || echo "  (nothing in dist/)"
echo "Staged voices:"
ls -1 dist/voices/*/*/ -d 2>/dev/null | sed 's/^/  /' || echo "  (none staged)"
echo
echo "Run the demo:"
echo "  npm install     # one-time"
echo "  npm run dev     # webpack-dev-server on http://localhost:8080"
