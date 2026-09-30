#!/usr/bin/env bash
# Builds bench/wasm.cpp with the demo's compiler flags and runs it under Node.
# Needs em++ and node on PATH. Extra arguments are passed to em++, e.g.
# bench/wasm.sh -mno-simd128 to compare against the scalar fallback.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
em++ -std=c++17 -O3 -msimd128 -flto -Iinclude "$@" bench/wasm.cpp \
  -sENVIRONMENT=node -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -o "$out/bench.js" 2>&1 \
  | grep -vE '^(cache|system_libs):' || true
test -f "$out/bench.js"
node "$out/bench.js"
