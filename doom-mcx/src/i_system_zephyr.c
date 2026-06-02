/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright 2024 NXP
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 *-----------------------------------------------------------------------------
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "d_event.h"
#include "d_main.h"
#include "g_game.h"
#include "i_system_zephyr.h"

#include "doomdef.h"
#include "lprintf.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sample, LOG_LEVEL_INF);
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <sys/time.h>

#ifdef CONFIG_DOOM_SHOW_NXP_LOGO
#include "nxp_80x32.h"
#endif

/*
 * I_GetTime() (in r_hotpath.iwram.c) calls gettimeofday(). With the full POSIX
 * AEP this comes from the C library, but we build with only the POSIX libc
 * extension (no timers/signals AEP, see the board conf) so picolibc does not
 * provide it. Back it with the Zephyr uptime clock -- only tv_sec/tv_usec are
 * used, and only as a monotonic tick source, so an uptime epoch is fine.
 */
int gettimeofday(struct timeval *tv, void *tz)
{
	ARG_UNUSED(tz);
	if (tv == NULL) {
		return -1;
	}
	int64_t ms = k_uptime_get();

	tv->tv_sec = (time_t)(ms / 1000);
	tv->tv_usec = (suseconds_t)((ms % 1000) * 1000);
	return 0;
}

#define RIGHT_NODE DT_ALIAS(right)
#define LEFT_NODE DT_ALIAS(left)
#define DOWN_NODE DT_ALIAS(down)
#define UP_NODE DT_ALIAS(up)
#define FIRE_NODE DT_ALIAS(fire)
#define ENTER_NODE DT_ALIAS(enter)
#define MENU_NODE DT_ALIAS(menu)
#define WEAPONTOGGLE_NODE DT_ALIAS(weapontoggle)
#define STRAFE_NODE DT_ALIAS(strafe)
#define RUN_NODE DT_ALIAS(run)
#define MAP_NODE DT_ALIAS(map)
#define TOGGLE_NODE DT_ALIAS(toggle)

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

#define STACKSIZE 8192
struct k_thread display_thread;
K_THREAD_STACK_DEFINE(display_stack, STACKSIZE);
struct display_capabilities d_capabilities;
struct k_sem sem_display;
struct k_sem sem_renderer;

//**************************************************************************************

struct key_mapping {
  struct gpio_dt_spec node;
  const int *key;
  evtype_t eventstate;
};

struct key_mapping keymap[] = {
#ifndef CONFIG_DOOM_ZEPHYR_ADC_JOYSTICK
    {GPIO_DT_SPEC_GET_OR(UP_NODE, gpios, {0}), &key_up, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(DOWN_NODE, gpios, {0}), &key_down, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(LEFT_NODE, gpios, {0}), &key_left, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(RIGHT_NODE, gpios, {0}), &key_right, ev_keyup},
#endif
    {GPIO_DT_SPEC_GET_OR(FIRE_NODE, gpios, {0}), &key_fire, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(ENTER_NODE, gpios, {0}), &key_enter, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(ENTER_NODE, gpios, {0}), &key_use, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(MENU_NODE, gpios, {0}), &key_escape, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(RUN_NODE, gpios, {0}), &key_speed, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(MAP_NODE, gpios, {0}), &key_map, ev_keyup},
    {GPIO_DT_SPEC_GET_OR(TOGGLE_NODE, gpios, {0}), &key_toggle, ev_keyup},
};

#define KEYMAP_SIZE (sizeof(keymap) / sizeof(keymap[0]))

#ifdef CONFIG_DOOM_ZEPHYR_ADC_JOYSTICK
static const struct device *const adc_joy_dev =
    DEVICE_DT_GET(DT_NODELABEL(adc_joystick));
#endif
#ifdef CONFIG_DOOM_ZEPHYR_TOUCH_SCREEN
static const struct device *const touch_screen_dev =
    DEVICE_DT_GET(DT_ALIAS(touch_screen));
#endif

