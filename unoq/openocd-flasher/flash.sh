#!/bin/sh
# Flash a firmware .bin to the UNO Q STM32U585 over SWD and verify it.
#
# Usage:  flash.sh <firmware.bin> [flash_addr]
#   firmware.bin  path (inside the container) to the image to flash
#   flash_addr    flash base, default 0x08000000 (whole-firmware slot used
#                 by mainline Zephyr, replacing the Arduino loader)
#
# Mainline Zephyr boots straight from reset, so no BOOT0-LOW hold is needed
# here (that dance is only for re-entering the Arduino loader). We just
# write+verify+reset.
#
# Exit code is openocd's: 0 = flashed and verified.
set -eu

FW="${1:?usage: flash.sh <firmware.bin> [flash_addr]}"
ADDR="${2:-0x08000000}"
CFG="/cfg/unoq-swd.cfg"

if [ ! -f "$FW" ]; then
    echo "flash.sh: firmware not found: $FW" >&2
    exit 2
fi

echo "flash.sh: flashing $FW -> $ADDR (sha256 $(sha256sum "$FW" | cut -d' ' -f1))"

exec openocd -f "$CFG" \
    -c "init" \
    -c "halt" \
    -c "flash write_image erase $FW $ADDR bin" \
    -c "verify_image $FW $ADDR bin" \
    -c "reset run" \
    -c "shutdown"
