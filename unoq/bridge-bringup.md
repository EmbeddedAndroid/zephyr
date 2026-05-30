# CAN -> SPI -> Linux bridge (step 3) — PASSED

Date 2026-05-28. Combined the two proven halves (FDCAN loopback + SPI slave)
into a working bridge: CAN frames generated on-chip in loopback are packed into
fixed SPI blocks and decoded by the QCM Linux master over /dev/spidev0.0. This
is the milestone: real CAN frames appearing, parsed, on the Linux side.

## Components

- MCU app: `apps/can_spi_bridge/` (+ boards/arduino_uno_q.overlay enabling
  fdcan1, same overlay as can_loopback). Two threads:
  - can_thread (prio 6): CAN_MODE_LOOPBACK, sends id=0x123 data=DE AD BE
    <counter> every ~5ms, drains the rx-filter msgq, counts g_can_rx, hands
    frames to ship_msgq.
  - main/SPI thread: SPI3 slave; loops spi_transceive of a fixed 64-byte block;
    after each completed transfer packs the NEXT block from ship_msgq (up to 3
    frames), counts g_frames_packed / g_blocks_sent.
- Linux reader: `host-tools/can_spi_read.py` (pure builtins -- device python has
  NO argparse and NO ctypes/spidev; uses sys.argv + array.buffer_info() for SPI
  ioctl). NOTE: must keep the open() file object alive; `open(...).fileno()`
  alone gets GC'd and the fd closes -> EBADF (bug hit + fixed during bringup).

## Wire format (fixed 64-byte block)

Size MUST match both ends exactly -- the STM32 slave is byte-count driven, a
short master transfer desyncs the stream (see spi-bringup.md).

  byte 0 : magic 0xA5
  byte 1 : version 0x01
  byte 2 : count (valid CAN records, 0..3)
  byte 3 : seq (block counter, wraps 256)
  byte 4.. : up to 3 records x 16 bytes:
      [0..3] id (LE u32)  [4] dlc  [5] flags  [6..7] rsvd  [8..15] data
  zero-padded to 64. MAX_RECS=3 because (64-4)/16 = 3.

The slave reacts one transfer late, so the master reads block N on transfer N+1.
count=0 = idle (no new frames). The first block (seq=0) is the preload, count=0.

## Result (REAL, verified)

Linux reader, 20 transfers @ 50ms:
- xfer 0: seq=0 count=0 (idle)  -- the preload block
- xfer 1..19: seq increments 1..19, every block count=3, all records
  id=0x123 data=deadbeXX (the loopback frames).
- Data counter is contiguous in the first blocks (deadbe00..deadbe1f) then
  jumps (..be1f, be4f, ..) once the MCU packs faster than the 50ms poll --
  expected backlog/batching behavior.
- summary: 20 valid blocks, 57 CAN frames decoded (19 data blocks x 3).

MCU SWD proof globals (correct addrs from `nm zephyr.elf` this build):
  g_state         0x20000894 = 0x3       running
  g_can_rx        0x20000890 = 0x44ca    17610 CAN frames received in loopback
  g_blocks_sent   0x2000088c = 0x14      20    SPI blocks master clocked (= 20 xfers)
  g_frames_packed 0x20000888 = 0x3c      60    CAN frames shipped over SPI

Consistency: 20 blocks clocked, 60 frames packed (3/block), reader decoded 57
data frames + 1 idle block = matches. HONEST CAVEAT: the MCU generated 17610
loopback frames but only 60 were shipped, because the master only polled 20
times. The rest were dropped/never-drained -- there is NO flow control yet, so
this is NOT a zero-loss result. ship_msgq (depth 32) overflows silently under
the ~200Hz CAN vs slow poll. Flow control (RDY line) is the next requirement.

## Known limitations / next steps

- MCU->Linux only (CAN RX path). Linux->MCU (inject CAN TX via MOSI command
  blocks) not done -- the slave currently ignores the master's MOSI bytes.
- NO RDY-line flow control (PG13/gpiochip1:70). Master free-polls; CAN can
  outrun it and ship_msgq drops frames silently. Add: MCU raises RDY when
  ship_msgq non-empty; Linux reads on the GPIO edge. Required before any
  no-loss-under-load claim.
- Classic CAN only (<=8 byte). CAN-FD (64-byte/BRS) needs bigger records/blocks
  (the 16-byte record holds only 8 data bytes). Bridge throughput math assumed FD.
- No CRC on the block; reader doesn't yet detect dropped blocks via seq gaps.

## Reproduce

1. build:  west-builder/build.sh -p /apps/can_spi_bridge
2. flash:  adb push build/zephyr/zephyr.bin -> /home/root/zephyr-flash/incoming.bin
           then adb exec-out /home/root/zephyr-flash/oo/recover.sh (boot into flash)
3. read:   adb push host-tools/can_spi_read.py;
           adb exec-out 'python3 /home/root/can_spi_read.py /dev/spidev0.0 20 0.05 1000000'
4. MCU cross-check: openocd mdw on g_can_rx/g_frames_packed/g_blocks_sent
   (FRESH addrs from `arm-zephyr-eabi-nm zephyr.elf` -- they move every rebuild;
   reading stale addrs gives garbage/zeros, learned the hard way).
