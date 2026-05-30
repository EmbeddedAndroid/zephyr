#!/usr/bin/env python3
"""Bidirectional CAN<->SPI bridge driver for the UNO Q, with RDY flow control.

The MCU (apps/can_spi_bridge) is the SPI slave + CAN-in-loopback. We (QCM Linux,
SPI master on /dev/spidev0.0) both:
  - INJECT CAN frames by sending a command block on MOSI; the MCU can_send()s
    them, they loopback, and come back to us on a later MISO block.
  - READ looped-back frames from MISO blocks, gated on the RDY GPIO
    (gpiochip1:70) so we only clock a transfer when the MCU has data.

Because every CAN frame originates from an injected record (no free-running
generator), accounting is exact: injected == looped-back-and-read, with no loss
when RDY flow control holds.

Block format (both directions), 64 bytes:
  [0] magic 0xA5  [1] ver 0x02  [2] count(0..3)  [3] seq
  [4+i*16] record: id(LE u32) dlc flags rsvd[2] data[8]

RDY is read via libgpiod's gpioget (shipped in oo/bin). Pure-builtins otherwise
(device python has no spidev/ctypes/argparse).

Usage: can_spi_bridge.py <n_inject> [dev] [speed]
  injects n_inject frames (id 0x100+i, data = i as 4 bytes), then drains.
"""
import array, fcntl, struct, time, sys, subprocess, os

SPI_IOC_MAGIC = ord('k')
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (t << 8) | nr
def WR_MODE():  return _IOC(1, SPI_IOC_MAGIC, 1, 1)
def WR_BITS():  return _IOC(1, SPI_IOC_MAGIC, 3, 1)
def WR_SPEED(): return _IOC(1, SPI_IOC_MAGIC, 4, 4)
XFER_FMT = "QQIIHBBBBBB"; XFER_SIZE = 32
def MESSAGE(n): return _IOC(1, SPI_IOC_MAGIC, 0, XFER_SIZE * n)

BLOCK = 64
REC = 16
MAGIC = 0xA5
VER = 0x02
MAX_RECS = 3

OO = "/home/root/zephyr-flash/oo"
RDY_CHIP = "/dev/gpiochip1"
RDY_LINE = "70"

def rdy_get():
    """Read RDY line via gpioget (libgpiod v3). Returns 0/1, or -1 on error."""
    env = dict(os.environ, LD_LIBRARY_PATH=OO + "/lib")
    try:
        out = subprocess.check_output(
            [OO + "/bin/gpioget", "-c", RDY_CHIP, RDY_LINE],
            env=env, stderr=subprocess.STDOUT).decode().strip()
    except subprocess.CalledProcessError as e:
        return -1
    # gpioget v3 prints e.g. "70=active" or "70=inactive" or just "1"/"0"
    if "active" in out:
        return 0 if "inactive" in out else 1
    tok = out.split("=")[-1].strip()
    return 1 if tok in ("1", "active") else 0

def xfer(fd, speed, txbytes):
    n = len(txbytes)
    txb = array.array("B", txbytes)
    rxb = array.array("B", [0]*n)
    ta, _ = txb.buffer_info(); ra, _ = rxb.buffer_info()
    s = struct.pack(XFER_FMT, ta, ra, n, speed, 0, 8, 0, 0, 0, 0, 0)
    fcntl.ioctl(fd, MESSAGE(1), s)
    return bytes(rxb)

def make_block(seq, frames):
    """frames: list of (id, data_bytes). up to MAX_RECS."""
    b = bytearray(BLOCK)
    b[0] = MAGIC; b[1] = VER; b[2] = len(frames); b[3] = seq & 0xff
    for i, (cid, data) in enumerate(frames[:MAX_RECS]):
        r = 4 + i*REC
        b[r+0] = cid & 0xff; b[r+1] = (cid >> 8) & 0xff
        b[r+2] = (cid >> 16) & 0xff; b[r+3] = (cid >> 24) & 0xff
        b[r+4] = len(data); b[r+5] = 0
        b[r+8:r+8+min(len(data),8)] = data[:8]
    return bytes(b)

def parse(block):
    if len(block) != BLOCK or block[0] != MAGIC:
        return None
    count, seq = block[2], block[3]
    recs = []
    for i in range(min(count, MAX_RECS)):
        r = block[4+i*REC : 4+(i+1)*REC]
        cid = r[0] | (r[1]<<8) | (r[2]<<16) | (r[3]<<24)
        dlc = r[4]; data = bytes(r[8:8+min(dlc,8)])
        recs.append((cid, dlc, data))
    return seq, count, recs

def main():
    n_inject = int(sys.argv[1]) if len(sys.argv) > 1 else 9
    dev = sys.argv[2] if len(sys.argv) > 2 else "/dev/spidev0.0"
    speed = int(sys.argv[3]) if len(sys.argv) > 3 else 1000000

    f = open(dev, "r+b", buffering=0)   # keep object alive (else fd GC'd -> EBADF)
    fd = f.fileno()
    fcntl.ioctl(fd, WR_MODE(), struct.pack("B", 0))
    fcntl.ioctl(fd, WR_BITS(), struct.pack("B", 8))
    fcntl.ioctl(fd, WR_SPEED(), struct.pack("I", speed))

    zeros = bytes(BLOCK)

    # Build the frames we will inject: id 0x100+i, data = 4 bytes of i.
    want = []
    for i in range(n_inject):
        want.append((0x100 + i, bytes([0xC0, 0xDE, (i >> 8) & 0xff, i & 0xff])))

    print("RDY before inject:", rdy_get())

    got = []
    def collect(rx):
        p = parse(rx)
        if p:
            _seq, count, recs = p
            for (cid, dlc, data) in recs:
                got.append((cid, data))

    # Inject in batches of up to 3 per MOSI block. CRUCIAL: every transfer is
    # full-duplex -- the MISO that returns while we inject already carries
    # looped-back frames, so we must collect it too (not just during drain).
    seq = 0
    sent = 0
    while sent < len(want):
        batch = want[sent:sent+MAX_RECS]
        blk = make_block(seq, batch)
        rx = xfer(fd, speed, blk)
        collect(rx)
        sent += len(batch); seq += 1
        time.sleep(0.005)
    print("injected %d frames in %d MOSI blocks" % (sent, seq))

    # Drain remaining: read (MOSI zeros = no injection) while RDY says data is
    # queued. Give the last loopback a moment to populate first.
    time.sleep(0.05)
    idle_reads = 0
    for _ in range(200):
        r = rdy_get()
        if r <= 0:
            idle_reads += 1
            if idle_reads >= 3:
                break
            time.sleep(0.01)
            continue
        idle_reads = 0
        rx = xfer(fd, speed, zeros)
        collect(rx)
        time.sleep(0.002)

    print("RDY after drain:", rdy_get())
    print("read back %d frames" % len(got))

    # Accounting.
    want_set = {(c, d) for (c, d) in want}
    got_set = set(got)
    missing = want_set - got_set
    extra = got_set - want_set
    print("injected unique:", len(want_set),
          "read unique:", len(got_set),
          "missing:", len(missing), "unexpected:", len(extra))
    if not missing and not extra:
        print("PASS: every injected frame round-tripped, no loss, no extras")
    else:
        if missing:
            print("MISSING:", sorted("0x%x:%s" % (c, d.hex()) for c, d in missing)[:10])
        if extra:
            print("EXTRA:", sorted("0x%x:%s" % (c, d.hex()) for c, d in extra)[:10])

if __name__ == "__main__":
    main()
