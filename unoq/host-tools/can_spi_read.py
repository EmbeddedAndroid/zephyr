#!/usr/bin/env python3
"""Read CAN-over-SPI blocks from the UNO Q MCU via /dev/spidev0.0.

The MCU (apps/can_spi_bridge) is the SPI slave; it packs looped-back CAN frames
into fixed 64-byte blocks. We are the master: each transfer clocks exactly 64
bytes (size MUST match the slave or the stream desyncs). The slave reacts one
transfer late, so block N is read on transfer N+1.

Block format (see apps/can_spi_bridge/src/main.c):
  [0] magic 0xA5  [1] ver 0x01  [2] count(0..3)  [3] seq
  then count records of 16 bytes: id(LE u32) dlc flags rsvd[2] data[8]

Usage: can_spi_read.py [dev] [n] [delay_s] [speed_hz]
  defaults: /dev/spidev0.0 20 0.05 1000000
(no argparse: the device python is stripped down to builtins only)
"""
import array, fcntl, struct, time, sys

SPI_IOC_MAGIC = ord('k')
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (t << 8) | nr
def WR_MODE():  return _IOC(1, SPI_IOC_MAGIC, 1, 1)
def WR_BITS():  return _IOC(1, SPI_IOC_MAGIC, 3, 1)
def WR_SPEED(): return _IOC(1, SPI_IOC_MAGIC, 4, 4)
XFER_FMT = "QQIIHBBBBBB"; XFER_SIZE = 32
def MESSAGE(n): return _IOC(1, SPI_IOC_MAGIC, 0, XFER_SIZE * n)

BLOCK = 64
REC = 16

def xfer(fd, speed, bits, txbytes):
    n = len(txbytes)
    txb = array.array("B", txbytes)
    rxb = array.array("B", [0]*n)
    ta, _ = txb.buffer_info(); ra, _ = rxb.buffer_info()
    s = struct.pack(XFER_FMT, ta, ra, n, speed, 0, bits, 0, 0, 0, 0, 0)
    fcntl.ioctl(fd, MESSAGE(1), s)
    return bytes(rxb)

def parse(block):
    if len(block) != BLOCK or block[0] != 0xA5:
        return None
    ver, count, seq = block[1], block[2], block[3]
    recs = []
    for i in range(min(count, 3)):
        r = block[4 + i*REC : 4 + (i+1)*REC]
        cid = r[0] | (r[1] << 8) | (r[2] << 16) | (r[3] << 24)
        dlc = r[4]; flags = r[5]
        data = r[8:8+min(dlc, 8)]
        recs.append((cid, dlc, flags, data))
    return ver, count, seq, recs

def main():
    dev   = sys.argv[1] if len(sys.argv) > 1 else "/dev/spidev0.0"
    n     = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    delay = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
    speed = int(sys.argv[4]) if len(sys.argv) > 4 else 1000000
    bits  = 8

    f = open(dev, "r+b", buffering=0)   # keep the object alive; .fileno() alone GCs it
    fd = f.fileno()
    fcntl.ioctl(fd, WR_MODE(), struct.pack("B", 0))
    fcntl.ioctl(fd, WR_BITS(), struct.pack("B", bits))
    fcntl.ioctl(fd, WR_SPEED(), struct.pack("I", speed))

    zeros = bytes(BLOCK)
    total_frames = 0
    valid_blocks = 0
    for t in range(n):
        rx = xfer(fd, speed, bits, zeros)
        p = parse(rx)
        if p is None:
            print("xfer %d: raw=%s (no valid block yet)" % (t, rx[:8].hex()))
        else:
            ver, count, seq, recs = p
            valid_blocks += 1
            total_frames += len(recs)
            if recs:
                desc = "; ".join("id=0x%03x dlc=%d data=%s" % (c, d, bytes(dd).hex())
                                 for (c, d, fl, dd) in recs)
            else:
                desc = "(idle)"
            print("xfer %d: seq=%d count=%d :: %s" % (t, seq, count, desc))
        time.sleep(delay)

    print("\nsummary: %d valid blocks, %d CAN frames received over SPI"
          % (valid_blocks, total_frames))

if __name__ == "__main__":
    main()
