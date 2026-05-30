# SPI-slave bring-up results (step 1, de-risk) — PASSED

Date 2026-05-28. Proved the STM32U585 SPI-slave path end-to-end before building
the CAN bridge. This is the foundation the CAN<->SPI transport sits on.

## What was tested

- MCU app: `apps/spi_echo/` — `&spi3` in SLAVE mode, one blocking
  `spi_transceive` of BUF_LEN=32 bytes in a loop; after each completes it
  `memcpy(tx, rx)` so the NEXT transfer echoes what the previous one received.
  Preloads a `0xA0..0xBF` banner. Green-LED heartbeat thread for liveness.
- Build: `west-builder/build.sh -p /apps/spi_echo` (33 KB bin).
- Flash: pushed to the systemd watcher (`incoming.bin`) -> auto-flashed ->
  `recover.sh` to boot into flash.
- Master: Linux `/dev/spidev0.0` driven by `host-tools/spidev_xfer.py`
  (pure-python raw ioctl SPI_IOC_MESSAGE; the device python has no `spidev`
  module and no `ctypes`, so it uses `array.buffer_info()` for buffer addrs).

## Result

Matched 32-byte transfers, mode 0, 1 MHz:
- xfer A (32x 0x11) -> RX = a0a1...bebf   (full preload banner)   OK
- xfer B (32x 0x22) -> RX = 11...11        (echo of xfer A)        OK
- xfer C (32x 0x00) -> RX = 22...22        (echo of xfer B)        OK

SPI slave works. The community-reported U5 slave roughness did NOT bite us in
this basic full-duplex path. spi_transceive with SPI_OP_MODE_SLAVE blocks until
the master clocks the full buffer, then returns.

## CRITICAL design fact for the CAN framing

The STM32 SPI slave transfer is **byte-count driven, NOT chip-select driven.**
Evidence: when the master did 8-byte transactions against the slave's 32-byte
`spi_transceive`, the slave did NOT complete/reset on NSS deassert — it kept
draining its 32-byte buffer across four 8-byte master transactions
(a0..a7, a8..af, b0..b7, b8..bf) and only ran the echo memcpy after all 32
bytes were clocked.

Implications for the CAN<->SPI protocol:
1. **Fixed frame size on BOTH ends.** Master and slave MUST agree on the exact
   transfer length. A short master transfer leaves the slave mid-buffer and
   desyncs everything after it. Pick one block size (e.g. N bytes) and always
   transfer exactly N.
2. NSS/CS is for selection/timing, not message framing — do our own framing
   inside the fixed block (magic/type/len/seq + payload + crc), and/or batch
   multiple CAN frames per block.
3. The one-transfer latency (slave reacts on the NEXT transfer, not the current
   one) is inherent to SPI-slave: design the protocol so the master polls /
   the RDY line tells it when a block is ready to read.

## Config that worked (apps/spi_echo/prj.conf)

CONFIG_SPI=y, CONFIG_SPI_SLAVE=y, CONFIG_SPI_STM32_INTERRUPT=y, CONFIG_GPIO=y,
CONFIG_PINCTRL=y, plus console/log. spi_config:
`SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB`, .slave=0, mode 0.
No overlay needed — board `&spi3` is already `okay` with SCK=PG9/MISO=PG10/
MOSI=PB5/NSS=PG12; the STM32 driver sets NSS HARD_INPUT in slave mode (uses the
hardware PG12 CS the QCM master drives).

## Next
- FDCAN loopback on the MCU (no transceiver).
- Then pack CAN frames into fixed SPI blocks and drain to /dev/spidev0.0,
  batching for throughput, RDY line for flow control.
