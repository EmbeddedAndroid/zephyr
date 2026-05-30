#!/usr/bin/env python3
# Copyright (c) 2026 Qualcomm Innovation Center, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Host (QCS Linux) driver for the charlieplex SPI bridge. Runs as the SPI
# master on /dev/spidevN.M via raw ioctl (no python-spidev dependency -- the
# stock qcom-distro image has no spidev module). Sends framebuffers / commands
# to the STM32 slave and reads back the MCU status block on the same
# full-duplex transfer.
#
# Protocol mirrors samples/drivers/charlieplex_bridge/src/main.c:
#   block = [magic 0xA5][ver 1][type][seq][len][payload...][crc16-le]
#   CRC-16/CCITT-FALSE over block[0:-2] == binascii.crc_hqx(buf, 0xFFFF).
#
# The status read back is the *previous* transfer's result (one-transfer
# pipeline), so a flush transfer is clocked to read the final status.
#
# Usage:
#   bridge_send.py all-on
#   bridge_send.py clear
#   bridge_send.py walk
#   bridge_send.py checker
#   bridge_send.py cmd brightness 128

import argparse
import array
import binascii
import fcntl
import struct
import time

# --- raw /dev/spidev transfer via ioctl --------------------------------------
SPI_IOC_MAGIC = ord("k")


def _iow(t, nr, size):
    return (1 << 30) | (size << 16) | (t << 8) | nr


SPI_IOC_WR_MODE = _iow(SPI_IOC_MAGIC, 1, 1)
SPI_IOC_WR_BITS_PER_WORD = _iow(SPI_IOC_MAGIC, 3, 1)
SPI_IOC_WR_MAX_SPEED_HZ = _iow(SPI_IOC_MAGIC, 4, 4)


def _spi_message(nr):
    return _iow(SPI_IOC_MAGIC, 0, nr * 32)


class RawSpi:
    def __init__(self):
        self.fd = None
        self.max_speed_hz = 2000000
        self.mode = 0

    def open(self, bus, dev):
        self.fd = open("/dev/spidev%d.%d" % (bus, dev), "r+b", buffering=0)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, struct.pack("=B", self.mode))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack("=B", 8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ,
                    struct.pack("=I", self.max_speed_hz))

    def xfer2(self, data):
        n = len(data)
        # array buffers stay alive for the ioctl; pass their addresses to the
        # kernel (no ctypes -- the stock image's python lacks it).
        txbuf = array.array("B", data)
        rxbuf = array.array("B", [0] * n)
        tx_addr, _ = txbuf.buffer_info()
        rx_addr, _ = rxbuf.buffer_info()
        # struct spi_ioc_transfer (32 bytes): u64 tx; u64 rx; u32 len;
        # u32 speed; u16 delay; u8 bits; u8 cs_change; u8 tx_nbits;
        # u8 rx_nbits; u8 word_delay; u8 pad.
        msg = struct.pack("QQIIHBBBBBB",
                          tx_addr, rx_addr, n, self.max_speed_hz,
                          0, 8, 0, 0, 0, 0, 0)
        fcntl.ioctl(self.fd, _spi_message(1), msg)
        return list(rxbuf)

    def close(self):
        if self.fd:
            self.fd.close()


# --- protocol ----------------------------------------------------------------
BLOCK_SIZE = 64
CRC_OFFSET = BLOCK_SIZE - 2
MAGIC = 0xA5
VERSION = 0x01

TYPE_FB = 0x01
TYPE_CMD = 0x02
TYPE_STATUS = 0x10
TYPE_VERSION = 0x11

CMD = {"blank-on": 1, "blank-off": 2, "brightness": 3, "query": 4, "version": 5}

WIDTH = 13
HEIGHT = 8
STRIDE = (WIDTH + 7) // 8
FB_BYTES = STRIDE * HEIGHT


def crc16(buf):
    return binascii.crc_hqx(bytes(buf), 0xFFFF)


