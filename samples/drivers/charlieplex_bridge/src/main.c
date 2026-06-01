/*
 * Bidirectional SPI bridge between the QCS Linux side (SPI master,
 * /dev/spidevN.M) and the Arduino UNO Q STM32U585 (SPI slave), driving the
 * on-board charlieplex LED matrix through the Zephyr display API.
 *
 * Reuses the proven SPI-slave + RDY flow-control + CRC-16 block protocol from
 * the earlier CAN<->SPI bridge (measured: clean 1-3 MHz, 2 MHz safe max for the
 * IRQ-mode slave). Here the records carry display frames and commands instead
 * of CAN frames.
 *
 * SPI is full-duplex: each transfer the master clocks a command block out on
 * MOSI while simultaneously reading the MCU's status block on MISO. The status
 * therefore reflects the *previous* transfer's processing (one-transfer
 * pipeline), which is expected for this slave model.
 *
 * Block format (fixed BLOCK_SIZE, MUST match the host -- the STM32 slave is
 * byte-count driven):
 *   [0] magic 0xA5  [1] version  [2] type  [3] seq  [4] len(payload bytes)
 *   [5 ..] payload
 *   [BLOCK_SIZE-2 .. -1] CRC-16/CCITT-FALSE over bytes [0 .. BLOCK_SIZE-3] (LE)
 *
 * Types (host -> MCU on MOSI):
 *   TYPE_FB  0x01  payload = packed MONO01 framebuffer (stride*height bytes),
 *                  row-major, written verbatim through display_write().
 *   TYPE_CMD 0x02  payload[0] = cmd, payload[1] = arg:
 *                    CMD_BLANK_ON 1, CMD_BLANK_OFF 2,
 *                    CMD_BRIGHTNESS 3 (arg 0..255), CMD_QUERY 4 (status only)
 *
 * Type (MCU -> host on MISO):
 *   TYPE_STATUS 0x10  payload = struct bridge_status (LE fields).
 *
 * SWD proof globals (re-nm each build): g_state, g_blocks, g_frames_drawn,
 * g_cmds, g_rx_crc_err, g_rdy_level.
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

LOG_MODULE_REGISTER(charlieplex_bridge, LOG_LEVEL_INF);

#define BLOCK_SIZE  64
#define CRC_OFFSET  (BLOCK_SIZE - 2)
#define BLK_MAGIC   0xA5
#define BLK_VERSION 0x01

#define TYPE_FB        0x01
#define TYPE_CMD       0x02
#define TYPE_OTA_BEGIN 0x03  /* payload: u32 total image size; init slot1 write */
#define TYPE_OTA_DATA  0x04  /* payload: next image chunk, streamed to slot1 */
#define TYPE_OTA_END   0x05  /* payload[0]: permanent flag; request swap + reboot */
#define TYPE_STATUS    0x10
#define TYPE_VERSION   0x11

#define CMD_BLANK_ON   1
#define CMD_BLANK_OFF  2
#define CMD_BRIGHTNESS 3
#define CMD_QUERY      4
#define CMD_VERSION    5    /* host asks for the MCU's Zephyr version string */
#define CMD_CONFIRM    6    /* mark the running image confirmed (cancel revert) */

#define MAX_FB_BYTES 32      /* >= stride*height for panels up to 16x16 mono */

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));

static const struct gpio_dt_spec rdy =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