bool x_l_pressed = false;
bool x_r_pressed = false;
bool x_center = false;

bool y_u_pressed = false;
bool y_d_pressed = false;
bool y_center = false;
bool action_pressed = false;
bool action_fire = false;

// TOD IFDEF TOUCH
uint16_t touch_initial_x = 0;
uint16_t touch_initial_y = 0;

uint16_t touch_x = 0;
uint16_t touch_y = 0;
bool touch_btn = false;
bool touch_released = true;

unsigned int vid_width = 0;
unsigned int vid_height = 0;

FAST_DATA_RAM unsigned short
    backbuffer[(SCREENWIDTH * SCREENHEIGHT) + CONFIG_DOOM_X_RES];

#ifndef CONFIG_DOOM_NO_WIPE
FAST_DATA_RAM unsigned short frontbuffer[SCREENWIDTH * SCREENHEIGHT];
#endif

#if defined(CONFIG_DOOM_RGB565) || defined(CONFIG_DOOM_BGR565)
static uint16_t pl_565[256];
const byte *pl_game = (byte *)pl_565;
#else
const byte *pl_game;
#endif

const struct device *display_dev;

#if defined(CONFIG_DOOM_CHARLIEPLEX)
/* Charlieplex output: downscale the indexed DOOM frame to the matrix size and
 * write per-pixel brightness directly into the driver framebuffer (grayscale),
 * bypassing the 1bpp display_write() path which would lose shading.
 */
#define CPLX_LEVELS 8 /* matches the matrix grayscale-bits=3 -> 0..7 */
#define CPLX_MAXPX 128 /* >= cplx_w*cplx_h (13*8 = 104) */
static uint8_t cplx_lum[256]; /* palette index -> full luma 0..255 */
static uint16_t cplx_w, cplx_h;
static uint8_t *cplx_fb; /* driver brightness buffer, 1 byte/pixel (0..max) */
static uint8_t cplx_avg[CPLX_MAXPX]; /* per-cell averaged luma 0..255 (pre-stretch) */
#endif

//**************************************************************************************

void I_uSleep(unsigned long usecs) { k_sleep(K_USEC(usecs)); }

//**************************************************************************************

