# DOOM on UNO Q — Pickup / Resume Doc (2026-06-01)

Goal: run DOOM on the Arduino UNO Q **STM32U585** (Zephyr) with the **13x8 charlieplex
LED matrix** as the game display, and the existing **SPI bridge** carrying keyboard
input from a Linux-side **WASD+space** script. Branch `doom` off
`display-charlieplex-led-matrix` in `/home/tyler/Dev/claude/zephyr-upstream`.

Base engine: **NXP Doom-MCX** (prBoom, GPLv2), vendored at
`/home/tyler/Dev/claude/zephyr-upstream/doom-mcx/` (sibling of `zephyr/`, OUTSIDE the
zephyr git tree). Full background + earlier steps in memory: `project_unoq_doom.md`.

## CURRENT STATUS (the important part)

DOOM **builds, fits, boots, and renders on the charlieplex** (user confirmed lit pixels
earlier). The remaining work is a chain of crashes during/after level load that I've been
fixing one by one. **Big realization:** the NXP port's map loaders DIRECT-CAST raw WAD
lumps to the engine's in-memory structs (they ship a build-time-PREPROCESSED WAD). A
**stock WAD (our Squashware) crashes** unless the loaders convert on-load. I'm rewriting
the broken loaders to convert (stock-prBoom behaviour).

### FIXES ALREADY APPLIED (all in doom-mcx/, uncommitted)
1. `prboom2/src/m_menu.c`: enum `gamma` -> `gamma_item` (picolibc `gamma()` collision). 2 spots.
2. `boards/arduino_uno_q_stm32u585xx.conf` (NEW board conf): picolibc, NO full POSIX AEP
   (only POSIX_C_LIB_EXT + DEVICE_IO + FILE_SYSTEM; timers/signals OFF — timer.c sigevent
   break), FPU, DISPLAY, FLASH, X_RES=400 Y_RES=320, ZONE_HEAP=194, RGB565,
   CHARLIEPLEX=y, MAIN_STACK=32768, ARM_MPU=y, HW_STACK_PROTECTION=y. NXP joystick/touch/logo OFF.
