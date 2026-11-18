#!/system/bin/sh
MODDIR=${0%/*}
CONFIG_FILE="$MODDIR/config.json"
LOG_FILE="$MODDIR/service.log"

log_info() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$LOG_FILE"
}

apply_from_config() {
    if [ ! -f "$CONFIG_FILE" ]; then
        log_info "Không tìm thấy $CONFIG_FILE"
        return 1
    fi

    # Đọc trạng thái global_enabled
    GLOBAL_EN=$(grep -o '"global_enabled"[[:space:]]*:[[:space:]]*[a-z]*' "$CONFIG_FILE" | head -n 1 | awk -F':' '{print $2}' | tr -d ' "')
    if [ "$GLOBAL_EN" = "false" ]; then
        log_info "PotatoGFX đang tắt toàn cục (global_enabled=false)."
        return 0
    fi

    # Duyệt từng block cấu hình package trong config.json bằng awk chuẩn POSIX (busybox awk trên Android 16)
    awk '
    /"apps"[[:space:]]*:/ { in_apps=1; next }
    in_apps {
        if ($0 ~ /"[^"]+"[[:space:]]*:[[:space:]]*\{/) {
            match($0, /"[^"]+"/)
            pkg = substr($0, RSTART+1, RLENGTH-2)
            if (pkg != "apps") {
                cur_pkg = pkg
                enabled = "false"
                downscale = "1.0"
                fps = "120"
            }
        }
        if (cur_pkg != "") {
            if ($0 ~ /"enabled"[[:space:]]*:/) {
                val = $0
                sub(/.*:[[:space:]]*/, "", val)
                sub(/[,}[:space:]].*/, "", val)
                enabled = val
            }
            if ($0 ~ /"downscale_factor"[[:space:]]*:/) {
                val = $0
                sub(/.*:[[:space:]]*/, "", val)
                sub(/[,}[:space:]].*/, "", val)
                downscale = val
            }
            if ($0 ~ /"target_fps"[[:space:]]*:/) {
                val = $0
                sub(/.*:[[:space:]]*/, "", val)
                sub(/[,}[:space:]].*/, "", val)
                fps = val
            }
            if ($0 ~ /\}/) {
                print cur_pkg, enabled, downscale, fps
                cur_pkg = ""
            }
        }
    }
    ' "$CONFIG_FILE" | while read -r PKG EN_VAL SCALE_VAL FPS_VAL; do
        [ -z "$PKG" ] && continue
        if [ "$EN_VAL" = "true" ]; then
            # Trên Android 16 (API 36) / HiOS 16: Khóa đồng bộ game_overlay & cmd game set
            if [ "$SCALE_VAL" = "1" ] || [ "$SCALE_VAL" = "1.0" ]; then
                cmd device_config put game_overlay "$PKG" "mode=2,fps=$FPS_VAL:mode=3,fps=$FPS_VAL" >/dev/null 2>&1
                cmd game set --mode 2 --fps "$FPS_VAL" "$PKG" >/dev/null 2>&1
            else
                cmd device_config put game_overlay "$PKG" "mode=2,fps=$FPS_VAL,downscaleFactor=$SCALE_VAL:mode=3,fps=$FPS_VAL,downscaleFactor=$SCALE_VAL" >/dev/null 2>&1
                cmd game set --mode 2 --fps "$FPS_VAL" --downscale "$SCALE_VAL" "$PKG" >/dev/null 2>&1
            fi
            cmd game mode performance "$PKG" >/dev/null 2>&1
            log_info "Đã áp dụng Android 16 Game Overlay cho $PKG: mode=2, fps=$FPS_VAL, downscaleFactor=$SCALE_VAL"
        else
            cmd device_config delete game_overlay "$PKG" >/dev/null 2>&1
            cmd game reset "$PKG" >/dev/null 2>&1
            log_info "Đã khôi phục mặc định Game Overlay cho $PKG"
        fi
    done
}

# Nếu gọi trực tiếp từ WebUI với tham số --apply-now thì thực thi ngay không chờ boot
if [ "$1" = "--apply-now" ]; then
    apply_from_config
    exit 0
fi

# Chờ hệ thống Android 16 / HiOS 16 khởi động hoàn tất
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 2
done
sleep 5

echo "=== PotatoGFX-KSU Service Started (Android 16 / HiOS 16) ===" > "$LOG_FILE"
# Khoá chống tự động ghi đè cấu hình device_config từ GMS trên Android 16
cmd device_config set_sync_disabled_for_tests persistent >/dev/null 2>&1
apply_from_config
