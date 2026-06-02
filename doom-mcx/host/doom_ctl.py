#!/usr/bin/env python3
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: gpl-2.0-only
#
# DOOM controller, DEVICE side (runs ON the UNO Q's QCS Linux). Captures WASD +
# space from its controlling terminal and clocks key-state blocks to the STM32
# DOOM firmware over SPI3 -- all in one process, no host<->device pipe.
#
# Run it over an adb shell so your laptop terminal IS its terminal:
#     adb shell python3 -u /home/root/doom_ctl.py
# (the laptop launcher host/wsad_doom.py pushes this file and execs that for you).
#
# Why device-side: `adb exec-out` does NOT forward stdin, so the older split
# design (laptop capture -> adb exec-out -> device sender) sent only heartbeat
# zeros and nothing moved. `adb shell` allocates a PTY and DOES forward the
# keyboard, so capturing here -- next to the SPI master -- is both simpler and
# correct.
#
#   W / S    move forward / back     (key_up   / key_down)
#   A / D    turn left / right       (key_left / key_right)   <- "look around"
#   Space    fire                    (key_fire)
#   E        use / open doors        (key_use)
#   Q / X    strafe left / right
#   Esc or Ctrl-C   quit (releases all keys)
#
# No external deps (stock qcom-distro image): raw spidev + GPIO cdev v2 ioctls.

# NOTE: the stock qcom-distro python3 is a minimal build WITHOUT the termios/tty
# modules, so raw/cbreak mode is set externally with `stty` (the launcher, or
# `doom.sh input`, runs `stty -echo -icanon min 1 time 0` before us). We only do
# non-blocking reads here, which need no termios.
import argparse
import array
import binascii
import fcntl
import os
import select
import struct
import sys
import time

# ---- raw spidev (master) ----------------------------------------------------
SPI_IOC_MAGIC = ord("k")


def _iow(t, nr, size):
    return (1 << 30) | (size << 16) | (t << 8) | nr


class RawSpi:
    def __init__(self, speed=2000000, mode=0):
        self.max_speed_hz = speed
        self.mode = mode

    def open(self, bus, dev):
        self.fd = open("/dev/spidev%d.%d" % (bus, dev), "r+b", buffering=0)
        fcntl.ioctl(self.fd, _iow(SPI_IOC_MAGIC, 1, 1), struct.pack("=B", self.mode))
        fcntl.ioctl(self.fd, _iow(SPI_IOC_MAGIC, 3, 1), struct.pack("=B", 8))
        fcntl.ioctl(self.fd, _iow(SPI_IOC_MAGIC, 4, 4),
                    struct.pack("=I", self.max_speed_hz))

    def xfer(self, data):
        n = len(data)
        txbuf = array.array("B", data)
        rxbuf = array.array("B", [0] * n)
        msg = struct.pack("QQIIHBBBBBB", txbuf.buffer_info()[0],
                          rxbuf.buffer_info()[0], n, self.max_speed_hz,
                          0, 8, 0, 0, 0, 0, 0)
        fcntl.ioctl(self.fd, _iow(SPI_IOC_MAGIC, 0, 32), msg)
        return list(rxbuf)


# ---- RDY line via GPIO cdev v2 ----------------------------------------------
class GpioLine:
    GPIO_V2_LINE_FLAG_INPUT = 1 << 2

    def _ioc(self, d, t, nr, size):
        return (d << 30) | (size << 16) | (t << 8) | nr

    def __init__(self, chip, offset, consumer=b"doomctl"):
        self.cfd = open("/dev/%s" % chip, "r+b", buffering=0)
        offsets = struct.pack("<64I", offset, *([0] * 63))
        cons = consumer[:31].ljust(32, b"\x00")
        cfg = struct.pack("<QI", self.GPIO_V2_LINE_FLAG_INPUT, 0) + b"\x00" * 260
        tail = struct.pack("<II", 1, 0) + b"\x00" * 20 + struct.pack("<i", 0)
        buf = array.array("B", bytearray(offsets + cons + cfg + tail))
        fcntl.ioctl(self.cfd, self._ioc(3, 0xB4, 0x07, 592), buf, True)
        self.lfd = struct.unpack_from("<i", buf, 588)[0]
        self._get = self._ioc(3, 0xB4, 0x0E, 16)

    def get(self):
        vbuf = array.array("B", struct.pack("<QQ", 0, 1))
        fcntl.ioctl(self.lfd, self._get, vbuf, True)
        return struct.unpack_from("<Q", vbuf, 0)[0] & 1

    def wait_high(self, timeout=0.02):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.get():
                return True
        return False


