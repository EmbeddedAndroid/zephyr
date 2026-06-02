#!/usr/bin/env python3
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: gpl-2.0-only
#
# DOOM input forwarder, QCS-Linux (SoC) side. Runs as the SPI master on
# /dev/spidevN.M and clocks TYPE_KEYS blocks to the STM32 DOOM firmware (SPI3
# slave). It reads a key bitmask from stdin -- one raw byte per update -- and
# resends the current bitmask continuously at a fixed rate so held keys survive
# any dropped SPI block. RDY (gpiochip1:70) is honoured exactly like the
# charlieplex bridge: only clock a block while the slave is armed.
#
# This is the on-device half. Drive it from the laptop with wsad_doom.py, which
# captures WASD/space and pipes bitmask bytes into this script's stdin over adb.
#
# No external deps (the stock qcom-distro image has no python-spidev / libgpiod
# bindings): raw spidev + GPIO cdev v2 ioctls, lifted from bridge_send.py.

import argparse
import array
import binascii
import fcntl
import os
import select
import struct
import sys
import time

SPI_IOC_MAGIC = ord("k")


def _iow(t, nr, size):
    return (1 << 30) | (size << 16) | (t << 8) | nr


SPI_IOC_WR_MODE = _iow(SPI_IOC_MAGIC, 1, 1)
SPI_IOC_WR_BITS_PER_WORD = _iow(SPI_IOC_MAGIC, 3, 1)
SPI_IOC_WR_MAX_SPEED_HZ = _iow(SPI_IOC_MAGIC, 4, 4)


def _spi_message(nr):
    return _iow(SPI_IOC_MAGIC, 0, nr * 32)


class RawSpi:
    def __init__(self, speed=2000000, mode=0):
        self.fd = None
        self.max_speed_hz = speed
        self.mode = mode

    def open(self, bus, dev):
        self.fd = open("/dev/spidev%d.%d" % (bus, dev), "r+b", buffering=0)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, struct.pack("=B", self.mode))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack("=B", 8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ,
                    struct.pack("=I", self.max_speed_hz))

    def xfer(self, data):
        n = len(data)
        txbuf = array.array("B", data)
        rxbuf = array.array("B", [0] * n)
        tx_addr, _ = txbuf.buffer_info()
        rx_addr, _ = rxbuf.buffer_info()
        msg = struct.pack("QQIIHBBBBBB", tx_addr, rx_addr, n,
                          self.max_speed_hz, 0, 8, 0, 0, 0, 0, 0)
        fcntl.ioctl(self.fd, _spi_message(1), msg)
        return list(rxbuf)


class GpioLine:
    """Read a single GPIO line via the cdev v2 uAPI -- honour the RDY handshake."""

    GPIO_V2_LINE_FLAG_INPUT = 1 << 2

    def _ioc(self, d, t, nr, size):
        return (d << 30) | (size << 16) | (t << 8) | nr

    def __init__(self, chip, offset, consumer=b"doominput"):
        self.cfd = open("/dev/%s" % chip, "r+b", buffering=0)
        offsets = struct.pack("<64I", offset, *([0] * 63))
        cons = consumer[:31].ljust(32, b"\x00")
        cfg = (struct.pack("<QI", self.GPIO_V2_LINE_FLAG_INPUT, 0)
               + b"\x00" * 20 + b"\x00" * 240)
        tail = (struct.pack("<II", 1, 0) + b"\x00" * 20 + struct.pack("<i", 0))
        req = bytearray(offsets + cons + cfg + tail)
        assert len(req) == 592, len(req)
        GPIO_V2_GET_LINE_IOCTL = self._ioc(3, 0xB4, 0x07, 592)
        buf = array.array("B", req)
        fcntl.ioctl(self.cfd, GPIO_V2_GET_LINE_IOCTL, buf, True)
        self.lfd = struct.unpack_from("<i", buf, 588)[0]
        self._get_values = self._ioc(3, 0xB4, 0x0E, 16)

    def get(self):
        vbuf = array.array("B", struct.pack("<QQ", 0, 1))
        fcntl.ioctl(self.lfd, self._get_values, vbuf, True)
        return struct.unpack_from("<Q", vbuf, 0)[0] & 1

    def wait_high(self, timeout=0.05):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.get():
                return True
        return False


BLOCK_SIZE = 64
CRC_OFFSET = BLOCK_SIZE - 2
MAGIC = 0xA5
VERSION = 0x01
TYPE_KEYS = 0x20


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
    ap = argparse.ArgumentParser(description="DOOM SPI input forwarder (SoC side)")
    ap.add_argument("--bus", type=int, default=0)
    ap.add_argument("--dev", type=int, default=0)
    ap.add_argument("--speed", type=int, default=2000000)
    ap.add_argument("--rdy-chip", default="gpiochip1")
    ap.add_argument("--rdy-line", type=int, default=70)
    ap.add_argument("--hz", type=float, default=60.0,
                    help="resend rate of the current key state")
    ap.add_argument("--no-rdy", action="store_true",
                    help="don't gate on RDY (clock blindly)")
    args = ap.parse_args()

    spi = RawSpi(speed=args.speed)
    spi.open(args.bus, args.dev)
    rdy = None if args.no_rdy else GpioLine(args.rdy_chip, args.rdy_line)

    period = 1.0 / args.hz
    seq = 0
    mask = 0
    stdin_fd = sys.stdin.buffer.fileno()
    # Newline-delimited decimal masks (e.g. "5\n"): robust over adb stdin, which
    # can mangle raw binary. Keep a residual buffer for partial lines.
    buf = b""
    sys.stderr.write("wsad_send: spidev%d.%d @ %d Hz, RDY %s:%d -> sending keys\n"
                     % (args.bus, args.dev, args.hz, args.rdy_chip, args.rdy_line))
    sys.stderr.flush()

    while True:
        # Drain any pending bitmask updates; keep only the most recent line.
        r, _, _ = select.select([stdin_fd], [], [], period)
        if r:
            data = os.read(stdin_fd, 4096)
            if data == b"":
                break  # EOF: laptop side closed
            buf += data
            if b"\n" in buf:
                *lines, buf = buf.split(b"\n")
                for ln in reversed(lines):
                    ln = ln.strip()
                    if ln.isdigit():
                        mask = int(ln) & 0xFF
                        break
        if rdy is not None:
            rdy.wait_high(0.02)
        try:
            spi.xfer(make_keys_block(seq, mask))
        except OSError:
            time.sleep(0.005)
        seq = (seq + 1) & 0xFF


if __name__ == "__main__":
    main()