#if defined(CONFIG_DOOM_CHARLIEPLEX)
void Z_DisplayThreadEntry(void) {
  /* Box-downscale the indexed DOOM frame -> cplx_w x cplx_h grayscale, writing
   * brightness straight into the matrix framebuffer. The rendered frame is
   * SCREENWIDTH x SCREENHEIGHT bytes (NOT CONFIG_DOOM_X_RES: the engine packs 2
   * columns per output pixel, so SCREENWIDTH = CONFIG_DOOM_X_RES/2). Using the
   * wrong stride here overran the backbuffer and bus-faulted past RAM. */
  while (1) {
    k_sem_take(&sem_renderer, K_FOREVER);
    const uint8_t *frame = (const uint8_t *)I_GetBackBuffer();

    if (cplx_fb != NULL) {
      const int sw = SCREENWIDTH, sh = SCREENHEIGHT;
      int npx = cplx_w * cplx_h;
      if (npx > CPLX_MAXPX) npx = CPLX_MAXPX;

      /* Pass 1: box-downscale into per-cell averaged luma (full 0..255). */
      for (int oy = 0; oy < cplx_h; oy++) {
        int y0 = (oy * sh) / cplx_h;
        int y1 = ((oy + 1) * sh) / cplx_h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int ox = 0; ox < cplx_w; ox++) {
          int x0 = (ox * sw) / cplx_w;
          int x1 = ((ox + 1) * sw) / cplx_w;
          if (x1 <= x0) x1 = x0 + 1;
          uint32_t acc = 0, cnt = 0;
          for (int sy = y0; sy < y1; sy++) {
            const uint8_t *row = frame + sy * sw;
            for (int sx = x0; sx < x1; sx++) {
              acc += cplx_lum[row[sx]];
              cnt++;
            }
          }
          int idx = oy * cplx_w + ox;
          if (idx < CPLX_MAXPX) cplx_avg[idx] = cnt ? (uint8_t)(acc / cnt) : 0;
        }
      }

      /* Pass 2: per-frame auto-contrast + brightening gamma. DOOM scenes are
       * dark, so a fixed linear map to 0..7 leaves the matrix dim and flat
       * (most cells land on 0..2). Instead, stretch this frame's [min,max] luma
       * across the full 0..255, then apply a gamma<1 curve (the thr[] table,
       * derived from level boundaries at (L/7)^1.67) that lifts midtones so all
       * 8 brightness levels get used. A range floor stops near-flat frames from
       * being amplified into noise. */
      int mn = 255, mx = 0;
      for (int i = 0; i < npx; i++) {
        int v = cplx_avg[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
      int range = mx - mn;
      if (range < 24) range = 24;
      static const uint8_t thr[7] = {10, 32, 62, 100, 145, 197, 240};
      for (int i = 0; i < npx; i++) {
        int s = ((cplx_avg[i] - mn) * 255) / range;
        if (s > 255) s = 255;
        else if (s < 0) s = 0;
        uint8_t lvl = 0;
        for (int k = 0; k < 7; k++)
          if (s >= thr[k]) lvl = k + 1;
        cplx_fb[i] = lvl;
      }
    }
    k_sem_give(&sem_display);
  }
}
#else
void Z_DisplayThreadEntry(void) {
  struct display_buffer_descriptor buf_desc;

  buf_desc.buf_size = CONFIG_DOOM_X_RES * CONFIG_DOOM_Y_RES;
  buf_desc.pitch = CONFIG_DOOM_X_RES;
  buf_desc.width = CONFIG_DOOM_X_RES;
  buf_desc.height = 1;

  int i, j;

  uint16_t *framebuffer = (uint16_t *)&backbuffer[0];
  unsigned char *frame;

  while (1) {
    k_sem_take(&sem_renderer, K_FOREVER);
    frame = (char *)I_GetBackBuffer();
    int lineheight;
    int y_offset = 0;

    /* Re-use Backbuffer while converting indexed color (8-bit) to 565 (16-bit)
     */
    for (i = 1; y_offset < CONFIG_DOOM_Y_RES; i++) {
      lineheight = i + (1 << (i - 1));

      if (lineheight + y_offset >= CONFIG_DOOM_Y_RES) {
        lineheight = CONFIG_DOOM_Y_RES - y_offset;
      }

      for (j = 0; j < CONFIG_DOOM_X_RES * lineheight; j++) {
        framebuffer[j] = pl_565[frame[j]];
      }

      frame += CONFIG_DOOM_X_RES * lineheight;
      buf_desc.height = lineheight;

      display_write(display_dev, 0, y_offset, &buf_desc, framebuffer);

      y_offset += lineheight;
    }
    k_sem_give(&sem_display);
  }
}
#endif /* CONFIG_DOOM_CHARLIEPLEX */

//**************************************************************************************

void I_InitScreen_zephyr() {
  printf("I_CreateWindow_e32\n");

  display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
  if (!device_is_ready(display_dev)) {
    LOG_ERR("Device %s not found. Aborting sample.", display_dev->name);
    return;
  }

  LOG_INF("Display sample for %s", display_dev->name);
  display_get_capabilities(display_dev, &d_capabilities);
  LOG_INF("Display %ix%i", d_capabilities.x_resolution,
          d_capabilities.y_resolution);

#if defined(CONFIG_DOOM_CHARLIEPLEX)
  /* Matrix grayscale path: cache dimensions + the driver brightness buffer. */
  cplx_w = d_capabilities.x_resolution;
  cplx_h = d_capabilities.y_resolution;
  cplx_fb = (uint8_t *)display_get_framebuffer(display_dev);
  if (cplx_fb == NULL) {
    LOG_ERR("charlieplex: display_get_framebuffer() returned NULL");
  }
#elif defined(CONFIG_DOOM_RGB888)
  display_set_pixel_format(display_dev, PIXEL_FORMAT_RGB_888); // TODO
#elif defined(CONFIG_DOOM_RGB565)
  display_set_pixel_format(display_dev, PIXEL_FORMAT_RGB_565);
#elif defined(CONFIG_DOOM_BGR565)
  display_set_pixel_format(display_dev, PIXEL_FORMAT_BGR_565);
#endif
  display_blanking_off(display_dev);

#ifdef CONFIG_DOOM_SHOW_NXP_LOGO
  struct display_buffer_descriptor buf_desc;

  buf_desc.buf_size = 80 * 320 * 2;
  buf_desc.pitch = 80;
  buf_desc.width = 80;
  buf_desc.height = 320;
  for (int i = 0; i < 6; i++) {
    display_write(display_dev, 80 * i, 0, &buf_desc, nxp_80_320);
  }

#endif

  k_sem_init(&sem_display, 0, K_SEM_MAX_LIMIT);
  k_sem_init(&sem_renderer, 0, K_SEM_MAX_LIMIT);

  /* Start display thread*/
  k_thread_create(&display_thread, display_stack, STACKSIZE,
                  (k_thread_entry_t)Z_DisplayThreadEntry, NULL, NULL, NULL,
                  K_PRIO_COOP(7), 0, K_NO_WAIT);

  vid_width = SCREENWIDTH;
  vid_height = SCREENHEIGHT;

  for (int i = 0; i < KEYMAP_SIZE; i++) {
    if (keymap[i].node.port == 0) {
      printk("Error: key %ls is not configured\n", keymap[i].key);
    } else {
      gpio_pin_configure_dt(&keymap[i].node, GPIO_INPUT);
    }
  }
}

//**************************************************************************************

void I_BlitScreenBmp_e32() {}

//**************************************************************************************

void I_StartWServEvents_e32() {}

//**************************************************************************************

#define ADC_CENTER 2000
#define ADC_THRESH 600
#define ADC_RELEASE_TRESH 400

#if defined(CONFIG_DOOM_SPI_INPUT)
/* ------------------------------------------------------------------------
 * SPI controller input.
 *
 * An SPI slave on SPI3 receives 64-byte key-state blocks from the QCS Linux
 * /dev/spidev master and exposes the current key bitmask in spi_key_state.
 * I_SpiPollKeys() (called each tic from I_PollWServEvents_e32) edge-detects
 * the bitmask and posts DOOM key events. Block protocol + RDY (PG13) flow
 * control mirror the charlieplex SPI bridge exactly.
 *
 * Key bitmask (host -> MCU), payload byte 0 of a TYPE_KEYS block:
 *   bit0 forward  bit1 back   bit2 turn-left  bit3 turn-right
 *   bit4 fire     bit5 use    bit6 strafe-left bit7 strafe-right
 * ---------------------------------------------------------------------- */
#define SPI_BLOCK_SIZE 64
#define SPI_CRC_OFFSET (SPI_BLOCK_SIZE - 2)
#define SPI_BLK_MAGIC 0xA5
#define SPI_BLK_VERSION 0x01
#define SPI_TYPE_KEYS 0x20
#define SPI_TYPE_STATUS 0x10

#define KEYBIT_FWD (1u << 0)
#define KEYBIT_BACK (1u << 1)
#define KEYBIT_LEFT (1u << 2)
#define KEYBIT_RIGHT (1u << 3)
#define KEYBIT_FIRE (1u << 4)
#define KEYBIT_USE (1u << 5)
#define KEYBIT_SL (1u << 6)
#define KEYBIT_SR (1u << 7)

static const struct device *const spi_in_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));
static const struct gpio_dt_spec spi_in_rdy =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

