# zephyr-flash watcher (device-side auto-flash)

A systemd `.path` unit watches a file; when an `adb push` replaces it, a oneshot
service flashes it to the STM32U585. This is the device half of the
"push a bin, it flashes itself" loop.

## Flow

```
adb push zephyr.bin  ->  /home/root/zephyr-flash/incoming.bin
        | (zephyr-flash.path, PathChanged)
        v
zephyr-flash.service (oneshot)  ->  flash-incoming.sh
        |  -> oo/flash.sh  (BOOT0 hold + write + verify + reset @0x08000000)
        v
status.json  +  last-flash.log   (adb pull to inspect)
```

## Files

On device, all under `/home/root/zephyr-flash/`:

| File | Role |
| --- | --- |
| `incoming.bin` | the watched file; `adb push` your firmware here |
| `oo/` | the native openocd flasher bundle (see ../flash.md) |
| `flash-incoming.sh` | service body: runs `oo/flash.sh`, writes status.json |
| `status.json` | result of last flash: `{"ok":true,"sha256":...}` |
| `last-flash.log` | full openocd output of last flash |
| `zephyr-flash.path` / `.service` | systemd units (installed to /etc/systemd/system) |
| `install.sh` | installs+enables the units (run once, on device) |

Source for all of these: `~/Dev/claude/zephyr-mainline/watcher/` (units +
scripts) and `~/Dev/claude/zephyr-mainline/openocd-native/bundle/` (the `oo/`
bundle).

## Install (once)

```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"
for f in zephyr-flash.path zephyr-flash.service flash-incoming.sh install.sh; do
    "$ADB" push watcher/$f /home/root/zephyr-flash/$f
done
"$ADB" exec-out 'sh /home/root/zephyr-flash/install.sh'
```

`/etc` is writable on the ostree qcom-distro image, so units live in
`/etc/systemd/system`. The unit is enabled, so it survives reboot — though note
the flasher itself needs the `oo/` bundle present.

## Use

```sh
ADB="..."
"$ADB" push build/zephyr/zephyr.bin /home/root/zephyr-flash/incoming.bin
# wait ~4s, then:
"$ADB" exec-out 'cat /home/root/zephyr-flash/status.json'
# {"ok":true,"sha256":"...","addr":"0x08000000","uptime":...}
```

On failure, `status.json` has `"ok":false` + an `rc`, and `last-flash.log` has
the openocd output.

## Notes / gotchas

- `PathChanged` triggers on close-after-write, so a partial `adb push` won't
  fire it mid-copy.
- A cold power-cycle drops the BOOT0 hold, so the MCU may come up in the ROM
  bootloader after an unplug (see ../flash.md). To boot an
  already-flashed image without re-flashing:
  `adb exec-out /home/root/zephyr-flash/oo/recover.sh`. A future improvement is
  a boot-time oneshot that runs recover.sh (mirrors mcu-control's recoverMCU).
- No wall-clock on the device is guaranteed; status.json stamps `/proc/uptime`
  seconds for ordering, not a real timestamp.
- Verified end-to-end 2026-05-28: push -> auto-flash -> PC=0x08002f7a (running
  flash) -> green LED.
