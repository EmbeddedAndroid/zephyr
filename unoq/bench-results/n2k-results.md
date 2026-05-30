# NMEA 2000 variant benchmark log

Variant: apps/can_spi_bridge_n2k. Classic CAN 2.0B, 250 kbit/s, 29-bit EXTENDED
IDs (CAN_FRAME_IDE), CAN_MODE_LOOPBACK for bench (BENCH_LOOPBACK=1; real bus
needs a transceiver on PD0/PD1 + CAN_MODE_NORMAL). Same RDY flow control +
CRC-16 block protection as the FD variant. Record 16 B (8 data), block 256 B,
up to 15 frames/block.

## Correctness (host-tools/n2k_test.py)
- 30 frames, 29-bit extended IDs, 8-byte payloads: 30/30 read back, 0 missing,
  0 extra, byte-exact. IDs verified <= 0x1FFFFFFF (29-bit). PASS.
- Required pacing injection to the bus rate. Without pacing the host outruns the
  250 kbit/s wire and the MCU drops a block while busy in can_send() (read 15/30
  first try). With pacing: 30/30 lossless.

## Throughput (host-tools/n2k_bench.py, 600 frames, pace sweep, MEASURED)
Three runs, consistent. Representative run:

| inject pace | read/600 | secs | frames/s | payload kB/s | result |
|-------------|----------|------|----------|--------------|--------|
| 2.0 ms/frame | 600/600 | 1.46 | 411  | 3.3 | lossless |
| 1.0 ms/frame | 600/600 | 0.86 | 698  | 5.6 | lossless |
| 0.6 ms/frame | 600/600 | 0.62 | 970  | 7.8 | lossless |
| 0.5 ms/frame | 600/600 | 0.56 | 1074 | 8.6 | lossless |
| 0.4 ms/frame | 600/600 | 0.50 | 1204 | 9.6 | lossless (best robust) |
| 0.3 ms/frame | 585/600 | 0.50 | 1175 | 9.4 | LOSS onset (15 lost) |
| 0.2 ms/frame | 300/600 | 0.40 | 756  | 6.0 | heavy loss |

BEST ROBUST LOSSLESS: 1204 frames/s, 9.6 kB/s payload, at 0.4 ms/frame pace.
(0.3 ms is marginal: best run hit 1281 fps but another lost 285/600 -- the
loss wall is right here. 0.4 ms was lossless in all 3 runs.)

## Honest interpretation (this REPLACES an earlier draft with wrong numbers)
- There IS a clear loss wall, at ~0.3-0.35 ms/frame. Below it the link is
  lossless; at/above it frames are dropped. (An earlier write-up claimed
  "lossless at every rate, 281 fps host-limited" -- that was WRONG, written
  before the benchmark actually ran. Corrected here from measured data.)
- Best lossless ~1,204 frames/s (9.6 kB/s) is close to the theoretical
  250 kbit/s ceiling of ~1,953 frames/s for 8-byte extended frames
  (~128 bits/frame incl. stuffing + inter-frame space). So the limiter is the
  CAN side (bus bit-time + the MCU TX-mailbox/inject loop draining at bus rate),
  NOT the SPI link or the host driver.
- The loss mechanism at high pace: the MCU SPI thread blocks in can_send() while
  the 250 kbit/s controller drains the TX mailbox; if the host clocks the next
  SPI transfer during that window with no block armed, it is lost. RDY flow
  control prevents RX-side loss but does not throttle the host's INJECT rate --
  inject pacing (or honoring a TX-side ready signal) is what keeps it lossless.
- MCU cross-check after a full sweep: g_frames_injected = g_can_rx =
  g_frames_packed = 0xcf3 = 3315 (all equal -> zero internal loss on the
  frames that were accepted), g_rx_crc_err = 0.

## What this means for a real N2K bus
- Bridge logic, framing, extended IDs, CRC, and RX flow control all verified.
- ~1,200 lossless frames/s is far above typical N2K aggregate traffic (usually
  well under 1,000 frames/s across a whole network), so the bridge has headroom.
- To drive an actual N2K bus: wire a 5V CAN transceiver (TJA1051 / MCP2562) to
  PD0(RX)/PD1(TX), 120 ohm termination + bus power per N2K spec, build with
  BENCH_LOOPBACK=0 (CAN_MODE_NORMAL). 250 kbit/s / 87.5% timing already correct.
- NOT YET TESTED on a physical N2K bus (no transceiver on the bench).
- N2K higher layers (fast-packet for >8-byte PGNs, address claiming, PGN
  decode) are OUT OF SCOPE -- this is the CAN transport layer.
