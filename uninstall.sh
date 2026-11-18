#!/system/bin/sh
MODDIR=${0%/*}
CONFIG_FILE="$MODDIR/config.json"

if [ -f "$CONFIG_FILE" ]; then
    grep -o '"[a-zA-Z0-9_]*\.[a-zA-Z0-9_.]*"[[:space:]]*:[[:space:]]*{' "$CONFIG_FILE" | awk -F'"' '{print $2}' | while read -r PKG; do
        if [ -n "$PKG" ] && [ "$PKG" != "apps" ]; then
            cmd device_config delete game_overlay "$PKG" >/dev/null 2>&1
            cmd game reset "$PKG" >/dev/null 2>&1
        fi
    done
fi
