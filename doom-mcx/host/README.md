# DOOM on the Arduino UNO Q — controls & flashing

DOOM runs on the STM32U585 and displays on the 13x8 charlieplex matrix. You
drive it with the keyboard on your laptop. Input runs **on the device** (the
capture + SPI master are one process), reached over an `adb shell` PTY:

    laptop keyboard --(adb shell PTY)--> UNO Q SoC: doom_ctl.py
        --(SPI3 master /dev/spidev0.0 + RDY gpiochip1:70)-->
            STM32 DOOM firmware (SPI3 slave) --> DOOM input queue

> Why device-side: `adb exec-out` does **not** forward stdin, so an earlier
> split design (laptop capture piped to the device) only ever sent zeros and
> nothing moved. `adb shell` forwards the keyboard, so the controller lives on
> the device next to the SPI master.

## Play

From the laptop (WSL), board on USB, `adb devices` showing it:

    python3 wsad_doom.py

That pushes `doom_ctl.py`, sets the terminal raw with `stty`, and runs the
controller on the board. Keep that terminal focused. Pass-through args go to the
controller, e.g. `python3 wsad_doom.py --hold 300`.

Equivalent directly on the device:

    adb shell /home/root/secureboot/doom.sh input

## Controls

    W / S    move forward / back        (key_up   / key_down)
    A / D    turn left / right          (key_left / key_right)   <- look around
    Space    fire                       (key_fire)
    E        use / open doors           (key_use)
    Q / X    strafe left / right
    Esc      quit (releases all keys)

DOOM (this GBA-derived prBoom) has no vertical look or mouse; you look around by
turning with A/D.

## Hold behaviour

Terminals report key *repeats*, not *release*, so each key is held for `--hold`
ms (default 220) after its last keystroke; OS auto-repeat keeps it down while you
hold. Stuttery at the start of a hold? Lower your keyboard repeat delay or raise
`--hold`. If the controller dies, the firmware's 300 ms watchdog releases every
key so the player never sticks.

## Device orchestrator: doom.sh

Lives at `/home/root/secureboot/doom.sh` (next to the OTA `reflash.sh`):

    doom.sh flash      flash the DOOM firmware -> 0x08000000 (no-verify, ~2 min)
    doom.sh input      run the controller (WASD) over this terminal
    doom.sh play       flash DOOM, then tell you how to connect input
    doom.sh demo       hand off to reflash.sh demo (restore A/B OTA mcuboot mode)
    doom.sh status     show staged images + input readiness

DOOM is a single 2 MB image at 0x08000000 with **no mcuboot** (engine + WAD fill
flash). The OTA demo is the other mode (mcuboot + signed bridge in slot0). Only
one runs at a time; `doom.sh` flashes DOOM-mode and `doom.sh demo` switches back
to OTA-mode, so the two co-exist as staged images you toggle between. `doom.sh`
never touches mcuboot's images.

## Files

- `wsad_doom.py`  — laptop launcher (push + `adb shell` + `stty`)
- `doom_ctl.py`   — device controller: keyboard capture + SPI master (one proc)
- `wsad_send.py`  — device SPI sender reading decimal masks on stdin (scripting/tests)
- `doom.sh`       — device orchestrator (flash / input / demo / status)
- `flash-fast.sh` — no-verify SWD flasher (in `/home/root/zephyr-flash/oo/`)
