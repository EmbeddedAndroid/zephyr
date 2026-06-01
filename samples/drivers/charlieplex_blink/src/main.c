/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Charlieplex LED-matrix blink, OTA-capable -- the A/B OTA *source* image for
 * the charlieplex_bridge demo. It does two things at once:
 *
 *   1. A background thread blinks the whole LED matrix on/off (so the board is
 *      visibly "the blink app" with nothing plugged in).
 *   2. The main loop runs the SAME SPI-slave + RDY flow-control + OTA-receive
 *      protocol as charlieplex_bridge, so you can OTA it UP to the full bridge
 *      firmware over the SPI link.
 *
 * Demo direction: boot this (matrix auto-blinks) -> OTA the bridge image ->
 * the auto-blink stops and the board becomes the Linux-driven SPI bridge. The
 * visible before/after is the proof.
 *
 * It reports its version as "ZEPHYR <ver> BLINK" so the running image is
 * unambiguous over the bridge version query. Built MCUboot-signed with the
 * same sysbuild config and ECDSA-P256 sample key as the bridge.
 *
 * Wire protocol is identical to charlieplex_bridge (see that file's header):
 * 64-byte blocks, magic 0xA5, CRC-16/CCITT-FALSE, TYPE_OTA_* + TYPE_CMD.
 *
 * Not for upstream: development/demo branch only.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

LOG_MODULE_REGISTER(charlieplex_blink, LOG_LEVEL_INF);

#define BLOCK_SIZE  64
#define CRC_OFFSET  (BLOCK_SIZE - 2)
#define BLK_MAGIC   0xA5
#define BLK_VERSION 0x01

#define TYPE_FB        0x01
#define TYPE_CMD       0x02
#define TYPE_OTA_BEGIN 0x03
#define TYPE_OTA_DATA  0x04
#define TYPE_OTA_END   0x05
#define TYPE_STATUS    0x10
#define TYPE_VERSION   0x11

#define CMD_BLANK_ON   1
#define CMD_BLANK_OFF  2
#define CMD_BRIGHTNESS 3
#define CMD_QUERY      4
#define CMD_VERSION    5
#define CMD_CONFIRM    6

#define MAX_FB_BYTES 32

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));

static const struct gpio_dt_spec rdy =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