# ---- protocol ---------------------------------------------------------------
BLOCK_SIZE = 64
CRC_OFFSET = BLOCK_SIZE - 2
MAGIC = 0xA5
VERSION = 0x01
TYPE_KEYS = 0x20

BIT = {
    "w": 1 << 0, "s": 1 << 1, "a": 1 << 2, "d": 1 << 3,
    " ": 1 << 4, "e": 1 << 5, "q": 1 << 6, "x": 1 << 7,
}


def make_keys_block(seq, mask):
    blk = bytearray(BLOCK_SIZE)
    blk[0] = MAGIC
    blk[1] = VERSION
    blk[2] = TYPE_KEYS
    blk[3] = seq & 0xFF
    blk[4] = 1
    blk[5] = mask & 0xFF
    crc = binascii.crc_hqx(bytes(blk[:CRC_OFFSET]), 0xFFFF)
    blk[CRC_OFFSET] = crc & 0xFF
    blk[CRC_OFFSET + 1] = (crc >> 8) & 0xFF
    return list(blk)


def main():
    ap = argparse.ArgumentParser(description="DOOM controller (device side)")
    ap.add_argument("--bus", type=int, default=0)
    ap.add_argument("--dev", type=int, default=0)
    ap.add_argument("--speed", type=int, default=2000000)
    ap.add_argument("--rdy-chip", default="gpiochip1")
    ap.add_argument("--rdy-line", type=int, default=70)
    ap.add_argument("--hz", type=float, default=60.0)
    ap.add_argument("--hold", type=int, default=220,
                    help="ms a key stays 'held' after its last keystroke")
    ap.add_argument("--no-rdy", action="store_true")
    args = ap.parse_args()

    spi = RawSpi(speed=args.speed)
    spi.open(args.bus, args.dev)
    rdy = None if args.no_rdy else GpioLine(args.rdy_chip, args.rdy_line)

    sys.stderr.write(
        "DOOM controls: WASD move/turn, Space fire, E use, Q/X strafe, Esc quit.\n")
    sys.stderr.flush()

    period = 1.0 / args.hz
    hold = args.hold / 1000.0
    held_until = {}
    seq = 0
    last_mask = -1
    fd = sys.stdin.fileno()

    try:
        while True:
            r, _, _ = select.select([fd], [], [], period)
            now = time.monotonic()
            if r:
                data = os.read(fd, 64)
                if data == b"":
                    break
                for ch in data.decode("latin-1", "ignore"):
                    if ch in ("\x1b", "\x03"):  # Esc / Ctrl-C
                        return
                    lc = ch.lower()
                    if lc in BIT:
                        held_until[lc] = now + hold

            mask = 0
            for ch, deadline in list(held_until.items()):
                if deadline > now:
                    mask |= BIT[ch]
                else:
                    del held_until[ch]

            if mask != last_mask or (seq & 0x3F) == 0:
                last_mask = mask
            if rdy is not None:
                rdy.wait_high(0.02)
            try:
                spi.xfer(make_keys_block(seq, mask))
            except OSError:
                time.sleep(0.005)
            seq = (seq + 1) & 0xFF
    finally:
        try:
            if rdy is not None:
                rdy.wait_high(0.02)
            spi.xfer(make_keys_block(seq, 0))  # release everything
        except Exception:
            pass


if __name__ == "__main__":
    main()
