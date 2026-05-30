# UNO Q mainline-Zephyr work (CAN/SPI bridge + LED matrix)

Snapshot of the out-of-tree work done on the Arduino UNO Q (QCM2290 +
STM32U585) with mainline Zephyr 4.4. This branch (`can-unoq`) is the Zephyr
**v4.4.0** release tag plus this `unoq/` directory, so nothing is lost.

These are out-of-tree Zephyr applications and a driver module kept here for
preservation. The build/flash tooling assumes the laptop environment they were
developed in (paths, the Windows-adb-connected UNO Q, the west-builder Docker
image) and is captured as-is.

## Apps (`apps/`)
- `spi_echo` — SPI-slave bring-up: STM32 SPI3 slave echo. Drove the discovery
  that the slave transfer is byte-count driven, not chip-select driven.
- `can_loopback` — FDCAN on-chip loopback bring-up.
- `can_spi_bridge` — bidirectional CAN-FD <-> SPI bridge with RDY flow control
  and CRC-16 block integrity (the optimized 64B/FD variant).
- `can_spi_bridge_n2k` — NMEA 2000 variant: classic CAN 2.0B, 250 kbit/s,
  29-bit extended IDs. `BENCH_LOOPBACK` switch for loopback vs real bus.
- `led_matrix` — demo for the charlieplex LED matrix display driver.

## Driver module (`modules/charlieplex-display/`)
Out-of-tree dev copy of the charlieplex LED matrix display driver (binding,
driver, Kconfig/CMake, ztest). The upstream-style version of this driver lives
on the separate `display-charlieplex-led-matrix` branch.

## Tooling
- `west-builder/` — Docker-wrapped `west build` (host side).
- `watcher/` — on-device systemd path-unit auto-flasher + boot-time MCU recover.
- `openocd-flasher/`, `openocd-native/` — SWD flasher (Docker recipe + native
  bundle config). The built ~15 MB openocd binary is intentionally NOT
  committed; rebuild from `openocd-flasher/Dockerfile`.
- `host-tools/` — Python SPI/CAN drivers, benchmarks, and test scripts (pure
  stdlib; the device Python lacks spidev/ctypes/argparse).

## Docs and results
Per-phase writeups (`flash.md`, `spi-bringup.md`, `can-bringup.md`,
`bridge-bidi.md`, `bridge.md`, `led-matrix.md`, `zephyr-mainline.md`), benchmark
logs (`bench-results/`), and the two PDF reports (`report/`, `report-n2k/`).
