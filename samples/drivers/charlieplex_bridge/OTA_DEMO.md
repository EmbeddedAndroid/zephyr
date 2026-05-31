# UNO Q MCUboot A/B OTA demo (charlieplex bridge + blink)

How to drive the LED matrix from Linux and run a full A/B firmware update
over the in-band SPI bridge on the Arduino UNO Q (STM32U585), with a
self-recovering "blink" image as the OTA target.

This is a development/demo flow on the `bridge` branch. It uses the MCUboot
**sample** signing key (`bootloader/mcuboot/root-ec-p256.pem`) — replace with a
private key for anything real.

---

## The two firmwares

| App | Path | What it does | Talks to Linux? |
|-----|------|--------------|-----------------|
| **bridge** | `samples/drivers/charlieplex_bridge` | SPI slave: Linux drives the matrix; serves status/version; receives OTA | yes (SPI) |
| **blink**  | `samples/drivers/charlieplex_blink`  | Standalone: flashes the whole matrix on/off every 400 ms | no |

The board boots `bridge`. You OTA `blink` **as a test swap** — it does *not*
confirm itself, so a single reset reverts to `bridge`. No SWD needed to recover.

Flash partition map (already on the mainline board):

```
mcuboot   64K  @ 0x08000000
image-0  416K  @ 0x08010000   (slot0 = running)
image-1  416K  @ 0x08078000   (slot1 = OTA staging)
storage  128K  @ 0x080E0000
```

MCUboot runs in **swap-using-offset** mode (board default): the uploaded image
lands one sector (0x2000) above slot1 base; `flash_img` handles that offset.

---

## Prerequisites

- Board reachable over adb (here: Windows `adb.exe` via WSL).
- `bridge_send.py` and the signed images already staged on the device:
  - `/home/root/bridge_send.py`
  - `/home/root/secureboot/{b1,b2,b3,blink}.signed.bin`
- The native SWD flasher bundle at `/home/root/zephyr-flash/oo/`
  (`flash.sh`, `recover.sh`, `openocd`, `gpioset`).

Set the adb path once per shell:

```sh
ADB=/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe
```

---

## 1. Drive the panel from Linux

```sh
$ADB shell 'cd /home/root && python3 bridge_send.py <cmd>'
```

| `<cmd>` | Effect |
|---------|--------|
| `all-on` | light every LED |
| `clear` | blank the matrix |
| `hat` | cowboy-hat bitmap |
| `walk` | single pixel walks the panel (loops, Ctrl-C) |
| `checker` | checkerboard (loops) |
| `kver` | scroll the live Linux kernel version (loops) |
| `versions` | alternate Linux ver / MCU Zephyr ver (loops) |
| `cmd brightness 64` | set brightness 0–255 |
| `cmd query` | read the MCU status block |

Flags: `--delay 0.03` (animation speed), `--text "HELLO"` (for `kver`).

```sh
$ADB shell 'cd /home/root && python3 bridge_send.py all-on'
$ADB shell 'cd /home/root && python3 bridge_send.py --delay 0.03 walk'
```

---

## 2. Check the bridge is alive (and unwedge it)

```sh
$ADB shell 'cd /home/root && python3 bridge_send.py cmd query'
```

The SPI slave can **wedge after sitting idle** — a query then returns `None` or
hangs. This is a known quirk, not a failure. A reset clears it:

```sh
$ADB shell 'cd /home/root/zephyr-flash/oo && ./recover.sh'
```

Re-query; you should get a status dict with `crc_err: 0, state: 1`.

---

## 3. Run the OTA (blink, test swap — reversible)

**a. Erase the staging slot (slot1):**

```sh
$ADB shell 'cd /home/root/zephyr-flash/oo && export LD_LIBRARY_PATH=$PWD/lib && \
  ./bin/gpioset -c gpiochip1 37=0 & sleep 0.3; \
  ./bin/openocd -s share/openocd/scripts -f unoq-swd.cfg \
    -c init -c halt -c "flash erase_address 0x08078000 0x68000" \
    -c "reset run" -c shutdown; kill %1'
```

**b. Stream the signed blink image and reboot into MCUboot:**

```sh
$ADB shell 'cd /home/root && python3 bridge_send.py --delay 0 \
  ota /home/root/secureboot/blink.signed.bin --chunk 48'
```

Watch the live counter climb to the full image size — this is the success tell:

```
sent=37918 / 37918 (streaming done)
pre-end status: {... 'ota_err': 0}
OTA_END sent (permanent=0); MCU will reboot into MCUboot.
```

