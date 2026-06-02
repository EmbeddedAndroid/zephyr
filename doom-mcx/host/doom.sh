#!/bin/sh
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: gpl-2.0-only
#
# DOOM device-side orchestrator for the Arduino UNO Q (STM32U585). Runs ON the
# board's QCS Linux side; lives next to reflash.sh at /home/root/secureboot/.
#
# DOOM is a SINGLE 2 MB image flashed straight to 0x08000000 with NO mcuboot
# (the engine + WAD need the whole flash, so there is no room for an A/B layout).
# The OTA demo is the OTHER mode: mcuboot @0x08000000 + a signed bridge app in
# slot0, driven by reflash.sh. Only one firmware runs on the MCU at a time, so
# DOOM and the OTA stack co-exist as two flashable modes you switch between --
# this script never touches mcuboot's images, and `doom.sh demo` hands straight
# back to reflash.sh so OTA mode is always one command away.
#
#   doom.sh flash             flash the DOOM firmware -> 0x08000000 (no verify)
#   doom.sh input [args...]   run the SPI input bridge (reads key masks on stdin)
#   doom.sh play              flash DOOM, then tell you how to connect input
#   doom.sh demo              hand off to reflash.sh demo (restore A/B OTA mode)
#   doom.sh status            show staged images + input-bridge readiness
#
# After a flash it resets into flash (BOOT0 held low by the flasher) so DOOM
# runs immediately, booting straight into E1M1.

set -u

OO="${OO:-/home/root/zephyr-flash/oo}"
IMG="${IMG:-/home/root/secureboot}"
DOOM_BIN="${DOOM_BIN:-$IMG/doom.bin}"
DOOM_ADDR=0x08000000
CTL="${CTL:-/home/root/doom_ctl.py}"
WSAD="${WSAD:-/home/root/wsad_send.py}"
FAST="$OO/flash-fast.sh"
SLOW="$OO/flash.sh"
REFLASH="$IMG/reflash.sh"

say() { printf '[doom] %s\n' "$*"; }
die() { printf '[doom] ERROR: %s\n' "$*" >&2; exit 1; }

flash_doom() {
	[ -f "$DOOM_BIN" ] || die "DOOM image not found: $DOOM_BIN"
	if [ -x "$FAST" ]; then
		say "flashing DOOM (no-verify) $DOOM_BIN -> $DOOM_ADDR (~2 min, do not interrupt)"
		"$FAST" "$DOOM_BIN" "$DOOM_ADDR" || die "flash failed"
	elif [ -x "$SLOW" ]; then
		say "flash-fast.sh missing; using flash.sh (slower, full verify read-back)"
		"$SLOW" "$DOOM_BIN" "$DOOM_ADDR" || die "flash failed"
	else
		die "no flasher found in $OO"
	fi
	say "DOOM flashed + reset into flash. Booting E1M1; the SPI3 input slave is live."
}

input_bridge() {
	[ -f "$CTL" ] || die "controller not found: $CTL (push host/doom_ctl.py to the board)"
	command -v python3 >/dev/null 2>&1 || die "python3 not on device"
	[ -e /dev/spidev0.0 ] || say "warning: /dev/spidev0.0 missing; pass --bus/--dev"
	say "WASD move/turn, Space fire, E use, Q/X strafe, Esc quit."
	# Device python lacks termios -> set raw with stty, restore sane after.
	stty -echo -icanon min 1 time 0 2>/dev/null || true
	python3 -u "$CTL" "$@"
	stty sane 2>/dev/null || true
}

status() {
	say "mode images staged in $IMG:"
	for f in doom.bin mcuboot.bin bridge.signed.bin blink.signed.bin; do
		if [ -f "$IMG/$f" ]; then
			printf '  %-20s %s bytes\n' "$f" "$(wc -c < "$IMG/$f")"
		else
			printf '  %-20s (absent)\n' "$f"
		fi
	done
	printf '  flasher fast=%s slow=%s\n' \
		"$([ -x "$FAST" ] && echo yes || echo no)" \
		"$([ -x "$SLOW" ] && echo yes || echo no)"
	printf '  controller %s ; python3 %s ; /dev/spidev0.0 %s\n' \
		"$([ -f "$CTL" ] && echo present || echo MISSING)" \
		"$(command -v python3 >/dev/null 2>&1 && echo yes || echo no)" \
		"$([ -e /dev/spidev0.0 ] && echo yes || echo no)"
	printf '  reflash.sh (OTA mcuboot) %s\n' "$([ -x "$REFLASH" ] && echo present || echo absent)"
}

usage() {
	sed -n '15,21p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

case "${1:-}" in
flash) flash_doom ;;
input)
	shift
	input_bridge "$@"
	;;
play)
	flash_doom
	say "DOOM is running. Drive it from the laptop:  python3 host/wsad_doom.py"
	say "or play directly over an adb shell:  /home/root/secureboot/doom.sh input"
	;;
demo)
	[ -x "$REFLASH" ] || die "reflash.sh not found at $REFLASH (OTA images not staged?)"
	say "handing off to the A/B OTA mcuboot demo (reflash.sh demo)"
	exec "$REFLASH" demo
	;;
status) status ;;
"" | -h | --help | help) usage 0 ;;
*) die "unknown command '$1' (try: flash|input|play|demo|status)" ;;
esac