/* 2 MHz: measured safe max for the IRQ-mode STM32U5 SPI slave. */
static const struct spi_config spi_in_cfg = {
    .frequency = 2000000U,
    .operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave = 0,
};

static uint8_t spi_in_tx[SPI_BLOCK_SIZE];
static uint8_t spi_in_rx[SPI_BLOCK_SIZE];

volatile uint32_t g_spi_keys;    /* current key bitmask from the host (SWD-visible) */
volatile uint32_t g_spi_blocks;  /* total blocks received (SWD proof) */
volatile uint32_t g_spi_crc_err;
volatile uint32_t g_spi_last_ms; /* uptime of the last received block (watchdog) */

/* If the host stops sending (crash/disconnect), release all keys after this
 * long so the player never gets stuck moving. The host resends state at ~60 Hz,
 * so a live link refreshes well within this window. */
#define SPI_INPUT_TIMEOUT_MS 300

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), matching host binascii.crc_hqx. */
static uint16_t spi_crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static void spi_in_stage_status(uint8_t last_seq) {
  /* Minimal outbound block so the full-duplex master always reads something
   * sane. Echoes the last seq + current key bitmask for host-side debugging. */
  memset(spi_in_tx, 0, SPI_BLOCK_SIZE);
  spi_in_tx[0] = SPI_BLK_MAGIC;
  spi_in_tx[1] = SPI_BLK_VERSION;
  spi_in_tx[2] = SPI_TYPE_STATUS;
  spi_in_tx[3] = last_seq;
  spi_in_tx[4] = 4;
  spi_in_tx[5] = (uint8_t)g_spi_keys;
  spi_in_tx[6] = (uint8_t)g_spi_blocks;
  spi_in_tx[7] = (uint8_t)g_spi_crc_err;
  spi_in_tx[8] = 0;
  uint16_t crc = spi_crc16(spi_in_tx, SPI_CRC_OFFSET);
  spi_in_tx[SPI_CRC_OFFSET] = (uint8_t)(crc & 0xff);
  spi_in_tx[SPI_CRC_OFFSET + 1] = (uint8_t)((crc >> 8) & 0xff);
}

