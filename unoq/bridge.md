# Getting data off the MCU: bridge/router design

Goal: once CAN works on the STM32 (FDCAN in loopback first, real bus later),
get CAN frames out to the QCM2290 Linux side (and northbound to network/tools).
This documents how the existing unoq-mcu project does it and what we inherit
vs. rebuild for mainline Zephyr.

## Key correction: it's UART, not SPI

The schematic (and the zephyr-mainline pin map) name SPI3 lines
(MCU_SPI3_MOSI/CS/RDY), and that shaped the "router talks over spidev"
expectation. But the **production unoq-mcu-app transport is UART**, not SPI:

- Linux side opens **`/dev/ttyHS1` @ 115200 8N1** (QCM GENI serial), via
  `go.bug.st/serial`. Source: `unoq-mcu-app/mcu-router/main.go` openSerial().
  Fallback probe order: ttyHS0, ttyHS1, ttyHS2, ttyS0.
- MCU side: STM32U585 **LPUART1** (PB6 TX / PB7 RX), through Arduino's
  `Arduino_RouterBridge` library.
- The only SPI3 pin actually wired in is **PG13 / MCU_SPI3_RDY**
  (`gpiochip1:70`) — and it's used purely as a **boot gate** for the Arduino
  loader animation (Linux holds it HIGH via `gpioset` so the loader stops
  animating and LLEXT-loads the sketch). SPI3 MOSI/CS are reserved-but-unused.

So there is **no spidev data path today.** We have two options for mainline
Zephyr (see "Decision" below): reuse the proven UART path, or actually build
the SPI path the schematic implies.

## The existing router (UART + msgpack-RPC)

Component: **`mcu-router`** (Go), in `unoq-mcu-app/mcu-router/`:

| File | Role |
| --- | --- |
| `main.go` | serial open (`/dev/ttyHS1` @115200, 100ms read timeout) |
| `router.go` | msgpack-RPC dispatch core |
| `handlers_monitor.go` | `mon/*` console I/O |
| `handlers_system.go` | `$/version`, `$/reset`, `$/register` |
| `handlers_tcp.go` / `handlers_udp.go` | socket bridging |
| `bridge_proxy.go` | northbound HTTP API |
| `pattern_state.go` | sticky-call persistence |

### Wire protocol — msgpack-RPC (self-delimiting, no length prefix/CRC/COBS)

```
Request:  [0, msgid, "method", [args...]]
Response: [1, msgid, error_or_nil, result]
Notify:   [2, "method", [args...]]
```

Constants (`router.go`): msgRequest=0, msgResponse=1, msgNotify=2; error codes
errOK=0/errGeneric=1/errParsing=2/errNotFound=3 (from Arduino_RPClite/error.h).
Encoder uses `UseArrayEncodedStructs(true)` / `UseLooseInterfaceDecoding(true)`.
All transport writes serialized behind a `writeMu` mutex; each inbound message
dispatched on its own goroutine.

MCU-side API (Arduino_RouterBridge): `Bridge.call(method, args...)` (blocking
request), `Bridge.notify(...)` (fire-and-forget), `Bridge.provide(name, fn)`
(register a handler Linux can invoke). `Monitor.*` rides the same link as
`mon/*` messages; `Monitor.begin()` blocks until `mon/connected` handshake.

### Northbound (laptop/network)

`bridge_proxy.go` serves HTTP on `:8082` (`MCU_ROUTER_HTTP_ADDR`):
- `POST /api/bridge/call`  `{method, args, timeout_ms}` -> `{ok, result|error}`
- `GET  /api/bridge/state` last sticky call (or null)
- `POST /api/bridge/clear` forget sticky state

Byte payloads in JSON use a `"base64:"` string prefix (JSON has no bytes type).
Sticky methods (set_frame/play_animation/set_pattern) are cached to
`/run/unoq/last_pattern.json` and auto-replayed on MCU reboot.

