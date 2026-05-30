#!/bin/sh
# Triggered by zephyr-flash.path when /home/root/zephyr-flash/incoming.bin
# changes. Flashes it via the native openocd bundle and records a status file
# that can be pulled with adb.
#
# Layout (on device):
#   /home/root/zephyr-flash/incoming.bin   <- adb push target (the watch file)
#   /home/root/zephyr-flash/oo/flash.sh    <- native flasher bundle
#   /home/root/zephyr-flash/status.json    <- result of last flash (adb pull)
#   /home/root/zephyr-flash/last-flash.log <- full openocd output of last flash
set -u

FLASH_DIR=/home/root/zephyr-flash
INCOMING="$FLASH_DIR/incoming.bin"
OO="$FLASH_DIR/oo"
ADDR=0x08000000
STATUS="$FLASH_DIR/status.json"
LOG="$FLASH_DIR/last-flash.log"

# No timestamp source guaranteed; use kernel monotonic + boot id for ordering.
stamp() { cat /proc/uptime 2>/dev/null | cut -d' ' -f1; }

if [ ! -f "$INCOMING" ]; then
    printf '{"ok":false,"error":"no incoming.bin","uptime":%s}\n' "$(stamp)" > "$STATUS"
    exit 1
fi

SHA=$(sha256sum "$INCOMING" | cut -d' ' -f1)

# Run the flasher; capture everything.
"$OO/flash.sh" "$INCOMING" "$ADDR" > "$LOG" 2>&1
RC=$?

if [ "$RC" -eq 0 ] && grep -q '^verified ' "$LOG"; then
    printf '{"ok":true,"sha256":"%s","addr":"%s","uptime":%s}\n' "$SHA" "$ADDR" "$(stamp)" > "$STATUS"
else
    printf '{"ok":false,"sha256":"%s","rc":%d,"uptime":%s,"see":"last-flash.log"}\n' "$SHA" "$RC" "$(stamp)" > "$STATUS"
fi
exit "$RC"
