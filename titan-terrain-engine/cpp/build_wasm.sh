#!/bin/bash
# Builds libTitanCore to WebAssembly for the TitanLab web viewer.
# The exact same C++ sources are compiled natively for the Mac app / Unreal
# plugin — this is just a second target, not a second implementation.
set -euo pipefail

cd "$(dirname "$0")"

OUT_DIR="../src/wasm"
mkdir -p "$OUT_DIR"

emcc -O3 -std=c++20 -fno-fast-math \
  -I libTitanCore/include \
  libTitanCore/src/TitanNoise.cpp \
  libTitanCore/src/TerrainEngine.cpp \
  libTitanCore/src/Erosion.cpp \
  libTitanCore/src/Fluvial.cpp \
  libTitanCore/src/Export.cpp \
  libTitanCore/src/CAPI.cpp \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createTitanModule \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sSINGLE_FILE=1 \
  -sFILESYSTEM=0 \
  -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU32,HEAPU8,UTF8ToString \
  -o "$OUT_DIR/titan_core.js"

echo "OK: $OUT_DIR/titan_core.js"
