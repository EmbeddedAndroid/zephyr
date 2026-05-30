# Charlieplex LED matrix display driver (mainline Zephyr) — WORKING

Date 2026-05-28. An upstreamable Zephyr `display` driver for the Arduino UNO Q's
13x8 = 104-LED charlieplexed matrix (STM32 PF0..PF10, scanned by TIM17).
Verified on real hardware AND under twister/CI.

## Status
- HARDWARE: led_matrix demo flashed to the UNO Q, matrix lit and working
  (walking dot + full-panel flash, green-LED heartbeat). Confirmed by user.
- AUTONOMOUS BOOT: confirmed across a real cold reboot -- matrix comes up on its
  own, no manual step. See "Boot persistence" below.
- CI: ztest suite passes 7/7 under twister on native_sim/native/64 (emulated
  GPIO + software counter, no hardware) -- the harness Zephyr CI uses.
- Build: led_matrix app builds clean for arduino_uno_q.

## Boot persistence (how it survives a reboot)
The Zephyr app lives in STM32 internal flash (0x08000000), so it persists across
power loss. BUT on a cold boot nothing drives BOOT0 low, so the MCU can come up
in the ROM bootloader (matrix dark) -- same issue as the CAN work. Fixed with a
boot-time oneshot systemd unit `zephyr-mcu-boot.service` (source:
watcher/zephyr-mcu-boot.service, installed to /etc/systemd/system, enabled) that
runs oo/recover.sh (BOOT0-hold + SWD reset) after local-fs.target. This mirrors
mcu-control's recoverMCU() in the production unoq-mcu-app stack. The
zephyr-flash.path push-to-flash watcher is also enabled and survives reboot.
Verified end-to-end: power-cycle -> matrix lights with no intervention.

## Why a driver (not a port)
Goal is upstreaming into mainline Zephyr. Mainline already has a precedent:
display_nrf_led_matrix.c (a timer-multiplexed GPIO LED matrix exposed via the
display API, used by bbc_microbit). We built the charlieplex analog of it. The
Arduino loader's loader/matrix.inc (raw GPIOF->MODER/BSRR register banging,
hardcoded to STM32 port F) is NOT upstreamable; our driver uses only the generic
gpio + counter APIs, so it is portable to any Zephyr SoC.

## Layout (out-of-tree module, ready to lift into upstream)
modules/charlieplex-display/
  zephyr/module.yml                 # registers as a Zephyr module (cmake+kconfig+dts_root)
  CMakeLists.txt, Kconfig           # module glue (add_subdirectory / rsource)
  drivers/display/
    display_charlieplex_led_matrix.c  # the driver
    Kconfig                         # CONFIG_CHARLIEPLEX_LED_MATRIX (+INIT_PRIORITY)
    CMakeLists.txt
  dts/bindings/display/charlieplex-led-matrix.yaml   # the binding
  tests/drivers/display/charlieplex_led_matrix/      # ztest suite (7 tests)
    src/main.c, app.overlay, prj.conf, testcase.yaml, CMakeLists.txt

To upstream: move drivers/display/* and dts/bindings/* into Zephyr's tree, add
the Kconfig source line + CMakeLists line to drivers/display/, move the test to
tests/drivers/display/. The module structure intentionally mirrors the upstream
layout so this is mechanical.

## How the driver works
- Charlieplex: N gpio lines drive up to N*(N-1) LEDs. Each pixel = a {high,low}
  pin-pair; lit by driving high high, low low, ALL OTHERS high-impedance
  (gpio_pin_configure_dt GPIO_DISCONNECTED). Generic GPIO API => portable.
- A counter (timer) ISR scans one pixel per tick; persistence of vision. Only
  one LED physically on at a time.
- Grayscale: scan the full pixel list (levels-1) sub-frames per refresh; a pixel
  of brightness B is lit in the first B sub-frames (duty modulation).
- display API: write (MONO01 -> framebuffer), blanking on/off, get_framebuffer,
  set_brightness, get_capabilities, set_pixel_format. Mandatory write +
  get_capabilities implemented; instance-based DEVICE_DT_INST_DEFINE.

## The binding: charlieplex-led-matrix
include: display-controller.yaml (width/height). Properties:
- gpios: phandle-array, the charlieplex pin bank (indices referenced by pixels)
- counter: phandle, the scan timer
- refresh-frequency: int, whole-matrix Hz
- pixel-pairs: uint16 array, one per pixel (row-major), (high_idx<<8)|low_idx
  into the gpios list. length must == width*height (BUILD_ASSERT enforces).
- grayscale-bits: int 1..4 (default 1)
GOTCHA: a DT property named *-map is treated by edtlib as a nexus map (needs
#*-cells); that's why it's `pixel-pairs` not `pixel-map`.

## UNO Q board enablement (apps/led_matrix/boards/arduino_uno_q.overlay)
Enables &gpiof + &timers17/counter_matrix, instantiates the matrix node with the
104-entry pixel-pairs table lifted from matrix.inc (LED idx -> {pin0=high,
pin1=low}), 13x8, grayscale-bits=3, refresh 100Hz. chosen { zephyr,display }.

## Build / test / flash
- build demo: west-builder/build.sh -p /apps/led_matrix -- -DEXTRA_ZEPHYR_MODULES=/modules/charlieplex-display
- run tests:  west-builder/build.sh -- /work/zephyr/scripts/twister \
    -T /modules/charlieplex-display/tests/drivers/display/charlieplex_led_matrix \
    -p native_sim/native/64 -x=EXTRA_ZEPHYR_MODULES=/modules/charlieplex-display -O ...
- flash: adb push zephyr.bin -> watcher -> recover.sh (same loop as CAN work)
- west-builder image gained: native_sim host deps (pkg-config) + twister python
  deps (natsort pytest tabulate ply colorama junitparser). native_sim is 32-bit;
  on the aarch64 host use platform native_sim/native/64. Disable SDL in test
  prj.conf (CONFIG_SDL_DISPLAY=n) - it needs host libSDL2.

## Known / TODO before a real upstream PR
- Geometry: framebuffer order == LED index order == matrix.inc order. The 13x8
  logical layout + 180-deg panel rotation is left to the app (the Arduino sketch
  API note covers the put()-with-flip remap). Driver itself is geometry-agnostic.
- The grayscale duty is simple linear sub-frame counting; matrix.inc used a
  coarser non-linear gate. Could refine but linear is fine + clearer for upstream.
- Upstream process: rebase on zephyr main, signed-off commits, checkpatch clean,
  add driver to drivers/display/{Kconfig,CMakeLists.txt}, doc/index entry, maybe
  a sample. Consider a vendor-neutral compatible review with maintainers.
- Not yet wired: display_read, set_contrast (optional APIs, omitted).
