#!/bin/sh
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Flash the MCUboot secure-boot pair to the UNO Q STM32U585 over SWD.
# Run on the QCS Linux side (over adb):
#
#   adb shell /home/root/flash_secureboot.sh
#
# Stage these two files first (e.g. via adb push):
#   $BOOT_BIN = MCUboot bootloader  -> flashed at 0x08000000
#   $APP_BIN  = signed slot0 image  -> flashed at 0x08010000 (image-0)
#
# MCUboot validates the ECDSA-P256 signature of the slot0 image at boot and
# only chain-loads it if the signature is valid. The native openocd flasher
# (oo/flash.sh) takes an explicit flash address, so we drive it twice.

set -eu

OO="${OO:-/home/root/zephyr-flash/oo/flash.sh}"
BOOT_BIN="${BOOT_BIN:-/home/root/secureboot/mcuboot.bin}"
APP_BIN="${APP_BIN:-/home/root/secureboot/app.signed.bin}"
BOOT_ADDR="${BOOT_ADDR:-0x08000000}"
SLOT0_ADDR="${SLOT0_ADDR:-0x08010000}"

say() { printf '[secureboot] %s\n' "$*"; }

[ -x "$OO" ]       || { say "ERROR: flasher not found: $OO"; exit 1; }
[ -f "$BOOT_BIN" ] || { say "ERROR: bootloader not found: $BOOT_BIN"; exit 1; }
[ -f "$APP_BIN" ]  || { say "ERROR: signed app not found: $APP_BIN"; exit 1; }

say "1/2 flashing MCUboot -> $BOOT_ADDR"
"$OO" "$BOOT_BIN" "$BOOT_ADDR"

say "2/2 flashing signed app -> $SLOT0_ADDR (slot0/image-0)"
"$OO" "$APP_BIN" "$SLOT0_ADDR"

say "done. MCUboot will verify the slot0 signature and boot the app."
