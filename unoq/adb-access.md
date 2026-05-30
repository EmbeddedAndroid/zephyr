# Accessing the UNO Q over ADB (from WSL2)

The board is **not** on wifi/LAN. The only way in is the Windows host's
`adb`, reached from WSL2 through Windows interop. You get a **root** shell on
the QCM2290 (Cortex-A / Linux side).

## The adb binary

```
/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe
```

It runs directly from WSL via Windows interop (`adb.exe`, not a Linux adb). A
Linux `/usr/bin/adb` also exists on this machine but it does **not** see the
board — the USB device is bound to Windows. Always use the `.exe`.

Handy to alias in a shell:

```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"
```

## Device facts

- Serial: `3a49b637` (the `transport_id` changes between sessions; the serial
  is stable)
- `adb devices` shows: `3a49b637  device`
- Linux side: **Qualcomm Linux Reference Distro (OTA-enabled) 2.0**
  (`ID=qcom-distro-sota`), kernel `7.1.0-rc4` aarch64, hostname
  `uno-q-3101861900`
- Shell user: `uid=0(root)` — full root, no `su` needed
- This is a **plain qcom-distro image**, NOT the `unoq-mcu-app` compose stack:
  - `/usr/bin/docker` is present
  - `/dev/ttyHS1` is present (LPUART1 console path to the MCU)
  - `/dev/gpiochip0` and `/dev/gpiochip1` exist (gpiochip1 carries the SWD
    lines to the STM32U585 — see the pin map in `zephyr-mainline.md`)
  - **No `openocd`** on the device
  - **No `gpioset`/`gpioget`/`gpioinfo`/`gpiodetect`** (libgpiod CLI tools not
    installed)
  - No mcu-control container running
- Storage: rootfs (`/dev/disk/by-label/otaroot`) 13G, ~11G free; `/tmp` is
  tmpfs, 867M

Implication for flashing the STM32 over SWD: you must bring your own openocd
(build/ship the aarch64 openocd image from
`~/Dev/claude/unoq-mcu-poc/openocd-build/`, which bundles libgpiod v2), since
neither openocd nor the gpiod CLIs are on the stock image.

## Two interop quirks (and the fix for both)

### 1. `adb shell` injects carriage returns

`adb shell <cmd>` allocates a PTY, so output comes back with `\r\n` line
endings and TTY artifacts. Use **`adb exec-out <cmd>`** instead — it runs the
command without a PTY and gives clean output.

### 2. Inline stdout gets duplicated in the agent terminal

When `adb.exe` output is rendered inline in the Bash tool, it can appear
**duplicated many times** — a WSL-interop terminal artifact. The data is fine;
only the inline rendering is wrong. Verified: `exec-out id` writes exactly
the right bytes to a file even when the inline echo looks tripled.

**Rule: always redirect adb output to a file, then read the file.**

```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"
"$ADB" exec-out sh -c '
  uname -a
  head -4 /etc/os-release
  ls /dev/gpiochip*
' > /tmp/probe.txt 2>&1
sed -i 's/\r$//' /tmp/probe.txt   # strip any stray CRs
# then open /tmp/probe.txt with the Read tool
```

For multi-command probes, wrap them in a single `sh -c '...'` so you pay the
adb round-trip once.

## Recovering a wedged adb

If output starts looking garbled/duplicated or commands hang:

```sh
"$ADB" kill-server   # clears stale daemons on the Windows side
"$ADB" start-server
"$ADB" devices       # confirm 3a49b637 is back as "device"
```

If an `adb.exe` invocation hangs the Bash tool, kill the stragglers:
`pkill -9 -f adb.exe` (Linux side) — but note this only reaps the WSL-side
launcher; a `kill-server` is what actually resets the Windows daemon.

## Common operations

```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"

# push a file to the device
"$ADB" push ./zephyr.bin /tmp/zephyr.bin

# pull a file back
"$ADB" pull /tmp/foo.log ./foo.log

# run a command, capture clean output
"$ADB" exec-out 'cat /etc/os-release' > /tmp/out.txt 2>&1

# list docker on the device
"$ADB" exec-out 'docker ps' > /tmp/ps.txt 2>&1
```
