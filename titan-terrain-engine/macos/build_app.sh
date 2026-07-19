#!/bin/bash
# Builds TitanLab.app: native libTitanCore + Swift sources -> signed bundle.
#
#   ./build_app.sh            build the app bundle
#   ./build_app.sh --smoke    build, then run the headless smoke test
#
# Signing is ad-hoc ("-"). For distribution, re-sign with a Developer ID
# certificate and notarize (see docs/launch/notarization-checklist.md).
set -euo pipefail

cd "$(dirname "$0")"

echo "==> Building libTitanCore (native)"
cmake -S ../cpp -B ../cpp/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build ../cpp/build --target TitanCore 2>&1 | tail -1

echo "==> Compiling Swift sources"
mkdir -p build
xcrun swiftc -O -swift-version 5 \
  -import-objc-header TitanLab-Bridging-Header.h \
  -I ../cpp/libTitanCore/include \
  Sources/Main.swift \
  Sources/EngineModel.swift \
  Sources/Shaders.swift \
  Sources/Renderer.swift \
  Sources/ContentView.swift \
  ../cpp/build/libTitanCore.a \
  -lc++ \
  -framework SwiftUI -framework MetalKit -framework Metal -framework AppKit \
  -o build/TitanLab

echo "==> Bundling TitanLab.app"
APP=build/TitanLab.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/TitanLab "$APP/Contents/MacOS/TitanLab"
cp Info.plist "$APP/Contents/Info.plist"

codesign --force --sign - "$APP"

echo "OK: $APP"

if [[ "${1:-}" == "--smoke" ]]; then
  echo "==> Running smoke test"
  "$APP/Contents/MacOS/TitanLab" --smoke-test
fi