static void spi_in_process(const uint8_t *blk) {
  if (blk[0] != SPI_BLK_MAGIC || blk[1] != SPI_BLK_VERSION)
    return;
  uint16_t want = spi_crc16(blk, SPI_CRC_OFFSET);
  uint16_t have =
      (uint16_t)blk[SPI_CRC_OFFSET] | ((uint16_t)blk[SPI_CRC_OFFSET + 1] << 8);
  if (want != have) {
    g_spi_crc_err++;
    return;
  }
  if (blk[2] == SPI_TYPE_KEYS && blk[4] >= 1)
    g_spi_keys = blk[5];
}

static void spi_in_thread(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  if (!device_is_ready(spi_in_dev) || !gpio_is_ready_dt(&spi_in_rdy))
    return;
  gpio_pin_configure_dt(&spi_in_rdy, GPIO_OUTPUT_INACTIVE);

  const struct spi_buf tx = {.buf = spi_in_tx, .len = SPI_BLOCK_SIZE};
  const struct spi_buf rx = {.buf = spi_in_rx, .len = SPI_BLOCK_SIZE};
  const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
  const struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

  spi_in_stage_status(0);

  while (1) {
    /* Armed and waiting: tell the host it may clock a block. */
    gpio_pin_set_dt(&spi_in_rdy, 1);
    int ret = spi_transceive(spi_in_dev, &spi_in_cfg, &tx_set, &rx_set);
    if (ret < 0) {
      gpio_pin_set_dt(&spi_in_rdy, 0);
      k_sleep(K_MSEC(5));
      continue;
    }
    /* Drop RDY while we parse so the host won't clock mid-processing. */
    gpio_pin_set_dt(&spi_in_rdy, 0);
    g_spi_blocks++;
    g_spi_last_ms = k_uptime_get_32();
    spi_in_process(spi_in_rx);
    spi_in_stage_status(spi_in_rx[3]);
  }
}

K_THREAD_DEFINE(spi_in_tid, 1536, spi_in_thread, NULL, NULL, NULL, 6, 0, 0);