3. `src/i_system_zephyr.c`: added `gettimeofday()` backed by `k_uptime_get()` (I_GetTime needs it).
4. `CMakeLists.txt`: `-mno-unaligned-access` (guarded `if(CONFIG_CPU_CORTEX_M)` so native_sim/x86 still builds).
5. NEW Kconfig `CONFIG_DOOM_CHARLIEPLEX` (Kconfig) + charlieplex render path in
   `src/i_system_zephyr.c`: `I_SetPallete_zephyr` builds `cplx_lum[256]` (Rec.601 luma 0..7);
   `Z_DisplayThreadEntry` (#if CHARLIEPLEX) box-downscales the indexed frame ->
   `cplx_w x cplx_h` straight into the matrix driver framebuffer (cplx_fb via
   display_get_framebuffer); `I_InitScreen_zephyr` caches dims+fb. **Downscale stride fix:**
   use `SCREENWIDTH/SCREENHEIGHT` (NOT CONFIG_DOOM_X_RES — engine packs 2 cols/pixel, SCREENWIDTH=X_RES/2).
6. `prboom2/src/d_main.c`: `D_StartTitle()` -> `G_DeferedInitNew(sk_medium,1,1)` (#if CHARLIEPLEX)
   so it boots straight into live E1M1 (title/demo are static/unreadable on 13x8 + we drive input).
7. **THE MAP-LOADER CONVERSIONS** in `prboom2/src/p_setup.c` (the core fix):
   - `P_LoadVertexes`: convert mapvertex_t(short x,y) -> vertex_t(fixed_t), `<<FRACBITS`.
   - `P_LoadLineDefs`: convert maplinedef_t(14B) -> line_t (resolve v1/v2 coords, dx/dy,
     bbox via BOXLEFT/RIGHT/TOP/BOTTOM from m_bbox.h, slopetype, flags/special/tag/sidenum, lineno).
   - `P_LoadSegs`: convert mapseg_t(12B) -> seg_t (resolve v1/v2 coords, angle<<16,
     offset<<FRACBITS, linenum, sidenum=ldef->sidenum[side], frontsectornum/backsectornum
     via `_g->sides[..].sector - _g->sectors` pointer->index).
   - (P_LoadSectors/P_LoadSideDefs/P_LoadSubsectors/P_LoadNodes were ALREADY correct on-disk.)
8. `prboom2/src/d_main.c`: **gate out ST_Drawer + HU_Drawer** `#if !defined(CONFIG_DOOM_CHARLIEPLEX)`
   — THE CRASH I JUST FOUND: status bar draws STGANUM* number graphics that Squashware
   STRIPPED ("STGANUM0-9 not found" at boot) -> NULL patch deref -> segfault. Status bar is
   useless on 13x8 anyway.

### DEBUG BREADCRUMBS STILL IN THE CODE (REMOVE before committing)
- `prboom2/src/p_tick.c`: `#include <stdio.h>` + 4 `printf("DBG P_Ticker: ...")` lines in P_Ticker.
- `prboom2/src/p_setup.c`: `lprintf(LO_INFO,"DBG counts: ...")` after P_GroupLines.
- (d_main.c R_Render + ST breadcrumbs already removed; the ST_Drawer #if gate is a real fix, keep it.)

### CRASH-CHAIN PROGRESS (debugged on native_sim — FAST loop, no flash needed)
native_sim with `-DCONFIG_DOOM_CHARLIEPLEX=y` reproduces the SAME crashes as hardware in
seconds (build_doom_ns). Sequence of crashes fixed:
- "P_GroupLines: Subsector a part of no sector" (every subsector) -> FIXED by seg/vertex/linedef conversion.
- Level now loads clean: DBG counts verts=402 sectors=82 sides=682 lines=431 subs=197 (sane).
- First tic (P_Ticker) runs full; first frame R_RenderPlayerView runs full.
- **CURRENT crash: in ST_Drawer (first frame, after R_RenderPlayerView).** Just gated it out (#8).
  NEXT: rebuild native_sim + run; expect it to survive multiple ticks/frames (exit 124 = ran full 15s = success).

## HOW TO BUILD/RUN (host, NOT the docker builder)
venv: `/tmp/zvenv` (has zephyr reqs). If gone: `python3 -m venv /tmp/zvenv && /tmp/zvenv/bin/pip install -r zephyr/scripts/requirements-base.txt`

NATIVE_SIM (fast debug loop, x86, with SDL — the dev workhorse):
```
cd /home/tyler/Dev/claude/zephyr-upstream
export ZEPHYR_BASE=$PWD/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host
. /tmp/zvenv/bin/activate
cmake -GNinja -B build_doom_ns -S doom-mcx -DBOARD=native_sim/native/64 -DZEPHYR_BASE=$ZEPHYR_BASE -DCONFIG_DOOM_CHARLIEPLEX=y   # CHARLIEPLEX=y forces E1M1 + the ST gate
ninja -C build_doom_ns
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 15 build_doom_ns/zephyr/zephyr.exe   # exit 124 = ran OK; 139 = segfault
```
NOTE: no system gdb and SDK gdb is ARM-only -> debug native_sim via printf/lprintf breadcrumbs.

HARDWARE (STM32U585):
```
export ZEPHYR_BASE=$PWD/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/home/tyler/zephyr-sdk-0.16-aarch64
. /tmp/zvenv/bin/activate
cmake -GNinja -B build_doom_uq -S doom-mcx -DBOARD=arduino_uno_q -DZEPHYR_BASE=$ZEPHYR_BASE -DEXTRA_CONF_FILE=boards/arduino_uno_q_stm32u585xx.conf
ninja -C build_doom_uq
# no .bin emitted (BUILD_OUTPUT_BIN=n): objcopy it:
~/zephyr-sdk-0.16-aarch64/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy -O binary build_doom_uq/zephyr/zephyr.elf /tmp/doom.bin
```
FLASH (no MCUboot for doom; image at 0x08000000):
```
ADB=/mnt/c/Users/force/Downloads/platform-tools-latest-windows/platform-tools/adb.exe
$ADB push /tmp/doom.bin /home/root/secureboot/doom.bin
$ADB exec-out 'cd /home/root/zephyr-flash/oo && ./flash.sh /home/root/secureboot/doom.bin 0x08000000'   # ~2min, 1.94MB
```
**FLASH WEDGE LESSON: do NOT run any other adb command while flash.sh is running — concurrent
SWD+adb wedges the link (board goes offline, needs physical reset). Run flash in background
(run_in_background) and DO NOT poll adb until it completes.**
Recover demo afterwards: `reflash.sh demo` (mcuboot+bridge) — but that's the OTA-demo branch stuff.

## WAD
- WAD baked as C array `prboom2/src/iwad/squashware.c` (1,703,241 B Squashware v1.3 full =
  `newdoom1.wad`), selected via `#include "iwad/squashware.c"` in `prboom2/src/doom_iwad.c`
  (doom1.c commented out). It's 8.5MB of source; gitignore-worthy. Original at /tmp/sqw/newdoom1.wad.
- Squashware STRIPS lumps (STGANUM* etc.) -> any code drawing them must be guarded.

## REMAINING AFTER THE CRASH IS FIXED
- Verify native_sim runs many frames clean, THEN flash + verify on HW (gametic advancing,
  PC never arch_system_halt, framebuffer ANIMATING — read cplx_framebuf_0 twice, must differ).
- Remove all DBG breadcrumbs (p_tick.c, p_setup.c).
- STEP 4: SPI-bridge input driver — engine reads DT aliases right/left/up/down/fire/enter/
  strafe/run/menu via INPUT_KEY_* (gpio-keys). Write a small Zephyr input device that injects
  INPUT_KEY_* from SPI-bridge key blocks (reuse the bridge 64B/CRC/RDY protocol from the
  mcuboot/bridge work). Board overlay must define the gpio-keys + aliases (currently the
  uno_q board has no doom input overlay — needs boards/arduino_uno_q.overlay in doom-mcx
  wiring those aliases to a gpio-emul or the new input device).
- STEP 5: `wsad_doom.py` on Linux — raw-terminal W/A/S/D/space -> SPI key blocks
  (reuse bridge_send.py framing from charlieplex_bridge/host).
- Commit on `doom` branch (GPLv2 ok on fork; keep OFF the upstream PR branch).

## KEY FILES
- doom-mcx/prboom2/src/p_setup.c — the loader conversions (core fix)
- doom-mcx/src/i_system_zephyr.c — charlieplex render + gettimeofday + input hooks
- doom-mcx/prboom2/src/d_main.c — E1M1 autostart + ST_Drawer gate
- doom-mcx/boards/arduino_uno_q_stm32u585xx.conf — board config
- doom-mcx/Kconfig — CONFIG_DOOM_CHARLIEPLEX
- build_doom_ns (native_sim) / build_doom_uq (hardware)
