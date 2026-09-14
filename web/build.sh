#!/usr/bin/env bash
# Compiles the renderer to WebAssembly for Web Workers and assembles the demo site
# in web/dist. Needs em++ on PATH (https://emscripten.org). CI runs this for Pages.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf dist && mkdir -p dist
em++ -std=c++17 -O3 -Wall -Wextra -I../include tracer_web.cpp \
  -sMODULARIZE=1 -sEXPORT_NAME=PhotonTracer -sENVIRONMENT=worker \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8 -sALLOW_MEMORY_GROWTH=1 \
  -o dist/photon_tracer.js
cp index.html app.js worker.js demo.css og.jpg dist/
echo "built web/dist"
