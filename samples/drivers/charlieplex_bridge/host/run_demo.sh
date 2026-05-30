#!/bin/sh
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Self-contained on-device demo for the charlieplex SPI bridge. Run it on the
# UNO Q QCS Linux side (e.g. over adb):
#
#   adb shell /home/root/run_demo.sh
#
# It (1) flashes the staged bridge firmware to the STM32 via the zephyr-flash
# path-unit, (2) waits for the flash to verify, then (3) drives the live kernel
# version scrolling across the LED matrix in a loop (Ctrl-C to stop).
#
# Paths are overridable via env vars.

set -u

FW_SRC="${FW_SRC:-/home/root/firmware/charlieplex_bridge.bin}"
FLASH_DIR="${FLASH_DIR:-/home/root/zephyr-flash}"
INCOMING="$FLASH_DIR/incoming.bin"
STATUS="$FLASH_DIR/status.json"
SENDER="${SENDER:-/home/root/bridge_send.py}"
DELAY="${DELAY:-0.07}"

say() { printf '[demo] %s\n' "$*"; }

[ -f "$FW_SRC" ] || { say "ERROR: firmware not found: $FW_SRC"; exit 1; }
[ -f "$SENDER" ] || { say "ERROR: sender not found: $SENDER"; exit 1; }

WANT_SHA="$(sha256sum "$FW_SRC" | cut -d' ' -f1)"
say "firmware $FW_SRC"
say "sha256   $WANT_SHA"

# --- 1. flash: copying to incoming.bin triggers zephyr-flash.path -------------
say "flashing MCU (copy -> $INCOMING) ..."
rm -f "$STATUS"
cp "$FW_SRC" "$INCOMING"

# --- 2. wait for the flasher to verify ---------------------------------------
ok=0
i=0
while [ "$i" -lt 30 ]; do
    if [ -f "$STATUS" ]; then
        if grep -q '"ok":true' "$STATUS" && grep -q "$WANT_SHA" "$STATUS"; then
            ok=1
            break
        fi
        # status written but not ok / sha mismatch -> report and stop
        if grep -q '"ok":false' "$STATUS"; then
            say "ERROR: flash failed:"; cat "$STATUS"; exit 1
        fi
    fi
    i=$((i + 1))
    sleep 1
done

[ "$ok" -eq 1 ] || { say "ERROR: flash did not verify in time:"; cat "$STATUS" 2>&1; exit 1; }
say "flash verified OK"

# Give the MCU a moment to boot into the bridge and arm the SPI slave.
sleep 2

# --- 3. drive the versions scrolling on loop ---------------------------------
# Alternates the Linux kernel release (read here) with the MCU's Zephyr version
# (queried over SPI from the firmware).
say "driving versions on the matrix (LINUX $(uname -r) <-> Zephyr)"
say "(Ctrl-C to stop)"
exec python3 "$SENDER" --delay "$DELAY" versions
