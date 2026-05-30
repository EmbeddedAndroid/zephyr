# Flashing mainline Zephyr to the UNO Q STM32U585

End-to-end flow, proven 2026-05-28: build artifact -> `adb push` -> native SWD
flash -> BOOT0-hold reset -> running firmware (green LED blinking from
`samples/basic/blinky`). No docker on the device, no kernel panic.

## TL;DR

```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"
"$ADB" push build/zephyr/zephyr.bin /home/root/zephyr-flash/blinky.bin
"$ADB" exec-out 'cd /home/root/zephyr-flash/oo && ./flash.sh /home/root/zephyr-flash/blinky.bin 0x08000000'
```

`flash.sh` does write -> verify -> BOOT0-hold reset in one shot and leaves the
MCU running the new firmware.

## Why not docker on the device

The first plan was to ship an openocd container (`docker save | adb push |
docker load`). The image was fine (169 MB, under the ~300 MB UNO Q panic
limit) but **`docker load` itself kernel-panicked the board** — the
decompression memory spike on a 1.7 GB-RAM device is the hazard, not the
resident image size. The device rebooted clean but we abandoned the device-side
docker route entirely.

The builder side (compiling Zephyr with west) can still be docker-wrapped — that
runs on the host, not the board.

## The native flasher bundle

Self-contained, no rootfs installs required. Source lives at
`~/Dev/claude/zephyr-mainline/openocd-native/bundle/`, deployed to the device at
`/home/root/zephyr-flash/oo/`.

```
oo/
├── bin/openocd          # built --enable-linuxgpiod on debian-trixie (glibc 2.41)
├── bin/gpioset          # libgpiod v3 CLI, for the BOOT0 hold
├── lib/libgpiod.so.3    # openocd + gpioset only non-system deps...
├── lib/libjim.so.0.83   # ...for the linuxgpiod path (no libusb/libjaylink needed)
├── share/openocd/scripts/   # tcl target/interface scripts
├── unoq-swd.cfg         # SWD adapter + target config (see fixes below)
├── flash.sh             # write + verify + BOOT0-hold reset (one shot)
└── recover.sh           # BOOT0-hold reset only (boot a already-flashed image)
```

Why it runs with no installs: the device is qcom-distro 2.0 with **glibc 2.43**
(newer than the build host's 2.41, so the binary is forward-compatible) and
`libusb` already present. openocd links only `libgpiod.so.3` + `libjim.so.0.83`
for linuxgpiod, both shipped in `lib/`. `flash.sh`/`recover.sh` set
`LD_LIBRARY_PATH=$HERE/lib`.

Bundle is ~21 MB extracted / ~7 MB tgz — push+extract, no decompression spike.

## Hardware: pins and slot

SWD over the QCM2290's `/dev/gpiochip1` (linuxgpiod bitbang):

| Signal | gpiochip1 line | STM32 pin |
| --- | --- | --- |
| SWCLK | 26 | PA14 |
| SWDIO | 25 | PA13 |
| SRST  | 38 | PG14 |
| BOOT0 | 37 | PH3 |

MCU: STM32U585xx Cortex-M33 r0p4, IDCODE `0x30076482`, 2 MB dual-bank flash at
base `0x08000000`, RDP level 0, TrustZone disabled (TZEN=0).

**Flash slot for mainline Zephyr = `0x08000000`** — the whole firmware,
replacing the Arduino loader. (The Arduino path instead keeps a loader at
`0x08000000` and LLEXTs sketches at `0x08100000`; that is a different model.)

## Two non-obvious fixes (both mandatory, both baked into the bundle)

### 1. linuxgpiod has no configurable clock speed

The stock `target/stm32u5x.cfg` reset events call `adapter speed NNN`. The
linuxgpiod bitbang adapter doesn't support a settable speed, so those calls
abort with:

```
Error: Translation from khz to adapter speed not implemented
Error: [stm32u5x.cpu] Execution of event reset-start failed
```

...and `reset run` never completes (firmware written but MCU not reset into it).

Fix in `unoq-swd.cfg`: remove the `adapter speed` line, and after
`source [find target/stm32u5x.cfg]` add no-op overrides:

```tcl
$_TARGETNAME configure -event reset-start { }
$_TARGETNAME configure -event reset-init  { }
```

The SRST pulse from `reset run` still resets the MCU; we just skip the
speed-dependent event hooks.

### 2. BOOT0 hold is MANDATORY on this image

This is the one that cost us the "flash succeeds but no LED" confusion.

On the stock qcom-distro image there is **no mcu-rdy / recoverMCU process
holding BOOT0 LOW** (that exists only in the unoq-mcu-app compose stack). So
when openocd's closing `reset run` pulses NRST while BOOT0 floats HIGH, the MCU
latches BOOT0=HIGH and jumps into the **system ROM bootloader** instead of
running flash.

Symptom: write + verify both succeed, but the firmware never runs (no LED).
Proof — halt and read PC:

- ROM bootloader (bad):  `pc = 0x0bf979e6`  (0x0bf8xxxx system-memory region)
- Running flash (good):  `pc = 0x08002f7a`  (0x0800xxxx image region)

The vector table at `0x08000000` reads valid either way
(`20001090 08000c1d` = initial SP + reset handler), confirming the write is
fine — it is purely a boot-source problem.

Fix: hold **BOOT0 (`gpiochip1:37`) LOW** across the whole write+verify+reset.
libgpiod v3 `gpioset` holds the line until the process exits, so:

```sh
gpioset -c gpiochip1 37=0 &      # libgpiod v3 syntax (note -c <chip>)
BOOT0_PID=$!
sleep 0.3                        # let gpioset claim the line before reset latches it
openocd ... -c "reset run" -c "shutdown"
kill "$BOOT0_PID"                # release BOOT0
```

This is integrated into `flash.sh`. `recover.sh` does the same hold + a reset
*without* re-flashing — use it to boot an image that is already in flash (e.g.
after a cold boot where BOOT0 floated HIGH again).

> Note: a cold power-cycle has no BOOT0 hold either, so a freshly-flashed board
> may come up in the ROM bootloader after an unplug. Re-running `recover.sh`
> boots it into flash. A permanent fix would be a boot-time oneshot that runs
> `recover.sh` (mirrors what mcu-control's `recoverMCU()` does in production).

## Verifying what is actually running

```sh
cd /home/root/zephyr-flash/oo
LD_LIBRARY_PATH=./lib ./bin/openocd -s ./share/openocd/scripts -f ./unoq-swd.cfg \
    -c "init" -c "halt" -c "reg pc" -c "mdw 0x08000000 2" -c "shutdown"
```

- `reg pc` in `0x0800xxxx` => running the flashed image. In `0x0bf8xxxx` =>
  stuck in ROM bootloader (run `recover.sh`).
- `mdw 0x08000000 2` => first word is the initial SP (`0x200xxxxx`), second is
  the reset-handler address (`0x0800xxxx`); a sane pair means a valid image is
  present.
- NB: `halt` stops the CPU (LED freezes). Re-run `recover.sh` afterwards to
  resume free-running execution.

## adb quirks (see also adb-access.md)

Use `adb exec-out` (not `adb shell`, which injects CRs) and redirect output to a
file then Read it — inline adb stdout duplicates under WSL interop. openocd
prints a benign `Error: checksum mismatch - attempting binary compare` during
verify; that is the normal binary-end-padding CRC fallback, not a failure — the
following `verified NNNNN bytes` line is the real result.
