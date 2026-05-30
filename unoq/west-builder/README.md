# west-builder: docker-wrapped Zephyr build for the UNO Q

Host-side (aarch64) container that runs `west build` against the mounted Zephyr
workspace + SDK, so you can compile mainline-Zephyr apps for `arduino_uno_q`
without polluting the host. Output drops to `<workspace>/build/zephyr/zephyr.bin`
owned by the host user, ready to `adb push` to the device watcher.

## Image

`unoq-west-builder:v1` (~981 MB) — debian-trixie-slim + Zephyr build deps + a
python venv with west and the deps `west build` needs (incl. jsonschema). The
**workspace and SDK are bind-mounted at runtime, NOT baked in** (they total ~16 GB).

Build the image:
```sh
cd west-builder && docker build --provenance=false -t unoq-west-builder:v1 .
```

## Workspace + SDK (host paths)

- Workspace: `~/Dev/claude/zephyr-mainline/zephyrproject` (Zephyr 4.4.0, west-managed)
- SDK: `~/zephyr-sdk-aarch64` = **Zephyr SDK 1.0.1** (the real Zephyr SDK).
  NOTE: the dir literally named `~/zephyr-sdk-1.0.1` is a **Yocto eSDK, NOT the
  Zephyr SDK** — confusingly named; do not use it. Use `~/zephyr-sdk-aarch64`.
  Override either path with env: `ZEPHYR_WORKSPACE=... ZEPHYR_SDK_DIR=... ./build.sh`

## Usage

```sh
# build a sample (path relative to workspace root)
./build.sh zephyr/samples/basic/blinky

# pristine rebuild + different board
./build.sh -p -b arduino_uno_q path/to/app

# interactive shell in the build env
./build.sh shell

# run an arbitrary west/any command in the env
./build.sh -- west boards
./build.sh -- west build -t menuconfig ...
```

Result: `~/Dev/claude/zephyr-mainline/zephyrproject/build/zephyr/zephyr.bin`.
Then flash it:
```sh
ADB="/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe"
"$ADB" push .../build/zephyr/zephyr.bin /home/root/zephyr-flash/incoming.bin
# systemd watcher auto-flashes; check status.json (see ../watcher/README.md)
```

Verified 2026-05-28: a fresh `./build.sh zephyr/samples/basic/blinky` produces a
zephyr.bin byte-identical to the original PoC artifact (sha256 d141ac10…34cdf).

## Gotchas

- The Zephyr SDK contains ABSOLUTE symlinks (e.g.
  `gnu/arm-zephyr-eabi -> $SDK/arm-zephyr-eabi`), so it MUST be bind-mounted at
  the SAME path inside the container as on the host (`-v $SDK:$SDK`), not at a
  fixed `/sdk`. Otherwise the toolchain symlinks dangle and cmake fails with a
  bogus `arm-zephyr-eabi-gcc: Syntax error "(" unexpected` (the kernel can't
  load the missing ELF). build.sh handles this.
- A `build/` dir copied/moved from another machine has the OLD absolute
  ZEPHYR_BASE baked into its CMakeCache, so `west build -p` runs the wrong
  `pristine.cmake` and fails with a stale `/home/.../zephyrproject` path. Fix:
  `rm -rf zephyrproject/build` and build fresh. (This bit us once after moving
  the workspace into the project dir.)
- The container drops to your uid/gid (via LOCAL_UID/LOCAL_GID + setpriv) so
  build artifacts aren't root-owned. `util-linux` (setpriv) + `adduser` are in
  the image for this.
- west config/HOME is set to /tmp inside the container (the workspace `.west/`
  already pins the manifest, so no re-init needed).
