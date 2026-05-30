# Mainline Zephyr 4.4.0 on the Arduino UNO Q — revival notes

A previous PoC session (2026-05-27) built mainline Zephyr's
`samples/basic/blinky` for the in-tree `arduino_uno_q` board target and ran
it on real hardware. The build artifact is preserved, but the west
workspace and exact build commands were not. This doc is for the agent who
revives that work.

The current production stack (`unoq-mcu-app`) deliberately uses Arduino's
prebuilt `arduino-zephyr` loader + LLEXT sketches instead, because it gives
us the Arduino runtime (LED matrix driver, Bridge API). Mainline Zephyr is
a separate path — pick when you want full Zephyr APIs (sensors, networking,
threading, BLE host stack, etc.) and don't need Arduino's pieces.

---

## What was proven to work

`backups/unoq-zephyr-blinky-v1.bin`

- Source: mainline **Zephyr 4.4.0**, sample `samples/basic/blinky`
- Board target: in-tree `arduino_uno_q` (Zephyr added support upstream)
- Toolchain: built natively on aarch64 — no x86_64 cross or QEMU
- Output: 18,588 bytes, sha256 `d141ac10…34cdf`
- Flash slot: `0x08000000` — replaces the Arduino loader entirely
- Verified: blinked the green LED end-to-end on a UNO Q with `BOOT0` LOW at
  reset; no PG13 hold required.

The binary still exists at the path above. You can flash it any time to
sanity-check the SWD path before redoing the build.

---

## Why this is *not* "load a sketch" — important architectural difference

| | Arduino path (today) | Mainline Zephyr path |
| --- | --- | --- |
| Flash at `0x08000000` | Arduino loader (`loader-arduino-zephyr-0.55.2.bin`, 225 KB) | Your Zephyr app (18 KB+) — *the whole firmware* |
| Flash at `0x08100000` | Sketch LLEXT slot (replaceable without reboot) | Unused |
| Boot gate | Loader waits for `gpiochip1:70` (PG13) HIGH from Linux side, then LLEXTs sketch | None — `main()` runs from reset |
| Linux↔MCU RPC | Arduino_RouterBridge over LPUART1 ↔ `/dev/ttyHS1` | You implement it — Zephyr shell, custom UART protocol, or USB-CDC |
| LED matrix | Arduino driver (charlieplexed via STM32 PF0–PF10, hw timer refresh) | No upstream driver — you'd write/port one |
| Rapid iteration | Re-flash 0x08100000 only (~5s), no reboot | Re-flash 0x08000000, full reset every time (~5–10s) |

The big tradeoff: mainline Zephyr gives you the upstream API surface
(sensors, net, threading, BLE host, settings, etc.) but you lose the
Arduino LED matrix, Bridge API, and the "swap sketch without rebooting"
LLEXT trick. Mix-and-match isn't possible — it's one or the other in the
loader slot.

---

## Hardware facts you'll need

QCM2290 ↔ STM32U585 pin map (verified from ABX00162 schematic + bench):

| STM32 net | STM32 pin | QCM line | Used for |
| --- | --- | --- | --- |
| SWDIO | PA13 | `gpiochip1:25` | SWD data |
| SWCLK | PA14 | `gpiochip1:26` | SWD clock |
| BOOT0 | PH3 | `gpiochip1:37` | LOW during reset = boot from flash; HIGH = ROM bootloader |
| NRST | PG14 | `gpiochip1:38` | SRST line, pulsed by `openocd reset run` |
| LPUART1 TX (QCM→MCU) | PB6 | `gpiochip1:71` | Console RX path; not wired in PoC |
| LPUART1 RX (MCU→QCM) | PB7 | `gpiochip1:80` | Console TX path; not wired in PoC |
| PG13 (MCU_SPI3_RDY) | PG13 | `gpiochip1:70` | **Arduino-loader-only gate.** Not needed for mainline Zephyr. |
| SPI3 MOSI | PB4 | `gpiochip1:15` | Reserved for Arduino RPC channel |

MCU details (from openocd probe):

- STM32U585xx, Cortex-M33 r0p4
- 2 MB internal flash, dual-bank, base `0x08000000`
- IDCODE `0x30076482`
- RDP level 0 (fully read/write accessible)
- TrustZone disabled

---

## What's preserved + reusable

All paths below are under `/home/tyler/Dev/claude/unoq-mcu-poc/` unless
noted.

| Artifact | Path | Notes |
| --- | --- | --- |
| Working mainline blinky binary | `backups/unoq-zephyr-blinky-v1.bin` | Sanity-check artifact. `openocd flash write_image erase …` to deploy. |
| Original Arduino loader (rollback) | `backups/loader-arduino-zephyr-0.55.2.bin` | Restore-to-Arduino path. Sha256 `1b765765…d8e22`. |
| Factory flash dump | `backups/stm32-flash-original-…bin` | Full 2 MB capture from 2026-05-27. |
| OpenOCD SWD configs | `unoq-runtime/unoq-swd*.cfg` | `linuxgpiod` adapter glue + pin mapping. Drop into any openocd run. |
| OpenOCD build recipe | `openocd-build/Dockerfile` | aarch64 openocd + libgpiod v2 + libjim. Same one we ship in `mcu-control`. |
| Flash flow source code | `/home/tyler/Dev/claude/unoq-mcu-app/mcu-control/openocd.go`, `loader.go` | `runOpenOCD`, `runBOOT0Hold`, the exact `flash write_image erase …  0x08000000` invocation. Production-tested. |

---

## What's NOT preserved (and needs reconstructing)