static const struct spi_config spi_cfg = {
	.frequency = 2000000U,
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

/* MCU->host status payload (matches charlieplex_bridge bit-for-bit). */
struct bridge_status {
	uint32_t frames_drawn;
	uint32_t blocks;
	uint32_t crc_err;
	uint8_t  last_seq;
	uint8_t  last_type;
	uint8_t  state;
	uint8_t  rdy;
	uint32_t ota_written;
	uint8_t  confirmed;
	uint8_t  ota_err;
} __packed;

volatile uint32_t g_state;
volatile uint32_t g_blocks;
volatile uint32_t g_frames_drawn;
volatile uint32_t g_cmds;
volatile uint32_t g_rx_crc_err;
volatile uint32_t g_rdy_level;

static bool pending_version;

static struct flash_img_context ota_ctx;
static bool ota_active;
static bool ota_inited; /* ota_ctx has been flash_img_init'd at least once */
static uint8_t ota_err;
static bool pending_reboot;

/* Set true while OTA is in progress so the blink thread stops touching the
 * display (keep the SPI/flash path uncontended during the update).
 */
static volatile bool blink_paused;

static uint8_t spi_tx[BLOCK_SIZE];
static uint8_t spi_rx[BLOCK_SIZE];

static uint16_t panel_w, panel_h, panel_stride, fb_bytes;
static volatile bool panel_ready;

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static void set_rdy(int level)
{
	gpio_pin_set_dt(&rdy, level);
	g_rdy_level = level;
}

static void blit(const uint8_t *fb)
{
	struct display_buffer_descriptor desc = {
		.buf_size = fb_bytes,
		.width = panel_w,
		.height = panel_h,
		.pitch = panel_stride * 8,
		.frame_incomplete = false,
	};

	if (display_write(disp, 0, 0, &desc, fb) == 0) {
		g_frames_drawn++;
	}
}

/* Background blink: fill the matrix, blank it, repeat. Paused during OTA. */
static void blink_thread(void)
{
	uint8_t fb[MAX_FB_BYTES];
	bool on = false;

	while (!panel_ready) {
		k_sleep(K_MSEC(20));
	}

	while (1) {
		if (!blink_paused) {
			memset(fb, on ? 0xFF : 0x00, fb_bytes);
			blit(fb);
			on = !on;
		}
		k_sleep(K_MSEC(400));
	}
}

K_THREAD_DEFINE(blink_tid, 1024, blink_thread, NULL, NULL, NULL, 7, 0, 0);

static void handle_cmd(uint8_t cmd, uint8_t arg)
{
	switch (cmd) {
	case CMD_BLANK_ON:
		display_blanking_on(disp);
		break;
	case CMD_BLANK_OFF:
		display_blanking_off(disp);
		break;
	case CMD_BRIGHTNESS:
		display_set_brightness(disp, arg);
		break;
	case CMD_VERSION:
		pending_version = true;
		break;
	case CMD_CONFIRM:
		boot_write_img_confirmed();
		break;
	case CMD_QUERY:
	default:
		break;
	}
	g_cmds++;
}

/* ---- OTA: stream a signed image into slot1, then ask MCUboot to swap ---- */

static void ota_begin(void)
{
	ota_err = 0;
	blink_paused = true; /* stop drawing while we erase/program flash */
	if (flash_img_init(&ota_ctx) != 0) {
		ota_err = 1;
		ota_active = false;
		return;
	}
	ota_inited = true;
	ota_active = true;
}

static void ota_data(const uint8_t *chunk, uint8_t len)
{
	if (!ota_active) {
		ota_err = 2;
		return;
	}
	if (flash_img_buffered_write(&ota_ctx, chunk, len, false) != 0) {
		ota_err = 3;
		ota_active = false;
	}
}

static void ota_end(uint8_t permanent)
{
	if (!ota_active) {
		ota_err = 2;
		return;
	}
	if (flash_img_buffered_write(&ota_ctx, NULL, 0, true) != 0) {
		ota_err = 4;
		ota_active = false;
		return;
	}
	ota_active = false;

	if (boot_request_upgrade(permanent ? BOOT_UPGRADE_PERMANENT
					   : BOOT_UPGRADE_TEST) != 0) {
		ota_err = 5;
		return;
	}
	pending_reboot = true;
}

static uint8_t process_block(const uint8_t *blk)
{
	if (blk[0] != BLK_MAGIC || blk[1] != BLK_VERSION) {
		return 0;
	}

	uint16_t want = crc16(blk, CRC_OFFSET);
	uint16_t have = (uint16_t)blk[CRC_OFFSET] |
			((uint16_t)blk[CRC_OFFSET + 1] << 8);
	if (want != have) {
		g_rx_crc_err++;
		return 0;
	}

	uint8_t type = blk[2];
	uint8_t seq = blk[3];
	uint8_t len = blk[4];

	if (len > (CRC_OFFSET - 5)) {
		len = CRC_OFFSET - 5;
	}

	switch (type) {
	case TYPE_CMD:
		if (len >= 2) {
			handle_cmd(blk[5], blk[6]);
		}
		break;
	case TYPE_OTA_BEGIN:
		ota_begin();
		break;
	case TYPE_OTA_DATA:
		ota_data(&blk[5], len);
		break;
	case TYPE_OTA_END:
		ota_end(len >= 1 ? blk[5] : 0);
		break;
	default:
		break;
	}
	return seq;
}

static void stamp_crc(void)
{
	uint16_t crc = crc16(spi_tx, CRC_OFFSET);

	spi_tx[CRC_OFFSET] = (uint8_t)(crc & 0xff);
	spi_tx[CRC_OFFSET + 1] = (uint8_t)((crc >> 8) & 0xff);
}

static void pack_version(uint8_t last_seq)
{
	static const char ver[] = "ZEPHYR " KERNEL_VERSION_STRING " BLINK";
	uint8_t len = (uint8_t)MIN(sizeof(ver) - 1, (size_t)(CRC_OFFSET - 5));

	memset(spi_tx, 0, BLOCK_SIZE);
	spi_tx[0] = BLK_MAGIC;
	spi_tx[1] = BLK_VERSION;
	spi_tx[2] = TYPE_VERSION;
	spi_tx[3] = last_seq;
	spi_tx[4] = len;
	memcpy(&spi_tx[5], ver, len);
	stamp_crc();
	set_rdy(1);
}

static void pack_status(uint8_t last_seq, uint8_t last_type)
{
	struct bridge_status st = {
		.frames_drawn = g_frames_drawn,
		.blocks = g_blocks,
		.crc_err = g_rx_crc_err,
		.last_seq = last_seq,
		.last_type = last_type,
		.state = (uint8_t)g_state,
		.rdy = (uint8_t)g_rdy_level,
		/* Only query the DFU context once an OTA has actually initialized it.
		 * flash_img_bytes_written() on a zeroed ota_ctx walks into the flash
		 * layer and, right after an MCUboot swap+reboot, wedges in a flash
		 * erase -- which hung this app at boot (before the SPI loop even
		 * started) and is why blink would never light after an OTA. Reporting
		 * 0 until ota_inited keeps the boot path off flash entirely.
		 */
		.ota_written = ota_inited ?
			(uint32_t)flash_img_bytes_written(&ota_ctx) : 0,
		.confirmed = boot_is_img_confirmed() ? 1 : 0,
		.ota_err = ota_err,
	};

	memset(spi_tx, 0, BLOCK_SIZE);
	spi_tx[0] = BLK_MAGIC;
	spi_tx[1] = BLK_VERSION;
	spi_tx[2] = TYPE_STATUS;
	spi_tx[3] = last_seq;
	spi_tx[4] = sizeof(st);
	memcpy(&spi_tx[5], &st, sizeof(st));
	stamp_crc();

	set_rdy(1);
}

static void pack_response(uint8_t last_seq, uint8_t last_type)
{
	if (pending_version) {
		pending_version = false;
		pack_version(last_seq);
	} else {
		pack_status(last_seq, last_type);
	}
}

int main(void)
{
	struct display_capabilities caps;

	LOG_INF("charlieplex blink (OTA-capable): auto-blink + SPI OTA receiver");

	if (!device_is_ready(disp) || !device_is_ready(spi_dev) ||
	    !gpio_is_ready_dt(&rdy)) {
		g_state = 0xE0;
		return -1;
	}

	display_get_capabilities(disp, &caps);
	panel_w = caps.x_resolution;
	panel_h = caps.y_resolution;
	panel_stride = (panel_w + 7) / 8;
	fb_bytes = panel_stride * panel_h;
	if (fb_bytes > MAX_FB_BYTES) {
		g_state = 0xE1;
		return -1;
	}

	gpio_pin_configure_dt(&rdy, GPIO_OUTPUT_INACTIVE);
	set_rdy(0);
	display_blanking_off(disp);

	g_state = 1;
	panel_ready = true; /* release the blink thread */

	const struct spi_buf tx = { .buf = spi_tx, .len = BLOCK_SIZE };
	const struct spi_buf rx = { .buf = spi_rx, .len = BLOCK_SIZE };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

	pack_status(0, 0);

	while (1) {
		int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);

		if (ret < 0) {
			g_state = 0xE4;
			k_sleep(K_MSEC(10));
			continue;
		}
		g_blocks++;

		/* Drop RDY while processing (OTA may run a slow flash op);
		 * pack_response() raises it once the next reply is staged.
		 */
		set_rdy(0);

		uint8_t seq = process_block(spi_rx);

		pack_response(seq, spi_rx[2]);

		if (pending_reboot) {
			k_sleep(K_MSEC(200));
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	return 0;
}
