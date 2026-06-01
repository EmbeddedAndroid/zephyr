#!/bin/sh
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# One-shot A/B OTA cycle demo for the Arduino UNO Q (STM32U585). Runs ON the
# board's QCS Linux side (push it there, or drive via `adb shell`).
#
# It alternates the two firmwares over the in-band SPI bridge, OTA'ing from one
# to the other every DWELL seconds, forever (Ctrl-C to stop):
#
#   blink  : the matrix blinks on its own  (image: blink.signed.bin,  ver "...BLINK")
#   bridge : Linux drives scrolling text   (image: bridge.signed.bin, ver "...BRIDGE")
#
# Both images carry the SPI + OTA receiver, so each can be updated to the other.
# OTAs are PERMANENT swaps here (the new image runs and stays); the cycle just
# OTAs back the other way next round.
#
# What you SEE each round:
#   - blink phase : panel blinks fully on/off by itself for DWELL seconds.
#   - bridge phase: host scrolls the running version across the panel for DWELL
#                   seconds, then OTAs back to blink.
#
# Requirements on the device (already staged by the demo setup):
#   /home/root/bridge_send.py
#   /home/root/secureboot/blink.signed.bin
#   /home/root/secureboot/bridge.signed.bin
#   /home/root/zephyr-flash/oo/   (openocd bundle, for the reset helper)

set -u

SENDER="${SENDER:-/home/root/bridge_send.py}"
IMG_DIR="${IMG_DIR:-/home/root/secureboot}"
BLINK="${BLINK:-$IMG_DIR/blink.signed.bin}"
BRIDGE="${BRIDGE:-$IMG_DIR/bridge.signed.bin}"
OO="${OO:-/home/root/zephyr-flash/oo}"
DWELL="${DWELL:-30}"      # seconds to dwell in each app before OTA'ing onward
BOOT_WAIT="${BOOT_WAIT:-8}" # seconds to let MCUboot swap + the new app boot
ROUNDS="${ROUNDS:-0}"     # number of blink->bridge rounds; 0 = loop forever

say() { printf '\n=== %s ===\n' "$*"; }

for f in "$SENDER" "$BLINK" "$BRIDGE"; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

# Read the running app's version string over the bridge ("" if it can't answer).
# Retries a few times: just after an OTA swap the new app's SPI slave may not be
# armed yet for the first transfer or two, which would otherwise read as blank.
read_ver() {
    python3 - "$SENDER" <<'PY' 2>/dev/null
import sys, time, importlib.util
spec = importlib.util.spec_from_file_location("bs", sys.argv[1])
b = importlib.util.module_from_spec(spec); spec.loader.exec_module(b)
v = ""
for attempt in range(6):
    try:
        spi = b.RawSpi(); spi.max_speed_hz = 2000000; spi.open(0, 0); seq = 1
        for _ in range(2):
            spi.xfer2(b.make_block(b.TYPE_CMD, seq, bytes([b.CMD["query"], 0]))); seq += 1
        v = b.query_zephyr_version(spi, seq) or ""
        spi.close()
    except Exception:
        v = ""
    if v:
        break
    time.sleep(0.5)
print(v)
PY
}

# OTA the given signed image (permanent), then wait for the swap+boot.
ota() {
    img="$1"
    python3 "$SENDER" --delay 0 ota "$img" --permanent --chunk 48 \
        2>&1 | grep -iE "streaming done|ota_err|RDY stuck" | tail -1
    sleep "$BOOT_WAIT"
}

# Scroll the running app's version across the panel for ~DWELL seconds.
show_bridge() {
    python3 "$SENDER" --delay 0.05 versions &
    pid=$!
    sleep "$DWELL"
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

trap 'echo; echo "stopping."; kill 0 2>/dev/null; exit 0' INT TERM

if [ "$ROUNDS" -eq 0 ]; then
    say "OTA cycle demo (dwell ${DWELL}s/app, loop forever, Ctrl-C to stop)"
else
    say "OTA cycle demo (dwell ${DWELL}s/app, ${ROUNDS} round(s) then stop)"
fi
ver="$(read_ver)"
echo "currently running: ${ver:-<no answer>}"

round=0
while true; do
    round=$((round + 1))

    # --- ensure we start each cycle on blink ---
    ver="$(read_ver)"
    case "$ver" in
        *BLINK*) : ;;                       # already blinking
        *) say "round $round: OTA -> blink"
           ota "$BLINK"
           ver="$(read_ver)"
           echo "now: ${ver:-<no answer>}" ;;
    esac

    say "round $round: BLINK app running for ${DWELL}s (panel blinks itself)"
    sleep "$DWELL"

    # --- OTA up to the bridge ---
    say "round $round: OTA blink -> bridge"
    ota "$BRIDGE"
    ver="$(read_ver)"
    echo "now: ${ver:-<no answer>}"

    say "round $round: BRIDGE app running for ${DWELL}s (Linux scrolls version)"
    show_bridge

    if [ "$ROUNDS" -ne 0 ] && [ "$round" -ge "$ROUNDS" ]; then
        say "done ($round round(s))"
        break
    fi
done