/* Edge-detect the host key bitmask and post DOOM key up/down events. Called on
 * the main thread each tic, so the event queue is only touched from one
 * thread. */
static void I_SpiPollKeys(void) {
  static uint32_t prev;
  uint32_t cur = g_spi_keys;
  /* Watchdog: if the host has gone quiet, drop all keys so nothing sticks. */
  if (g_spi_blocks == 0 ||
      (k_uptime_get_32() - g_spi_last_ms) > SPI_INPUT_TIMEOUT_MS)
    cur = 0;
  uint32_t changed = cur ^ prev;
  if (!changed) {
    prev = cur;
    return;
  }

  static const struct {
    uint32_t bit;
    const int *key;
  } map[] = {
      {KEYBIT_FWD, &key_up},      {KEYBIT_BACK, &key_down},
      {KEYBIT_LEFT, &key_left},   {KEYBIT_RIGHT, &key_right},
      {KEYBIT_FIRE, &key_fire},   {KEYBIT_USE, &key_use},
      {KEYBIT_SL, &key_strafeleft}, {KEYBIT_SR, &key_straferight},
  };

  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    if (!(changed & map[i].bit))
      continue;
    event_t ev = {0};
    ev.type = (cur & map[i].bit) ? ev_keydown : ev_keyup;
    ev.data1 = *map[i].key;
    D_PostEvent(&ev);
  }
  prev = cur;
}
#endif /* CONFIG_DOOM_SPI_INPUT */

void I_PollWServEvents_e32() {
  event_t event = {0};
#if defined(CONFIG_DOOM_SPI_INPUT)
  /* SPI is the sole input source on this board. Return before the legacy
   * ADC-joystick / touch polling below: with no joystick, action_pressed is
   * never set, so that code's else-branch posts an UNCONDITIONAL keyup for
   * key_fire/key_enter every call -- which would immediately cancel the
   * keydown I_SpiPollKeys just posted, so fire never registered. */
  I_SpiPollKeys();
  return;
#endif

  if (x_center) {
    event.type = ev_keyup;
    event.data1 = key_left;
    D_PostEvent(&event);
    event.type = ev_keyup;
    event.data1 = key_right;
    D_PostEvent(&event);
  } else if (x_l_pressed) {
    event.type = ev_keydown;
    event.data1 = key_left;
    D_PostEvent(&event);
  } else if (x_r_pressed) {
    event.type = ev_keydown;
    event.data1 = key_right;
    D_PostEvent(&event);
  }

  x_l_pressed = false;
  x_r_pressed = false;
  x_center = false;

  if (y_center) {
    event.type = ev_keyup;
    event.data1 = key_up;
    D_PostEvent(&event);
    event.type = ev_keyup;
    event.data1 = key_down;
    D_PostEvent(&event);
  } else if (y_u_pressed) {
    event.type = ev_keydown;
    event.data1 = key_up;
    D_PostEvent(&event);
  } else if (y_d_pressed) {
    event.type = ev_keydown;
    event.data1 = key_down;
    D_PostEvent(&event);
  }

  y_u_pressed = false;
  y_d_pressed = false;
  y_center = false;

  if (action_pressed && action_fire) {
    event.type = ev_keydown;
    event.data1 = key_fire;
    D_PostEvent(&event);
    action_pressed = false;
  } else if (action_pressed && !action_fire) {
    event.type = ev_keydown;
    event.data1 = key_enter;
    D_PostEvent(&event);
    action_pressed = false;
  } else {
    event.type = ev_keyup;
    event.data1 = key_fire;
    D_PostEvent(&event);
    event.type = ev_keyup;
    event.data1 = key_enter;
    D_PostEvent(&event);
  }

  for (int i = 0; i < KEYMAP_SIZE; i++) {
    if (keymap[i].node.port != 0) {
      if (gpio_pin_get_dt(&keymap[i].node) > 0 &&
          keymap[i].eventstate == ev_keyup) {
        keymap[i].eventstate = ev_keydown;
        event.type = ev_keydown;
        event.data1 = *keymap[i].key;
        D_PostEvent(&event);
      } else if (gpio_pin_get_dt(&keymap[i].node) == 0 &&
                 keymap[i].eventstate == ev_keydown) {
        keymap[i].eventstate = ev_keyup;
        event.type = ev_keyup;
        event.data1 = *keymap[i].key;
        D_PostEvent(&event);
      }
    }
  }
}

