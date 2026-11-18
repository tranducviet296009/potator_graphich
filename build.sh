#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build_arm64"
OUT_ZIP="$ROOT_DIR/PotatoGFX-KSU-v1.0.0-arm64.zip"

echo "===================================================="
echo "   PotatoGFX-KSU Automated NDK Builder (arm64-v8a)  "
echo "===================================================="

# Tự động tìm Android NDK nếu biến ANDROID_NDK_HOME chưa được đặt
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT/ndk" ]; then
        ANDROID_NDK_HOME="$(ls -d "$ANDROID_SDK_ROOT/ndk/"* 2>/dev/null | sort -V | tail -n 1)"
    elif [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME/ndk" ]; then
        ANDROID_NDK_HOME="$(ls -d "$ANDROID_HOME/ndk/"* 2>/dev/null | sort -V | tail -n 1)"
    fi
fi

if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "[!] Lỗi: Không tìm thấy ANDROID_NDK_HOME."
    echo "    Hãy cài đặt Android NDK (r25c / r26d / r27) và chạy:"
    echo "    export ANDROID_NDK_HOME=/duong/dan/toi/android-ndk"
    exit 1
fi

echo "[*] Sử dụng Android NDK: $ANDROID_NDK_HOME"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$ROOT_DIR/zygisk"

cmake -S "$ROOT_DIR/zygisk/jni" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SHADOWHOOK=ON

cmake --build "$BUILD_DIR" --config Release -- -j"$(nproc 2>/dev/null || echo 4)"

if [ ! -f "$BUILD_DIR/libpotatogfx.so" ]; then
    echo "[!] Lỗi: Không tìm thấy file $BUILD_DIR/libpotatogfx.so sau khi biên dịch!"
    exit 1
fi

# Chuẩn cấu trúc Zygisk của KernelSU + ZygiskNext: đặt file tại zygisk/arm64-v8a.so
cp -f "$BUILD_DIR/libpotatogfx.so" "$ROOT_DIR/zygisk/arm64-v8a.so"
echo "[*] Đã xuất thư viện Zygisk: $ROOT_DIR/zygisk/arm64-v8a.so"

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

echo "===================================================="
echo "[+] Đóng gói thành công: $OUT_ZIP"
echo "    Flash trực tiếp file này trong KernelSU Manager!"
echo "===================================================="
