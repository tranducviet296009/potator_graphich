#!/system/bin/sh
SKIPUNZIP=0

ui_print "================================================"
ui_print "      PotatoGFX-KSU (Zygisk + Game Overlay)     "
ui_print "  Target: Tecno Pova 7 (ARM64 / Android 16 / HiOS 16)"
ui_print "================================================"

ARCH=$(getprop ro.product.cpu.abi)
if [ "$ARCH" != "arm64-v8a" ]; then
    abort "! Module chỉ hỗ trợ kiến trúc arm64-v8a (Hiện tại: $ARCH)"
fi

ANDROID_VER=$(getprop ro.build.version.release)
ANDROID_SDK=$(getprop ro.build.version.sdk)
HIOS_VER=$(getprop ro.tranos.version)
[ -z "$HIOS_VER" ] && HIOS_VER="HiOS 16"

ui_print "- Phát hiện hệ điều hành: Android $ANDROID_VER (SDK $ANDROID_SDK) - $HIOS_VER"
ui_print "- Thiết bị: $(getprop ro.product.model) ($(getprop ro.product.device))"

LIVE_MODDIR="/data/adb/modules/potatogfx_ksu"
if [ -f "$LIVE_MODDIR/config.json" ]; then
    ui_print "- Giữ nguyên cấu hình config.json hiện có của người dùng..."
    cp -af "$LIVE_MODDIR/config.json" "$MODPATH/config.json"
else
    ui_print "- Khởi tạo cấu hình config.json mặc định..."
fi

if [ ! -f "$MODPATH/zygisk/arm64-v8a.so" ]; then
    ui_print "! Cảnh báo: Chưa có zygisk/arm64-v8a.so (Hãy chạy script build trước khi flash)"
else
    ui_print "- Đã nạp thư viện Zygisk (16KB ELF Page-Aligned cho Android 16): zygisk/arm64-v8a.so"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/config.json" 0 0 0644

if [ -d "$MODPATH/zygisk" ]; then
    set_perm_recursive "$MODPATH/zygisk" 0 0 0755 0644
fi

ui_print "- Cài đặt hoàn tất! Mở KernelSU Manager -> WebUI để tùy chỉnh."
ui_print "================================================"
