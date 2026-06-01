#!/bin/sh
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Reflash the UNO Q STM32U585 to a known state over SWD. Runs ON the board's
# QCS Linux side (lives at /home/root/secureboot/reflash.sh).
#
# It wraps the openocd flasher in /home/root/zephyr-flash/oo and the staged
# images in /home/root/secureboot. Pick a target:
#
#   reflash.sh demo      restore the A/B OTA demo: mcuboot @0x08000000 +
#                        bridge (signed) @0x08010000 (slot0). Leaves the board
#                        running the SPI bridge with mcuboot underneath so OTA
#                        works again.
#   reflash.sh blink     demo bootloader + OTA-capable blink in slot0 (board
#                        boots blinking; OTA it up to the bridge).
#   reflash.sh display   the plain PR display sample (no bootloader) @0x08000000.
#                        Overwrites mcuboot. This is the upstream driver image.
#   reflash.sh mcuboot   (re)flash just the bootloader @0x08000000.
#   reflash.sh slot0 <img.signed.bin>   flash a signed app to slot0 (0x08010000).
#   reflash.sh raw   <img.bin> <addr>   flash any bin to any address.
#
# After flashing it resets into flash (BOOT0 held low) so the image runs.

set -u

OO="${OO:-/home/root/zephyr-flash/oo}"
IMG="${IMG:-/home/root/secureboot}"
FLASH="$OO/flash.sh"
RECOVER="$OO/recover.sh"
MCUBOOT_ADDR=0x08000000
SLOT0_ADDR=0x08010000

say() { printf '[reflash] %s\n' "$*"; }
die() { printf '[reflash] ERROR: %s\n' "$*" >&2; exit 1; }

[ -x "$FLASH" ] || die "flasher not found at $FLASH"

flash() {
    img="$1"; addr="$2"
    [ -f "$img" ] || die "image not found: $img"
    say "flashing $img -> $addr"
    "$FLASH" "$img" "$addr" || die "flash failed ($img -> $addr)"
}

reset_run() {
    say "resetting into flash"
    [ -x "$RECOVER" ] && "$RECOVER" >/dev/null 2>&1 || true
}

usage() {
    sed -n '5,23p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

target="${1:-}"
case "$target" in
    demo)
        flash "$IMG/mcuboot.bin"        "$MCUBOOT_ADDR"
        flash "$IMG/bridge.signed.bin"  "$SLOT0_ADDR"
        reset_run
        say "done: mcuboot + bridge (slot0). OTA is available again."
        ;;
    blink)
        flash "$IMG/mcuboot.bin"        "$MCUBOOT_ADDR"
        flash "$IMG/blink.signed.bin"   "$SLOT0_ADDR"
        reset_run
        say "done: mcuboot + OTA-capable blink (slot0). Panel should blink."
        ;;
    display)
        flash "$IMG/displaytest.bin"    "$MCUBOOT_ADDR"
        reset_run
        say "done: plain PR display sample @ $MCUBOOT_ADDR (no bootloader)."
        ;;
    mcuboot)
        flash "$IMG/mcuboot.bin"        "$MCUBOOT_ADDR"
        reset_run
        say "done: bootloader flashed. slot0 must hold a valid signed image."
        ;;
    slot0)
        img="${2:-}"; [ -n "$img" ] || die "usage: reflash.sh slot0 <img.signed.bin>"
        case "$img" in /*) : ;; *) img="$IMG/$img" ;; esac
        flash "$img" "$SLOT0_ADDR"
        reset_run
        say "done: $img -> slot0."
        ;;
    raw)
        img="${2:-}"; addr="${3:-}"
        [ -n "$img" ] && [ -n "$addr" ] || die "usage: reflash.sh raw <img.bin> <addr>"
        case "$img" in /*) : ;; *) img="$IMG/$img" ;; esac
        flash "$img" "$addr"
        reset_run
        say "done: $img -> $addr."
        ;;
    ""|-h|--help|help)
        usage 0
        ;;
    *)
        die "unknown target '$target' (try: demo blink display mcuboot slot0 raw, or --help)"
        ;;
esac
