#!/usr/bin/env python3
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: gpl-2.0-only
#
# DOOM controller launcher (laptop side). Pushes the device-side controller
# (doom_ctl.py) to the UNO Q and hands your terminal to it over `adb shell`, so
# your keyboard drives DOOM on the STM32.
#
# The capture + SPI both run ON the device: `adb exec-out` does not forward
# stdin (the old split design sent only zeros and nothing moved), but `adb
# shell` allocates a PTY and forwards the keyboard. So this launcher just does
# the push and exec -- all the real work is in doom_ctl.py.
#
#   python3 wsad_doom.py
#
# Controls: WASD move/turn (A/D = look around), Space fire, E use, Q/X strafe,
# Esc quits. Extra args are passed through to doom_ctl.py (e.g. --hold 300).

import argparse
import os
import subprocess
import sys

DEF_ADB = "/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"


def main():
    ap = argparse.ArgumentParser(description="DOOM WASD launcher (laptop side)")
    ap.add_argument("--adb", default=DEF_ADB)
    ap.add_argument("--local",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "doom_ctl.py"))
    ap.add_argument("--remote", default="/home/root/doom_ctl.py")
    ap.add_argument("--no-push", action="store_true")
    args, passthrough = ap.parse_known_args()

    if not args.no_push:
        sys.stderr.write("Pushing doom_ctl.py to the board...\n")
        subprocess.run([args.adb, "push", args.local, args.remote], check=True)

    # CRITICAL: `adb shell <cmd>` runs WITHOUT a PTY by default, which leaves your
    # laptop terminal in cooked mode -- keys echo locally and only send on Enter,
    # and `stty` on the device has no tty to configure. `-t` forces adb to
    # allocate a PTY (and put your local terminal in raw mode), so keystrokes go
    # straight through. The device python lacks termios, so we still disable the
    # remote PTY's echo/line-buffering with `stty` (restored to sane on exit).
    extra = " ".join(passthrough)
    remote_cmd = (
        "stty -echo -icanon min 1 time 0 2>/dev/null; "
        "python3 -u %s %s; "
        "stty sane 2>/dev/null" % (args.remote, extra)
    )
    cmd = [args.adb, "shell", "-t", remote_cmd]
    sys.stderr.write("Launching controller on the board (Esc to quit)...\n")
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    main()