def make_block(btype, seq, payload):
    blk = bytearray(BLOCK_SIZE)
    blk[0] = MAGIC
    blk[1] = VERSION
    blk[2] = btype
    blk[3] = seq & 0xFF
    blk[4] = len(payload)
    blk[5:5 + len(payload)] = payload
    crc = crc16(blk[:CRC_OFFSET])
    blk[CRC_OFFSET] = crc & 0xFF
    blk[CRC_OFFSET + 1] = (crc >> 8) & 0xFF
    return list(blk)


def parse_status(rx):
    rx = bytes(rx)
    if len(rx) < BLOCK_SIZE or rx[0] != MAGIC or rx[2] != TYPE_STATUS:
        return None
    want = crc16(rx[:CRC_OFFSET])
    have = rx[CRC_OFFSET] | (rx[CRC_OFFSET + 1] << 8)
    if want != have:
        return {"crc_err_local": True}
    n = rx[4]
    body = rx[5:5 + n]
    if len(body) < 16:
        return None
    fd, blocks, ce, seq, typ, state, rdy = struct.unpack("<IIIBBBB", body[:16])
    return {"frames_drawn": fd, "blocks": blocks, "crc_err": ce,
            "last_seq": seq, "last_type": typ, "state": state, "rdy": rdy}


# 5x7 font, columns left-to-right, bit0 = top row. Uppercased text only.
FONT = {
    " ": [0x00, 0x00, 0x00],
    "0": [0x3E, 0x51, 0x49, 0x45, 0x3E], "1": [0x00, 0x42, 0x7F, 0x40, 0x00],
    "2": [0x42, 0x61, 0x51, 0x49, 0x46], "3": [0x21, 0x41, 0x45, 0x4B, 0x31],
    "4": [0x18, 0x14, 0x12, 0x7F, 0x10], "5": [0x27, 0x45, 0x45, 0x45, 0x39],
    "6": [0x3C, 0x4A, 0x49, 0x49, 0x30], "7": [0x01, 0x71, 0x09, 0x05, 0x03],
    "8": [0x36, 0x49, 0x49, 0x49, 0x36], "9": [0x06, 0x49, 0x49, 0x29, 0x1E],
    "A": [0x7E, 0x11, 0x11, 0x11, 0x7E], "B": [0x7F, 0x49, 0x49, 0x49, 0x36],
    "C": [0x3E, 0x41, 0x41, 0x41, 0x22], "D": [0x7F, 0x41, 0x41, 0x22, 0x1C],
    "E": [0x7F, 0x49, 0x49, 0x49, 0x41], "F": [0x7F, 0x09, 0x09, 0x09, 0x01],
    "G": [0x3E, 0x41, 0x49, 0x49, 0x7A], "H": [0x7F, 0x08, 0x08, 0x08, 0x7F],
    "I": [0x00, 0x41, 0x7F, 0x41, 0x00], "J": [0x20, 0x40, 0x41, 0x3F, 0x01],
    "K": [0x7F, 0x08, 0x14, 0x22, 0x41], "L": [0x7F, 0x40, 0x40, 0x40, 0x40],
    "M": [0x7F, 0x02, 0x0C, 0x02, 0x7F], "N": [0x7F, 0x04, 0x08, 0x10, 0x7F],
    "O": [0x3E, 0x41, 0x41, 0x41, 0x3E], "P": [0x7F, 0x09, 0x09, 0x09, 0x06],
    "Q": [0x3E, 0x41, 0x51, 0x21, 0x5E], "R": [0x7F, 0x09, 0x19, 0x29, 0x46],
    "S": [0x46, 0x49, 0x49, 0x49, 0x31], "T": [0x01, 0x01, 0x7F, 0x01, 0x01],
    "U": [0x3F, 0x40, 0x40, 0x40, 0x3F], "V": [0x1F, 0x20, 0x40, 0x20, 0x1F],
    "W": [0x3F, 0x40, 0x38, 0x40, 0x3F], "X": [0x63, 0x14, 0x08, 0x14, 0x63],
    "Y": [0x07, 0x08, 0x70, 0x08, 0x07], "Z": [0x61, 0x51, 0x49, 0x45, 0x43],
    ".": [0x00, 0x60, 0x60, 0x00], "-": [0x08, 0x08, 0x08, 0x08, 0x08],
    "_": [0x40, 0x40, 0x40, 0x40, 0x40], "/": [0x20, 0x10, 0x08, 0x04, 0x02],
    "+": [0x08, 0x08, 0x3E, 0x08, 0x08], ":": [0x00, 0x36, 0x36, 0x00],
}


