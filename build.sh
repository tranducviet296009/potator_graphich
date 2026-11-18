#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build_arm64"
OUT_ZIP="$ROOT_DIR/PotatoGFX-KSU-v1.1.0-arm64.zip"

echo "===================================================="
echo "   PotatoGFX-KSU Automated NDK Builder (arm64-v8a)  "
echo "===================================================="

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$ROOT_DIR/zygisk"

cmake -S "$ROOT_DIR/zygisk/jni" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" --config Release -- -j4

cp -f "$BUILD_DIR/libpotatogfx.so" "$ROOT_DIR/zygisk/arm64-v8a.so"

rm -f "$OUT_ZIP"
cd "$ROOT_DIR"
zip -r9 "$OUT_ZIP" \
    module.prop \
    customize.sh \
    service.sh \
    uninstall.sh \
    config.json \
    webroot/ \
    zygisk/arm64-v8a.so

echo "[+] Done: $OUT_ZIP"
