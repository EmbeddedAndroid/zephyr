#!/bin/sh
# Fast (no-verify) flasher for the UNO Q STM32U585 over SWD.
# Same as the stock oo/flash.sh but:
#   - "reset halt" instead of bare "halt" -> clean target state, avoids the
#     "target was in unknown state when halt was requested" stall.
#   - drops verify_image (the 1.9MB read-back over SWD @15KiB/s that wedges).
# Usage: flash-fast.sh <firmware.bin> [flash_addr]
set -eu
HERE=/home/root/zephyr-flash/oo
FW="${1:?usage: flash-fast.sh <firmware.bin> [flash_addr]}"
ADDR="${2:-0x08000000}"
CFG="$HERE/unoq-swd.cfg"
[ -f "$FW" ] || { echo "flash-fast.sh: firmware not found: $FW" >&2; exit 2; }

export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}"
OPENOCD="$HERE/bin/openocd"
GPIOSET="$HERE/bin/gpioset"
SCRIPTS="$HERE/share/openocd/scripts"

echo "flash-fast.sh: flashing $FW -> $ADDR (sha256 $(sha256sum "$FW" | cut -d' ' -f1))"

# Hold BOOT0 (gpiochip1:37 / PH3) LOW for the whole write + reset.
"$GPIOSET" -c gpiochip1 37=0 &
BOOT0_PID=$!
sleep 0.3
trap 'kill "$BOOT0_PID" 2>/dev/null || true' EXIT INT TERM

"$OPENOCD" -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "flash write_image erase $FW $ADDR bin" \
    -c "reset run" \
    -c "shutdown"
RC=$?

kill "$BOOT0_PID" 2>/dev/null || true
wait "$BOOT0_PID" 2>/dev/null || true
echo "flash-fast.sh: done rc=$RC"
exit "$RC"