def short_kernel_release(rel):
    """Trim a Linux release to its base version (drop the -NNNNN-gHASH-dirty
    build suffix) so it scrolls in a comparable time to the Zephyr version.
    e.g. '7.1.0-rc4-00852-gdf3ae9703774-dirty' -> '7.1.0-rc4'."""
    import re
    m = re.match(r"^[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?", rel)
    return m.group(0) if m else rel


def query_zephyr_version(spi, seq):
    """Ask the MCU for its Zephyr version; reply arrives on the next transfer."""
    spi.xfer2(make_block(TYPE_CMD, seq, bytes([CMD["version"], 0])))
    rx = bytes(spi.xfer2(make_block(TYPE_CMD, seq + 1, bytes([CMD["query"], 0]))))
    if len(rx) >= 6 and rx[0] == MAGIC and rx[2] == TYPE_VERSION:
        return rx[5:5 + rx[4]].decode("ascii", "replace")
    return None


def text_columns(text):
    """Flatten text to a list of column-bytes, 1 blank column between glyphs."""
    cols = []
    for ch in text.upper():
        glyph = FONT.get(ch, FONT[" "])
        cols.extend(glyph)
        cols.append(0x00)
    return cols


def set_pixel(fb, x, y, on):
    i = y * STRIDE + (x // 8)
    if on:
        fb[i] |= (1 << (x % 8))
    else:
        fb[i] &= ~(1 << (x % 8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bus", type=int, default=0)
    ap.add_argument("--dev", type=int, default=0)
    ap.add_argument("--speed", type=int, default=2000000)
    ap.add_argument("--delay", type=float, default=0.1)
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("walk")
    sub.add_parser("checker")
    sub.add_parser("hat")
    pk = sub.add_parser("kver")
    pk.add_argument("--text", default=None,
                    help="override text (default: live kernel release)")
    sub.add_parser("versions")
    sub.add_parser("all-on")
    sub.add_parser("clear")
    pc = sub.add_parser("cmd")
    pc.add_argument("name", choices=list(CMD))
    pc.add_argument("arg", type=int, nargs="?", default=0)
    args = ap.parse_args()

    spi = RawSpi()
    spi.max_speed_hz = args.speed
    spi.open(args.bus, args.dev)

    seq = 1

    def send(btype, payload):
        return parse_status(spi.xfer2(make_block(btype, seq, payload)))

    if args.mode == "cmd":
        send(TYPE_CMD, bytes([CMD[args.name], args.arg & 0xFF]))
        st = send(TYPE_CMD, bytes([CMD["query"], 0]))  # flush -> read status
        print("status:", st, flush=True)
    elif args.mode == "all-on":
        st = send(TYPE_FB, b"\xff" * FB_BYTES)
        st = send(TYPE_FB, b"\xff" * FB_BYTES)  # 2nd xfer reads the draw status
        print("status:", st, flush=True)
    elif args.mode == "clear":
        send(TYPE_FB, bytes(FB_BYTES))
        st = send(TYPE_FB, bytes(FB_BYTES))
        print("status:", st, flush=True)
    elif args.mode == "walk":
        try:
            while True:
                for y in range(HEIGHT):
                    for x in range(WIDTH):
                        fb = bytearray(FB_BYTES)
                        set_pixel(fb, x, y, True)
                        st = send(TYPE_FB, bytes(fb))
                        seq += 1
                        if isinstance(st, dict) and "frames_drawn" in st:
                            print("drawn=%d crc_err=%d rdy=%d   " %
                                  (st["frames_drawn"], st["crc_err"], st["rdy"]),
                                  end="\r", flush=True)
                        time.sleep(args.delay)
        except KeyboardInterrupt:
            pass
    elif args.mode == "checker":
        try:
            phase = 0
            while True:
                fb = bytearray(FB_BYTES)
                for y in range(HEIGHT):
                    for x in range(WIDTH):
                        set_pixel(fb, x, y, ((x + y + phase) & 1) == 0)
                st = send(TYPE_FB, bytes(fb))
                seq += 1
                phase ^= 1
                if isinstance(st, dict) and "frames_drawn" in st:
                    print("drawn=%d crc_err=%d   " %
                          (st["frames_drawn"], st["crc_err"]),
                          end="\r", flush=True)
                time.sleep(max(args.delay, 0.3))
        except KeyboardInterrupt:
            pass

    elif args.mode == "hat":
        # Cowboy hat, 11 wide x 7 tall, '#' = lit. Centered in the 13x8 panel
        # so it can sway +/-1 column ("tip the hat") and bob +/-0..1 row.
        hat = [
            "...#####...",
            "..#######..",
            "..#######..",
            ".#########.",
            "###########",
            "###########",
            ".#########.",
        ]
        hat_w = len(hat[0])
        base_x = (WIDTH - hat_w) // 2
        # sway/bob keyframes (dx, dy) -> a gentle nod
        anim = [(-1, 0), (0, 0), (1, 0), (1, 1), (0, 1), (-1, 1), (0, 0)]
        try:
            f = 0
            while True:
                dx, dy = anim[f % len(anim)]
                f += 1
                fb = bytearray(FB_BYTES)
                for ry, row in enumerate(hat):
                    for rx, ch in enumerate(row):
                        if ch != "#":
                            continue
                        x = base_x + rx + dx
                        y = ry + dy
                        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
                            set_pixel(fb, x, y, True)
                st = send(TYPE_FB, bytes(fb))
                seq += 1
                if isinstance(st, dict) and "frames_drawn" in st:
                    print("hat drawn=%d crc_err=%d   " %
                          (st["frames_drawn"], st["crc_err"]),
                          end="\r", flush=True)
                time.sleep(max(args.delay, 0.25))
        except KeyboardInterrupt:
            pass

    elif args.mode in ("kver", "versions"):
        import os

        def scroll_text(text, start_seq):
            cols = text_columns(text)
            y_off = max((HEIGHT - 7) // 2, 0)
            total = len(cols) + WIDTH
            s = start_seq
            for scroll in range(total):
                fb = bytearray(FB_BYTES)
                for x in range(WIDTH):
                    src = x + scroll - WIDTH
                    if src < 0 or src >= len(cols):
                        continue
                    colbits = cols[src]
                    for row in range(7):
                        if colbits & (1 << row):
                            set_pixel(fb, x, y_off + row, True)
                st = parse_status(spi.xfer2(make_block(TYPE_FB, s, bytes(fb))))
                s += 1
                if isinstance(st, dict) and "frames_drawn" in st:
                    print("drawn=%d crc_err=%d   " %
                          (st["frames_drawn"], st["crc_err"]),
                          end="\r", flush=True)
                time.sleep(args.delay)
            return s

        if args.mode == "kver":
            msgs = [getattr(args, "text", None) or os.uname().release]
        else:
            zver = query_zephyr_version(spi, seq) or "ZEPHYR ?"
            seq += 2
            msgs = ["LINUX " + short_kernel_release(os.uname().release), zver]
        print("scrolling: %s" % " | ".join(msgs), flush=True)
        try:
            while True:
                for m in msgs:
                    seq = scroll_text(m, seq)
        except KeyboardInterrupt:
            pass

    spi.close()


if __name__ == "__main__":
    main()
