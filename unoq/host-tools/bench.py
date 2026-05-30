#!/usr/bin/env python3
"""Throughput benchmark for the CAN<->SPI bridge (UNO Q).

Measures sustained round-trip throughput: each SPI transfer injects up to
RECS_PER_BLOCK CAN frames (MOSI) and reads up to RECS_PER_BLOCK looped-back
frames (MISO). CAN is in loopback so injected frames come straight back.

Methodology (honest + repeatable):
  - warmup transfers (not timed) to fill the pipeline
  - timed loop: run back-to-back full-duplex transfers until TARGET frames have
    been read back; measure wall-clock with time.monotonic()
  - NO artificial sleeps, NO per-transfer gpioget (subprocess fork would
    dominate) -- we saturate the link and let RDY correctness be tested
    separately by can_spi_bridge.py
  - repeat RUNS times, report median frames/s + payload bytes/s + blocks/s

Block format is parameterized so the same harness works across phases
(classic 64B / CAN-FD bigger blocks / +CRC). Override via args.

Usage: bench.py [block_size] [rec_size] [recs_per_block] [payload] [target] [runs] [dev] [speed]
Defaults tuned for the classic-CAN baseline: 64 16 3 8 5000 5 /dev/spidev0.0 1000000
"""
import array, fcntl, struct, time, sys, binascii

SPI_IOC_MAGIC = ord('k')
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (t << 8) | nr
def WR_MODE():  return _IOC(1, SPI_IOC_MAGIC, 1, 1)
def WR_BITS():  return _IOC(1, SPI_IOC_MAGIC, 3, 1)
def WR_SPEED(): return _IOC(1, SPI_IOC_MAGIC, 4, 4)
XFER_FMT = "QQIIHBBBBBB"; XFER_SIZE = 32
def MESSAGE(n): return _IOC(1, SPI_IOC_MAGIC, 0, XFER_SIZE * n)

MAGIC = 0xA5

def crc16(buf, n):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Matches the firmware.
    Uses the C-implemented binascii.crc_hqx (init 0xFFFF) -- ~100x faster than
    a pure-python bit loop, which dominated the +CRC benchmark."""
    return binascii.crc_hqx(bytes(buf[:n]), 0xFFFF)

def make_xfer_fn(fd, speed):
    def xfer(txb, rxb, n):
        ta, _ = txb.buffer_info(); ra, _ = rxb.buffer_info()
        s = struct.pack(XFER_FMT, ta, ra, n, speed, 0, 8, 0, 0, 0, 0, 0)
        fcntl.ioctl(fd, MESSAGE(1), s)
    return xfer

def main():
    a = sys.argv
    BLOCK   = int(a[1]) if len(a) > 1 else 64
    REC     = int(a[2]) if len(a) > 2 else 16
    RECS    = int(a[3]) if len(a) > 3 else 3
    PAYLOAD = int(a[4]) if len(a) > 4 else 8     # data bytes per frame
    TARGET  = int(a[5]) if len(a) > 5 else 5000
    RUNS    = int(a[6]) if len(a) > 6 else 5
    dev     = a[7] if len(a) > 7 else "/dev/spidev0.0"
    speed   = int(a[8]) if len(a) > 8 else 1000000
    VER     = int(a[9]) if len(a) > 9 else 0x02  # 0x02 classic/fd, 0x03 crc

    f = open(dev, "r+b", buffering=0)
    fd = f.fileno()
    fcntl.ioctl(fd, WR_MODE(), struct.pack("B", 0))
    fcntl.ioctl(fd, WR_BITS(), struct.pack("B", 8))
    fcntl.ioctl(fd, WR_SPEED(), struct.pack("I", speed))
    xfer = make_xfer_fn(fd, speed)

    CRC = (VER >= 0x03)
    CRC_OFF = BLOCK - 2

    # Pre-build a MOSI inject block: RECS frames, id 0x100+i, PAYLOAD data bytes.
    def build_mosi(seq):
        b = bytearray(BLOCK)
        b[0] = MAGIC; b[1] = VER; b[2] = RECS; b[3] = seq & 0xff
        for i in range(RECS):
            r = 4 + i*REC
            cid = 0x100 + i
            b[r+0] = cid & 0xff; b[r+1] = (cid>>8)&0xff
            b[r+4] = PAYLOAD; b[r+5] = 0
            for k in range(PAYLOAD):
                b[r+8+k] = (i*PAYLOAD + k) & 0xff
        if CRC:
            c = crc16(b, CRC_OFF)
            b[CRC_OFF] = c & 0xff; b[CRC_OFF+1] = (c >> 8) & 0xff
        return bytes(b)

    mosi = build_mosi(0)
    txb = array.array("B", mosi)
    rxb = array.array("B", bytes(BLOCK))

    stats = {"crc_err": 0, "seq_gap": 0, "last_seq": None}
    def count_miso():
        # parse rxb, return number of valid records; verify CRC + seq continuity
        if rxb[0] != MAGIC:
            return 0
        if CRC:
            want = crc16(rxb, CRC_OFF)
            have = rxb[CRC_OFF] | (rxb[CRC_OFF+1] << 8)
            if want != have:
                stats["crc_err"] += 1
                return 0
        seq = rxb[3]
        if stats["last_seq"] is not None:
            exp = (stats["last_seq"] + 1) & 0xff
            if seq != exp:
                stats["seq_gap"] += 1
        stats["last_seq"] = seq
        c = rxb[2]
        return c if c <= RECS else 0

    results = []
    for run in range(RUNS):
        # warmup: ~300 frames worth of transfers
        for _ in range(max(1, 300 // RECS)):
            xfer(txb, rxb, BLOCK)
        # timed
        read = 0
        blocks = 0
        t0 = time.monotonic()
        while read < TARGET:
            xfer(txb, rxb, BLOCK)
            read += count_miso()
            blocks += 1
        dt = time.monotonic() - t0
        fps = read / dt
        Bps = fps * PAYLOAD
        bps_blocks = blocks / dt
        results.append((fps, Bps, bps_blocks, read, blocks, dt))
        print("run %d: %.0f frames/s  %.1f kB/s payload  %.0f blocks/s  (%d frames, %d blocks, %.3fs)"
              % (run, fps, Bps/1000.0, bps_blocks, read, blocks, dt))

    results.sort(key=lambda r: r[0])
    med = results[len(results)//2]
    print("\nMEDIAN: %.0f frames/s | %.1f kB/s payload | %.0f blocks/s"
          % (med[0], med[1]/1000.0, med[2]))
    print("INTEGRITY: crc_err=%d seq_gap=%d (across last run)"
          % (stats["crc_err"], stats["seq_gap"]))
    print("PARAMS: block=%dB rec=%dB recs/blk=%d payload=%dB speed=%dHz ver=0x%02x crc=%s"
          % (BLOCK, REC, RECS, PAYLOAD, speed, VER, CRC))

if __name__ == "__main__":
    main()