After the reboot **the panel blinks on its own** — that is the new firmware
running. (`permanent=0` = test swap.)

---

## 4. Recover (revert to the bridge)

Because the blink image never confirmed itself, a single reset makes MCUboot
revert to the confirmed bridge image:

```sh
$ADB shell 'cd /home/root/zephyr-flash/oo && ./recover.sh'
```

The panel returns to bridge behaviour and `bridge_send.py cmd query` answers
again.

---

## 5. Prove the swap actually happened (channel-proof)

The bridge version query can be flaky after an idle wedge, so the unambiguous
check is to SWD-dump slot0 and byte-count the version string — a number, immune
to any serial/adb mangling:

```sh
$ADB shell 'cd /home/root/zephyr-flash/oo && export LD_LIBRARY_PATH=$PWD/lib && \
  ./bin/gpioset -c gpiochip1 37=0 & sleep 0.3; \
  ./bin/openocd -s share/openocd/scripts -f unoq-swd.cfg \
    -c init -c halt -c "dump_image /home/root/slot0.bin 0x08010000 0xc000" \
    -c "reset run" -c shutdown; kill %1'
$ADB pull /home/root/slot0.bin /tmp/slot0.bin
python3 -c 'd=open("/tmp/slot0.bin","rb").read(); \
  print("bridge:", d.count(b"ZEPHYR 4.4.99"), "blink:", d.count(b"charlieplex_blink"))'
```

Verified round-trip on hardware:

| Step | slot0 contents |
|------|----------------|
| before OTA | bridge (`B3`) |
| after blink test-swap | `blink` |
| after reset (no confirm) | bridge (`B3`) — reverted |

---

## Optional: make an OTA permanent

Add `--permanent` to the `ota` command and the swap sticks across resets.

> ⚠️ The blink app has **no SPI bridge**, so once it is running permanently the
> only way back is an SWD reflash of the bridge into slot0:
>
> ```sh
> $ADB shell 'cd /home/root/zephyr-flash/oo && \
>   ./flash.sh /home/root/secureboot/b3.signed.bin 0x08010000'
> ```
>
> For the demo, prefer the test swap in step 3.

To make a *bridge* test image (e.g. `b3`) permanent instead, send `confirm`:

```sh
$ADB shell 'cd /home/root && python3 bridge_send.py confirm'
```

---

## How it works / why it was hard

The MCU is a blocking single-block SPI slave: it can only receive a block while
parked in `spi_transceive()`, then runs a slow flash erase/program for that
block in `process_block()`. The **RDY** line (STM32 PG13 → Linux `gpiochip1`
line 70) is the handshake — the host must wait for RDY high before clocking each
block, or the slave drops it and the staged image is corrupt (MCUboot then
rejects it: no swap).

The fix is on both sides of the link:

- **MCU** (`bridge/src/main.c`): drop RDY at the top of the main loop, before
  `process_block()`, and raise it again once the next reply is staged. (The bug
  was that RDY was only ever lowered once, at boot, so it was stuck high and the
  host's gate did nothing.)
- **host** (`bridge/host/bridge_send.py`): read RDY via the GPIO cdev v2 uAPI
  (`GpioLine`, hand-rolled ioctls — the stock image has no python-spidev or
  libgpiod) and wait for high before every OTA transfer (`--rdy-chip` /
  `--rdy-line`, default `gpiochip1` / 70).

Success tell: the `acked` counter tracks `sent` all the way to the image size.
While the bug was live it plateaued partway and the swap never happened.

---

## Build (for reference)

From the docker wrapper (`zephyr-mainline/west-builder`), pass args separately
(not as one quoted string):

```sh
ZEPHYR_WORKSPACE=.../zephyr-upstream ./build.sh -- \
  west build -p always --sysbuild -b arduino_uno_q \
  -d build_bridge /work/zephyr/samples/drivers/charlieplex_bridge

ZEPHYR_WORKSPACE=.../zephyr-upstream ./build.sh -- \
  west build -p always --sysbuild -b arduino_uno_q \
  -d build_blink /work/zephyr/samples/drivers/charlieplex_blink
```

Outputs: `<build>/mcuboot/zephyr/zephyr.bin` (→ 0x08000000) and
`<build>/<app>/zephyr/zephyr.signed.bin` (→ 0x08010000 / OTA payload).

To make version tags distinguishable for testing, append e.g. `" B2"` to the
`ver[]` string in `bridge/src/main.c` before building.