/* 2 MHz: measured safe max for the IRQ-mode STM32U5 SPI slave (4 MHz+ underruns). */
static const struct spi_config spi_cfg = {
	.frequency = 2000000U,
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

/* MCU->host status payload (little-endian on the wire). */
struct bridge_status {
	uint32_t frames_drawn;
	uint32_t blocks;
	uint32_t crc_err;
	uint8_t  last_seq;
	uint8_t  last_type;
	uint8_t  state;
	uint8_t  rdy;
	uint32_t ota_written;   /* bytes streamed into slot1 so far */
	uint8_t  confirmed;     /* 1 if the running image is confirmed */
	uint8_t  ota_err;       /* last OTA error code, 0 = none */
} __packed;

volatile uint32_t g_state;
volatile uint32_t g_blocks;
volatile uint32_t g_frames_drawn;
volatile uint32_t g_cmds;
volatile uint32_t g_rx_crc_err;
volatile uint32_t g_rdy_level;

/* Set when the host requests the Zephyr version; the next outbound block then
 * carries a TYPE_VERSION payload instead of the normal status.
 */
static bool pending_version;

/* OTA streaming state. The DFU flash_img helper writes the incoming image into
 * the secondary slot (image-1); MCUboot swaps it in on the next boot.
 */
static struct flash_img_context ota_ctx;
static bool ota_active;
static bool ota_inited; /* ota_ctx has been flash_img_init'd at least once */
static uint8_t ota_err;
static bool pending_reboot;

static uint8_t spi_tx[BLOCK_SIZE];
static uint8_t spi_rx[BLOCK_SIZE];

static uint16_t panel_w, panel_h, panel_stride, fb_bytes;
static uint8_t framebuf[MAX_FB_BYTES];

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) -- matches host binascii.crc_hqx
 * and the earlier bridge firmware bit-for-bit.
 */
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

static void draw_frame(const uint8_t *fb)
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
		/* Cancel a pending revert: mark the running image good. */
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
	/* flush=false: buffered, flushed to flash as full blocks fill up. */
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
	/* Final flush of any partial buffer. */
	if (flash_img_buffered_write(&ota_ctx, NULL, 0, true) != 0) {
		ota_err = 4;
		ota_active = false;
		return;
	}
	ota_active = false;

	/* Ask MCUboot to boot slot1 next reset. permanent=0 is a revertible
	 * test swap (auto-reverts unless the new image confirms itself).
	 */
	if (boot_request_upgrade(permanent ? BOOT_UPGRADE_PERMANENT
					   : BOOT_UPGRADE_TEST) != 0) {
		ota_err = 5;
		return;
	}
	pending_reboot = true;
}

/* Parse one inbound MOSI block. Returns the processed seq. */
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
	case TYPE_FB:
		if (len >= fb_bytes) {
			memcpy(framebuf, &blk[5], fb_bytes);
			draw_frame(framebuf);
		}
		break;
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

/* Stage a TYPE_VERSION block carrying the MCU's Zephyr version string. */
static void pack_version(uint8_t last_seq)
{
	static const char ver[] = "ZEPHYR " KERNEL_VERSION_STRING;
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

/* Stage the status block for the master to read on the next transfer. */
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
		 * layer and, right after an MCUboot swap+reboot, can wedge in a flash
		 * erase, hanging this app at boot (before the SPI loop starts).
		 * Reporting 0 until ota_inited keeps the boot path off flash.
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

	/* Status is always available; raise RDY so the master knows it can read. */
	set_rdy(1);
}

/* Choose the outbound block: a one-shot version reply if requested, else status. */
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

	LOG_INF("charlieplex SPI bridge: Linux frames -> LED matrix");

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

		/* Drop RDY while we process this block: process_block() may run a
		 * slow flash erase/program (OTA), during which the SPI slave is not
		 * in spi_transceive and would silently drop any block the host
		 * clocks. pack_response() raises RDY again once the next reply is
		 * staged, so the host's RDY gate only clocks while we are waiting.
		 */
		set_rdy(0);

		uint8_t seq = process_block(spi_rx);

		pack_response(seq, spi_rx[2]);

		/* OTA_END asked us to reboot into the freshly-staged image. Give
		 * the host a moment to finish its last transfer, then reset so
		 * MCUboot performs the swap.
		 */
		if (pending_reboot) {
			k_sleep(K_MSEC(200));
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	return 0;
}
