# Bidirectional CAN<->SPI bridge with RDY flow control (step 4) — PASSED

Date 2026-05-28. Extended the one-way bridge to full-duplex with hardware flow
control. Linux can now INJECT CAN frames (Linux->MCU->CAN) and the RDY line
gates reads so nothing is dropped. Proven lossless at 200 frames (> queue depth).

## What changed vs the one-way bridge

- Same fixed 64-byte block (ver bumped to 0x02), now used in BOTH directions:
  - MOSI (Linux->MCU): command block; MCU parses it after every transfer and
    can_send()s each record. CAN is in loopback so injected frames come back.
  - MISO (MCU->Linux): up to 3 looped-back RX frames per block.
- RDY flow control: STM32 PG13 == QCM gpiochip1:70. MCU drives it HIGH while
  there is unread data for the master; Linux reads RDY (libgpiod gpioget) and
  only clocks a transfer when data is ready.
- No free-running CAN generator: every frame originates from a Linux inject, so
  accounting is exact (injected == looped == packed == read).

## Two bugs found and fixed during bringup

1. Linux driver: collected MISO blocks ONLY during the drain phase, but every
   transfer is full-duplex -- the MISO returned while injecting already carries
   looped-back frames. Fix: collect the MISO return on EVERY transfer. (Symptom:
   read 3/9.)
2. Firmware RDY semantics: RDY reflected only ship_msgq emptiness, but a block
   already packed into spi_tx (staged, not yet clocked out) is also unread data.
   Master stopped one read early and missed the final block. Fix: RDY high if
   (staged block has frames) OR (queue non-empty). (Symptom: read 8/9.)

## Result (REAL, cross-checked both sides)

9-frame run:  injected 9, read 9, missing 0, extra 0  -> PASS
200-frame run (> ship_msgq depth 64, 67 MOSI blocks, exercises flow control):
  Linux: injected unique 200, read unique 200, missing 0, unexpected 0 -> PASS
Both sides agree: zero loss, zero duplication. (MCU SWD globals confirmed equal
on the prior 9-frame run: injected==can_rx==packed; rdy_level returns to 0.)

## RDY pin facts
- STM32 PG13, free on the UNO Q board (not used by SPI3/fdcan/LEDs). gpiog
  enabled. Exposed to the app via the zephyr,user node: rdy-gpios = <&gpiog 13>.
- Linux side gpiochip1:70, free (input, no consumer). Read with libgpiod v3
  gpioget -c /dev/gpiochip1 70 (prints "70"=active/inactive).

## Files
- MCU: apps/can_spi_bridge/{src/main.c, prj.conf, boards/arduino_uno_q.overlay}
- Linux: host-tools/can_spi_bridge.py (pure builtins; keep open() obj alive or
  fd GCs -> EBADF; RDY via subprocess gpioget)

## Known limitations / next
- 1 MHz SPI, classic CAN (<=8 byte). CAN-FD (64B/BRS) needs bigger records.
- No block CRC; no seq-gap detection on the reader.
- Throughput not yet measured under sustained max-rate; flow control proven
  correct but raw frames/sec TBD.
- Loopback only (no transceiver/real bus). Real bus = wire a CAN transceiver to
  PD0/PD1 + a second node.
