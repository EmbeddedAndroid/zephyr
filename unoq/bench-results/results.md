# CAN<->SPI bridge benchmark log

Methodology: host-tools/bench.py. Full-duplex transfers in a tight loop (no
sleeps, no per-transfer gpioget). Warmup ~300 frames, then timed until TARGET
frames read back. 5 runs, median reported. CAN in loopback. time.monotonic().

| Phase | block | recs/blk | payload | SPI clk | frames/s | payload kB/s | blocks/s | notes |
|-------|-------|----------|---------|---------|----------|--------------|----------|-------|
| Baseline (classic CAN) | 64B | 3 | 8B | 1 MHz | 750 | 6.0 | 1500 | 0.5 frames/block returned; wire ~96kB/s (77% of 125kB/s ceiling) |
| CAN-FD (64B/BRS) | 256B | 3 | 64B | 1 MHz | 671 | 43.0 | 447 | +7.2x payload kB/s vs baseline. 64B frames verified round-trip. block 4x bigger so blocks/s down, but 8x data/frame wins. |
| + CRC-16 + seq-gap | 256B | 3 | 64B | 1 MHz | 438 | 28.0 | 146 | integrity verified: corrupt block -> g_rx_crc_err++ + dropped (5/5), good 15/15 injected. crc_err=0 on bench. seq_gap=4 EXPECTED (host can't read every block MCU makes -> legit seq jumps, not corruption). COST: -35% from per-transfer pure-python CRC-16 over 256B x2 (build+verify); firmware CRC is cheap. -> prime optimization target. |
| opt: fast CRC (1MHz) | 256B | 3 | 64B | 1 MHz | 666 | 42.6 | 444 | binascii.crc_hqx (C CRC-16/CCITT init 0xFFFF) replaces pure-python loop; matches firmware bit-for-bit. Recovers nearly all CRC cost (438->666). crc_err=0. |
| opt: 2 MHz SPI | 256B | 3 | 64B | 2 MHz | 845 | 54.1 | 824 | clock sweep: 1-3MHz clean (crc_err=0); 4MHz+ collapses (slave IRQ-mode FIFO underruns, valid blocks=0). 2MHz chosen as safe max. |
| OPTIMIZED FINAL | 512B | 7 | 64B | 2 MHz | 1040 | 66.5 | 446 | 512B/7rec amortizes per-transfer ioctl+CRC+slave-CPU. crc_err=0. MEASURED median of 5 runs (<1% spread). |

## Optimization summary (all MEASURED on hardware)
- Fast CRC (binascii.crc_hqx): +52% @1MHz (438->666 fps). Host pure-python CRC-16
  was the +CRC bottleneck; firmware CRC is cheap.
- SPI clock 1->2 MHz: +27% (666->845 @256B). Slave is IRQ-mode (no DMA); clean
  to ~3MHz, collapses at 4MHz+.
- Block 256B/3rec -> 512B/7rec: +23% (845->1040). Amortizes per-transfer overhead.

## Headline deltas (MEASURED)
- Frame rate:  baseline 750 -> final 1040 = 1.39x (while ADDING FD+CRC+integrity)
- Payload throughput: 6.0 -> 66.5 kB/s     = 11.1x
- vs the un-optimized +CRC low point (28.0): 2.4x

## Proven vs not
- INTEGRITY (crc_err=0): every FD/opt config. Corrupt-block test: 5/5 rejected
  (g_rx_crc_err+=5), 15/15 good injected.
- LOSSLESSNESS (RDY): proven separately by RDY-gated test (200 frames > 64 queue
  depth, 0 missing/0 extra, MCU counters equal). Benchmark uses a saturated
  non-RDY sampler so its seq_gap>0 is EXPECTED, not loss.
- CAN-FD 64B payload: 12 frames byte-exact round-trip.
- Real CAN bus: NOT tested (loopback only, no transceiver wired).
