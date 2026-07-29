#!/bin/bash
# Builds libTitanCore to WebAssembly for the TitanLab web viewer.
# The exact same C++ sources are compiled natively for the Mac app / Unreal
# plugin — this is just a second target, not a second implementation.
set -euo pipefail

# WASM_BIGINT matters: the export functions return int64_t, and without it
# emscripten legalizes an i64 return into a truncated i32 — silently reviving
# the >2 GB export overflow the int64 change exists to fix.

cd "$(dirname "$0")"

OUT_DIR="../src/wasm"
mkdir -p "$OUT_DIR"

# -ffp-contract=off matters as much as -fno-fast-math: without it the compiler
# may fuse a*b+c into an FMA where the target has one, making results depend on
# the instruction set rather than on the seed. Keep in step with CMakeLists.txt.
emcc -O3 -std=c++20 -fno-fast-math -ffp-contract=off \
  -I libTitanCore/include \
  libTitanCore/src/TitanNoise.cpp \
  libTitanCore/src/TerrainEngine.cpp \
  libTitanCore/src/Erosion.cpp \
  libTitanCore/src/Fluvial.cpp \
  libTitanCore/src/Layers.cpp \
  libTitanCore/src/Volcano.cpp \
  libTitanCore/src/Filters.cpp \
  libTitanCore/src/Export.cpp \
  libTitanCore/src/CAPI.cpp \
  -sWASM_BIGINT=1 \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createTitanModule \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sSINGLE_FILE=1 \
  -sFILESYSTEM=0 \
  -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU32,HEAPU8,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -o "$OUT_DIR/titan_core.js"

echo "OK: $OUT_DIR/titan_core.js"