#ifdef CONFIG_DOOM_ZEPHYR_ADC_JOYSTICK
static void input_adc_joy_cb(struct input_event *evt, void *user_data) {

  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
    if (evt->value == -1) {
      x_l_pressed = true;
    } else if (evt->value == 0) {
      x_center = true;
    } else if (evt->value == 1) {
      x_r_pressed = true;
    }
  }

  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
    if (evt->value == -1) {
      y_d_pressed = true;
    } else if (evt->value == 0) {
      y_center = true;
    } else if (evt->value == 1) {
      y_u_pressed = true;
    }
  }
}

INPUT_CALLBACK_DEFINE(adc_joy_dev, input_adc_joy_cb, NULL);
#endif

#ifdef CONFIG_DOOM_ZEPHYR_TOUCH_SCREEN
static void input_touch_cb(struct input_event *evt, void *user_data) {

#ifdef CONFIG_DOOM_ZEPHYR_TOUCH_SCREEN_SWAP_XY
  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
    touch_y = evt->value;
  }

#ifdef CONFIG_DOOM_ZEPHYR_TOUCH_SCREEN_INVERT_X
  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
    touch_x = d_capabilities.x_resolution - evt->value;
  }
#else
  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
    touch_x = evt->value;
  }
#endif
#else
  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
    touch_x = evt->value;
  }

  if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
    touch_y = evt->value;
  }
#endif
  if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH) {
    touch_btn = evt->value;
  }

  if (evt->sync) {

    if (!touch_btn) {
      touch_released = true;

      x_center = true;
      y_center = true;
      // STOP MOVING
    } else if (touch_y >= CONFIG_DOOM_X_RES) {
      x_center = true;
      y_center = true;

      if (touch_btn && touch_released) {
        action_pressed = true;
        action_fire = touch_x < (CONFIG_DOOM_Y_RES / 2);
      }
    } else if (touch_btn && touch_released) {
      touch_initial_x = touch_x;
      touch_initial_y = touch_y;
      touch_released = false;
      x_center = true;
      y_center = true;
    } else {

      if (touch_btn) {
        // CALC distance
        double distance = sqrt(pow(touch_x - touch_initial_x, 2) +
                               pow(touch_y - touch_initial_y, 2));

        double angle =
            atan2(touch_initial_y - touch_y, touch_initial_x - touch_x);

        angle = (angle) * (180.0 / M_PI);
        if (distance > 20) {

          if ((angle >= -22.5 && angle <= 22.5)) {
            // Down
            x_center = true;
            y_u_pressed = false;
            y_d_pressed = true;
          } else if (angle > 22.5 && angle <= 67.5) {
            // Left-down
            x_l_pressed = true;
            x_r_pressed = false;
            y_u_pressed = false;
            y_d_pressed = true;
          } else if (angle > 67.5 && angle <= 112.5) {
            // Left
            x_l_pressed = true;
            x_r_pressed = false;
            y_center = true;
          } else if (angle > 112.5 && angle <= 157.5) {
            // Left-Up
            x_l_pressed = true;
            x_r_pressed = false;
            y_u_pressed = true;
            y_d_pressed = false;
          } else if ((angle > 157.5 && angle <= 180) ||
                     (angle >= -180 && angle <= -157.5)) {
            // Up
            x_center = true;
            y_u_pressed = true;
            y_d_pressed = false;
          } else if (angle > -157.5 && angle <= -112.5) {
            // Right-up
            x_l_pressed = false;
            x_r_pressed = true;
            y_u_pressed = true;
            y_d_pressed = false;
          } else if (angle > -112.5 && angle <= -67.5) {
            // Right
            x_l_pressed = false;
            x_r_pressed = true;
            y_center = true;
          } else if (angle > -67.5 && angle <= -22.5) {
            // Right-down
            x_l_pressed = false;
            x_r_pressed = true;
            y_u_pressed = false;
            y_d_pressed = true;
          } else {
            x_center = true;
            y_center = true;
          }

        } else {

          x_center = true;
          y_center = true;
        }
      }
    }
  }
}

