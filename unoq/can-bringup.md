# FDCAN loopback bring-up (step 2) — PASSED

Date 2026-05-28. Proved the STM32U585 FDCAN peripheral TX/RX works entirely
on-chip in CAN_MODE_LOOPBACK (no transceiver, no bus wiring). This is the
on-MCU CAN half of the bridge; next step pipes these frames out over SPI.

## App

`apps/can_loopback/` — sets CAN_MODE_LOOPBACK, can_start, registers an
accept-all rx filter into a msgq, sends one frame/sec (id 0x123, data
DE AD BE <counter>), drains the msgq. Green-LED heartbeat thread.

## Board overlay (apps/can_loopback/boards/arduino_uno_q.overlay)

The fdcan1 node exists in `zephyr/dts/arm/st/u5/stm32u5.dtsi`
(`st,stm32-fdcan`, can@4000a400) but is `status = "disabled"` and the UNO Q
board never enables it. Overlay enables it:

```dts
/ { chosen { zephyr,canbus = &fdcan1; }; };
&fdcan1 {
    clocks = <&rcc STM32_CLOCK(APB1_2, 9)>,
             <&rcc STM32_SRC_PLL1_Q FDCAN1_SEL(1)>;   /* kernel clock */
    pinctrl-0 = <&fdcan1_rx_pd0 &fdcan1_tx_pd1>;
    pinctrl-names = "default";
    status = "okay";
};
```

Key points:
- FDCAN needs a SECOND clock entry (the kernel clock). PLL1_Q works and the
  board already enables &pll1 with div-q=<2>. (Same pattern as
  boards/st/nucleo_u5a5zj_q.)
- Pins PD0(RX)/PD1(TX) are free on the UNO Q (PA11/PA12 are USB, PB9 is spi2
  nss). For on-chip loopback the pins aren't physically used but the binding
  requires pinctrl-0.
- prj.conf: CONFIG_CAN=y, CONFIG_CAN_STM32_FDCAN=y, CONFIG_GPIO=y,
  CONFIG_PINCTRL=y.

## Verification = SWD-readable globals (NOT UART)

The QCM console-UART path is unverified on this board: the board console is
`usart1` on PB6/PB7 and `/dev/ttyHS1` is free on the QCM, but a 4s `cat` of
ttyHS1 got 0 bytes — so usart1's pins may not route to the QCM serial (or need
different config). Rather than chase that, the app exposes proof state in RAM
and we read it with openocd `mdw`. This is a reliable UART-free verification
pattern for all the bridge bring-up.

Globals (addresses from `arm-zephyr-eabi-nm zephyr.elf`, will move on rebuild):
g_state, g_tx_count, g_rx_count, g_rx_match_count, g_last_rx_id, g_last_rx_data.

Result after ~6s (this run, addrs 0x200007xx):
- g_state          = 0x3        (running: mode set, started, filter ok)
- g_tx_count       = 0x0e (14)
- g_rx_count       = 0x0e (14)  -- every TX looped back
- g_rx_match_count = 0x0e (14)  -- all matched expected id+data
- g_last_rx_id     = 0x123      (= TX_ID)
- g_last_rx_data   = 0xdeadbe0d (DE AD BE + counter 0x0d)

tx == rx == match => FDCAN loopback fully working.

How to re-read (after flashing + recover.sh to boot):
```
cd /home/root/zephyr-flash/oo
LD_LIBRARY_PATH=./lib ./bin/openocd -s ./share/openocd/scripts -f ./unoq-swd.cfg \
  -c init -c halt -c "mdw <g_addr> 1" ... -c shutdown
```
(get fresh <g_addr> from nm after each rebuild.)

## TODO / open
- Resolve the UART console path (would make iteration nicer) OR keep SWD-globals
  verification. Decided: shell-on-LPUART1 still wanted for the SPI bridge debug;
  need to confirm which QCM tty maps to which STM32 UART. usart1(PB6/PB7) gave
  nothing on /dev/ttyHS1 — investigate when we add the shell.
- CAN FD (BRS, 64-byte payloads) not yet tested — classic CAN only so far. The
  bridge throughput math assumed FD; revisit.

## Next: wire CAN frames over SPI (task #9)
Pack looped-back CAN frames into fixed-size SPI blocks (size matched both ends,
per spi-bringup.md), drain to /dev/spidev0.0, batch for throughput, RDY line
(PG13/gpiochip1:70) for flow control.