## What we inherit vs. rebuild for mainline Zephyr

We do NOT get Arduino_RouterBridge (that's the Arduino loader runtime, which
mainline Zephyr replaces wholesale). On the Zephyr side we implement the MCU
end ourselves. The Linux-side `mcu-router` Go code is **mostly reusable** if we
keep msgpack-RPC framing — it's transport-agnostic above the `io.ReadWriteCloser`.

### Decision needed: UART vs SPI transport for CAN data

**Option A — UART (`/dev/ttyHS1`) + msgpack-RPC.** Reuse the production path.
- Zephyr side: enable LPUART1 in the board DTS (already present:
  `&lpuart1 ... current-speed = <115200>`), write/port a small msgpack-RPC
  endpoint that `Bridge.provide()`s a `can/*` namespace and `notify`s RX frames.
- Pros: proven transport, mcu-router reuse, no new wiring, console + data share
  one link. Cons: 115200 is ~11 KB/s — fine for moderate CAN, tight for a
  saturated 1 Mbit bus dumping every frame; would need framing care / higher
  baud.

**Option B — SPI (spidev) as the schematic implies.** Build the real SPI path.
- Zephyr side: SPI **slave/peripheral** on SPI3 (DTS has
  `&spi3 ... pinctrl spi3_sck_pg9 spi3_miso_pg10 spi3_mosi_pb5 spi3_nss_pg12`),
  Linux side opens `/dev/spidevX.Y` as **master**, RDY GPIO (`gpiochip1:70`) as
  a data-ready interrupt/handshake so the MCU can signal "I have a frame".
- Pros: much higher throughput, RDY line gives proper flow control for
  bursty CAN RX. Cons: SPI-slave on STM32 + a spidev master protocol is new
  code on both sides; no existing router speaks spidev; more work.

Recommendation to discuss: **start with Option A (UART)** to get CAN frames
flowing end-to-end with minimal new code and the existing router, then move to
Option B if/when CAN throughput demands it. The RDY GPIO and SPI3 pins remain
available for the upgrade.

## DECISION: Option B (SPI). Recon results (2026-05-28)

### QCM master side already exists AND is purpose-built for the MCU
- `/dev/spidev0.0` is live; `spidev` module loaded.
- Controller: GENI SE `spi@4a94000` (under `geniqup@4ac0000`), spi0, CS0.
- DT child node `mcu@0`, **compatible = `arduino,unoq-mcu`**, modalias
  `spi:unoq-mcu`. This is NOT the generic `rohm,dh2228fv` dummy — it's a
  named, purpose-built MCU spidev. Strong confirmation this IS the QCM<->STM32
  SPI link, deliberately wired by the board's DT.
- `spi-max-frequency`: DT-capped (PoC notes ~1 MHz; confirm exact value via
  SPI_IOC_RD_MAX_SPEED_HZ). 1 MHz ~= 125 KB/s, ~10x the UART path. May raise
  via SPI_IOC_WR_MAX_SPEED_HZ but the DT child cap likely bounds it.
- So NO kernel/DT work needed on the Linux side to get a master spidev — it's
  already there, named for us.

### Authoritative STM32 SPI3 pins (Zephyr board DTS is source of truth)
`arduino_uno_q-common.dtsi:137-141`:
  SCK=PG9, MISO=PG10, MOSI=PB5, NSS=PG12  (all AF6).
The PoC README's PB4(MOSI)/PG11(CS) map is WRONG — ignore it. RDY=PG13 still
correct (= QCM gpiochip1:70).

### Direction
STM32 SPI3 = SLAVE/peripheral, QCM `/dev/spidev0.0` = MASTER.