INPUT_CALLBACK_DEFINE(touch_screen_dev, input_touch_cb, NULL);
#endif

//**************************************************************************************

void I_ClearWindow_e32() {}

unsigned short *I_GetBackBuffer() {
  return &backbuffer[CONFIG_DOOM_X_RES]; // Offset by SCREENWIDTH for color
                                         // conversion space
}

unsigned short *I_GetFrontBuffer() {
#ifndef CONFIG_DOOM_NO_WIPE
  return &frontbuffer[0];
#else
  return NULL;
#endif
}

//**************************************************************************************

void I_CreateWindow_e32() {
  // display_blanking_off(display_dev);
}

//**************************************************************************************

void I_CreateBackBuffer_zephyr() { I_CreateWindow_e32(); }

//**************************************************************************************

void I_FinishUpdate_zephyr(const byte *srcBuffer, const byte *pallete,
                        const unsigned int width, const unsigned int height) {

  k_sem_give(&sem_renderer);

  /* Wait for coop thread to let us have a turn */
  k_sem_take(&sem_display, K_FOREVER);
}

//**************************************************************************************

void I_SetPallete_zephyr(const byte *pallete) {
#if defined(CONFIG_DOOM_CHARLIEPLEX)
  /* Build a palette-index -> luminance(0..CPLX_LEVELS-1) table. Each DOOM
   * frame pixel is an index into this 256-entry palette; we precompute its
   * brightness here so the per-frame downscale is just table lookups + averaging.
   */
  for (int i = 0; i < 256; i++) {
    int r = *pallete++;
    int g = *pallete++;
    int b = *pallete++;
    /* Rec.601 luma, 0..255. Keep the full range here; the per-frame
     * auto-contrast + gamma in Z_DisplayThreadEntry maps it to 0..7. */
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    cplx_lum[i] = (uint8_t)y;
  }
#elif defined(CONFIG_DOOM_RGB565)
  for (int i = 0; i < 256; i++) {
    int r = *pallete++;
    int g = *pallete++;
    int b = *pallete++;
    int nr = r >> 3, ng = g >> 2, nb = b >> 3;
    pl_565[i] = (uint16_t)((nr << 11) | (ng << 5) | nb);
    pl_565[i] = doom_swap_s(pl_565[i]);
  }
#elif defined(CONFIG_DOOM_BGR565)
  for (int i = 0; i < 256; i++) {
    int r = *pallete++;
    int g = *pallete++;
    int b = *pallete++;
    int nr = r >> 3, ng = g >> 2, nb = b >> 3;
    pl_565[i] = (uint16_t)((nr << 11) | (ng << 5) | nb);
  }
#else
  pl_game = pallete;
#endif
}

//**************************************************************************************

int I_GetVideoWidth_zephyr() { return vid_width; }

//**************************************************************************************

int I_GetVideoHeight_zephyr() { return vid_height; }

//**************************************************************************************

void I_ProcessKeyEvents() { I_PollWServEvents_e32(); }

//**************************************************************************************

#define MAX_MESSAGE_SIZE 512

void I_Error(const char *error, ...) {
  char msg[MAX_MESSAGE_SIZE];

  va_list v;
  va_start(v, error);

  vsprintf(msg, error, v);

  va_end(v);

  printf("%s\n", msg);

  fflush(stderr);
  fflush(stdout);

  I_Quit_zephyr();
}

//**************************************************************************************

void I_Quit_zephyr() {}

//**************************************************************************************