- The west workspace. No `west.yml`, no `manifest.yml`, no toolchain pin.
  Step 1 of revival is `west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.0` (or whatever the
  proper manifest URL is) and confirming `arduino_uno_q` resolves.
- Zephyr SDK pin. Mainline Zephyr 4.4.0 wants SDK 0.17.x; not the 0.16.x
  that Arduino-zephyr 0.55.2 bundles. The PoC built natively on aarch64,
  so the SDK install command is whatever Zephyr's getting-started.html
  recommends for arm64 hosts as of the 4.4.0 release.
- The exact `west build` command. Almost certainly:
  ```sh
  west build -b arduino_uno_q zephyr/samples/basic/blinky
  ```
  Output: `build/zephyr/zephyr.bin`. Then flash that to `0x08000000`.

---

## Revival recipe (proposed order)

1. **Confirm artifact still flashes.** Boot a UNO Q. From the laptop:
   ```sh
   ssh fio@<device-ip>
   sudo docker exec unoq-mcu-app-mcu-control-1 \
     openocd -f /cfg/cfg.cfg \
       -c "init; reset halt;
           flash write_image erase /opt/unoq-zephyr-blinky-v1.bin 0x08000000;
           reset run; shutdown"
   ```
   You'll need to first `scp` the .bin into the container and adjust the
   path. Or use mcu-control's `/api/flash/loader` endpoint with the
   binary as the body. Look for the green LED to blink. Once blinking
   confirmed, the SWD + flash path is good and the problem is purely
   "rebuild this artifact from source."

2. **Rebuild the SDK + workspace** on the aarch64 laptop:
   ```sh
   mkdir -p ~/zephyrproject && cd ~/zephyrproject
   pip install west
   west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.0
   west update
   west zephyr-export
   pip install -r zephyr/scripts/requirements.txt
   # SDK install — follow getting_started for v4.4 on linux-aarch64
   wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-aarch64.tar.xz
   tar xf zephyr-sdk-0.17.0_linux-aarch64.tar.xz
   cd zephyr-sdk-0.17.0 && ./setup.sh
   ```
   (Version numbers above may need bumping — check zephyr docs at the time.)

3. **Sanity build:** `west build -b arduino_uno_q zephyr/samples/basic/blinky`.
   Compare the resulting `build/zephyr/zephyr.bin` size against the
   archived `unoq-zephyr-blinky-v1.bin` (18,588 B) as a smoke check.

4. **Flash + verify** via the same `mcu-control` SWD path. Green LED
   blinks → revival complete.

5. **Capture state**: drop a `west.yml` + commit-pinned SDK version into a
   new repo (alongside this PoC or in a new project dir) so future agents
   don't redo the archaeology.

---

## Known gaps / decisions to make

1. **No upstream LED matrix driver.** UNO Q's 12×8 (actually 13×8 per
   sketch agent's correction) is charlieplexed across STM32 PF0–PF10 with
   a hardware-timer-driven refresh. Arduino's `loader/matrix.inc` is the
   only working implementation. Options if your new project needs the
   matrix:
   - Port Arduino's driver to Zephyr (medium effort, would also be a
     useful upstream contribution).
   - Use `display_*` subsys with a custom driver shim.
   - Skip the matrix; drive other GPIO/LEDs.

2. **No Linux↔MCU RPC.** mainline Zephyr boots with no equivalent of
   Arduino_RouterBridge. Options:
   - Wire LPUART1 to `/dev/ttyHS1` via a board overlay and run Zephyr's
     shell over it — gives you an interactive console at minimum.
   - Roll a custom UART protocol matching whatever your project needs.
   - Use USB-CDC (UNO Q's USB-C goes to the QCM side, not the MCU, so
     this is non-trivial — would need GPIO-driven bit-banged USB on a
     spare interface).

3. **`LED_BUILTIN` mapping.** In `arduino:zephyr:unoq` it maps to PH10
   (LED3 RED). Verify the mainline `arduino_uno_q.dts` aliases what you
   expect — `led3_green` was used directly in the PoC to avoid confusion.

4. **No PG13 dance needed.** mcu-rdy on the device exists solely for the
   Arduino loader's boot-animation gate. If you're flashing mainline
   Zephyr at `0x08000000`, you can run *without* mcu-rdy — the Zephyr
   image boots straight from reset. Decide whether to keep mcu-rdy
   running (harmless, just an idle process) or remove from the compose-app
   for that fleet.

5. **BOOT0 self-heal at boot.** Mainline Zephyr or Arduino, you need
   `mcu-control`'s `recoverMCU()` step (forces BOOT0 LOW + SWD reset at
   container start) — otherwise a cold boot where BOOT0 floats HIGH
   silently lands the MCU in ROM bootloader. That fix is already in
   `mcu-control` as of `uno-q-10` and is portable.

---

## Pointers

- **PoC writeup**: `/home/tyler/Dev/claude/unoq-mcu-poc/README.md` (full
  end-to-end build + flash flow from the original session)
- **PoC handoff backlog**: `/home/tyler/Dev/claude/unoq-mcu-poc/HANDOFF.md`
  (productionization gaps — most have since been closed in `unoq-mcu-app`)
- **Production flash code**: `/home/tyler/Dev/claude/unoq-mcu-app/mcu-control/`
  (`openocd.go`, `loader.go`, `handlers.go`)
- **Production-side OpenOCD config**: container image bakes it at
  `/cfg/cfg.cfg`; source at `mcu-control/unoq-swd-noreset.cfg`
- **Mainline Zephyr board source**: in upstream Zephyr at
  `boards/arduino/uno_q/` (path may have shifted by 4.4 — search the
  tree if not exact)