### Zephyr STM32U5 SPI-slave support
Exists: CONFIG_SPI=y, CONFIG_SPI_SLAVE=y, CONFIG_SPI_STM32_INTERRUPT=y,
CONFIG_GPIO=y, CONFIG_PINCTRL=y. Caveat: community reports rough edges on U5
slave mode (HAL_SPI_ERROR_FLAG 0x20). Validate the slave path early with a
trivial echo before layering CAN on top.
  ST community: stm32u5-spi-slave-not-working-zephyr
  Zephyr SPI peripheral docs: docs.zephyrproject.org/latest/hardware/peripherals/spi.html

## Option B design (SPI bridge)

Master (QCM) drives every transfer (SPI is master-clocked), so the MCU can't
"push" asynchronously. Use the **RDY (PG13 / gpiochip1:70) line as a data-ready
interrupt**: MCU raises RDY when it has CAN frame(s) queued; Linux sees the GPIO
edge and clocks a transfer to drain them. For Linux->MCU (TX CAN frames) the
master just initiates a transfer whenever it wants.

Framing over SPI (need our own, SPI has no message boundaries): fixed-size
frame slots. Proposal: 1 SPI transfer = 1 fixed N-byte block:
  [magic:1][type:1][len:1][seq:1][payload:len][pad...][crc:2]
type = {can_tx, can_rx, ping, ...}; payload for CAN = {id:4, dlc:1, flags:1,
data:0..64}. Fixed block keeps slave DMA simple. (Refine once slave echo works.)

Zephyr side (new code):
- board overlay: `&spi3` already okay+pinned as slave-capable; add
  `cs-gpios`/slave config; enable CONFIG_SPI_SLAVE + interrupt; FDCAN node
  overlay for the CAN peripheral (separate task).
- app: SPI-slave RX/TX using zephyr spi async; CAN driver in loopback first;
  glue that packs RX frames into SPI blocks and raises RDY (GPIO out PG13).

Linux side (reuse mostly): a small Go service opening /dev/spidev0.0 + polling
the RDY gpio (gpiochip1:70 as input/IRQ via gpiod). Can keep mcu-router's
msgpack-RPC ABOVE the SPI block framing, or go raw. Northbound HTTP/WS as before.

## Build order (Option B)
1. Validate Zephyr STM32U5 SPI-slave with a trivial loopback/echo (de-risk the
   known-rough slave mode) — master = a quick spidev_test from Linux.
2. FDCAN in loopback on the MCU (no SPI yet) — prove CAN frames TX/RX on-chip.
3. Wire the two: CAN-loopback frames -> SPI blocks -> Linux reads them.
4. RDY-line flow control for bursty RX.
5. Northbound + (optional) msgpack-RPC reuse.

## Decisions (2026-05-28)
1. Target = SATURATED 1 Mbit CAN bus. Worst case classic CAN ~= up to ~8k
   frames/s; with overhead a frame is ~111-130 bits, so ~7.5-9k frames/s, each
   up to 8 data bytes. As a fixed SPI block (~16-24B incl header/crc) that is
   ~150-220 KB/s. The DT spi-max-frequency cap is 1 MHz (125 KB/s) which is
   NOT enough at full saturation -> MUST (a) try raising SPI clock above the
   1 MHz DT child cap (SPI_IOC_WR_MAX_SPEED_HZ; if capped, patch the DT child
   / use a different spidev instantiation), AND (b) BATCH many CAN frames per
   SPI transfer (burst-drain on RDY, not one-frame-per-transfer). FDCAN-FD with
   64B payloads raises throughput needs further. Design framing for batching
   from day one.
2. KEEP a Zephyr shell on LPUART1 (/dev/ttyHS1) during bringup — interactive
   debugging through the known-rough U5 SPI-slave mode. zephyr,shell-uart is
   already &usart1 in the board dts; we get shell ~free. (Console currently
   usart1 PB6/PB7; LPUART1 is the other option — confirm which is /dev/ttyHS1.)
3. Framing: lean binary block w/ batching (see above), msgpack-RPC reuse is
   optional and secondary; raw blocks first for throughput.
