/*
 * Bidirectional CAN-FD <-> SPI bridge with RDY flow control, for the Arduino
 * UNO Q (STM32U585).
 *
 * Full-duplex, lossless (RDY-gated). CAN runs in FD loopback (CAN_MODE_FD |
 * CAN_MODE_LOOPBACK) with BRS, so frames carry up to 64 data bytes.
 *
 *  - MOSI (Linux->MCU): command block; MCU parses it and can_send()s each
 *    record (as an FD/BRS frame). Loopback returns them.
 *  - MISO (MCU->Linux): up to RECS_PER_BLOCK looped-back RX frames.
 *  - RDY (STM32 PG13 == QCM gpiochip1:70): HIGH while unread data is staged or
 *    queued for the master.
 *
 * Block format (same both directions), fixed BLOCK_SIZE (MUST match master --
 * the STM32 slave is byte-count driven, see spi-bringup.md):
 *   [0] magic 0xA5  [1] ver  [2] count(0..RECS_PER_BLOCK)  [3] seq
 *   [4 + i*REC_SIZE] record:
 *       [0..3] id (LE u32)  [4] len (data bytes, 0..64)  [5] flags  [6..7] rsvd
 *       [8 ..] data (up to 64 bytes)
 * REC_SIZE = 8 + 64 = 72. BLOCK_SIZE = 4 + 3*72 = 220, rounded to 256.
 *
 * Record's [4] field carries the DATA BYTE COUNT (not the raw DLC) so the host
 * side stays DLC-agnostic; firmware converts to/from CAN DLC via the helpers.
 *
 * SWD proof globals (addrs move each rebuild, re-nm): g_state, g_can_rx,
 * g_blocks_sent, g_frames_packed, g_frames_injected, g_rdy_level.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(can_spi_bridge, LOG_LEVEL_INF);

#define BLOCK_SIZE  512
#define REC_SIZE    72            /* 8 header + 64 data */
#define MAX_DATA    64
#define MAX_RECS    7             /* (512 - 4 - 2 crc) / 72 = 7 */
#define BLK_MAGIC   0xA5
#define BLK_VERSION 0x03          /* 0x03 = CAN-FD bidirectional WITH CRC-16 */
#define CRC_OFFSET  (BLOCK_SIZE - 2)  /* CRC-16 stored in the last 2 bytes (LE) */

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));

static const struct gpio_dt_spec rdy =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

static const struct spi_config spi_cfg = {
	.frequency = 1000000U,
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

CAN_MSGQ_DEFINE(can_rx_msgq, 32);
CAN_MSGQ_DEFINE(ship_msgq, 64);

volatile uint32_t g_state;
volatile uint32_t g_can_rx;
volatile uint32_t g_blocks_sent;
volatile uint32_t g_frames_packed;
volatile uint32_t g_frames_injected;
volatile uint32_t g_rdy_level;
volatile uint32_t g_rx_crc_err;     /* inbound MOSI blocks failing CRC */

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Cheap, good for short blocks.
 * Zephyr provides crc16_ccitt() but a local impl keeps the host side trivially
 * matchable and avoids a header dependency. */
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

static uint8_t spi_tx[BLOCK_SIZE];
static uint8_t spi_rx[BLOCK_SIZE];

static void set_rdy(int level)
{
	gpio_pin_set_dt(&rdy, level);
	g_rdy_level = level;
}

static void inject_block(const uint8_t *blk)
{
	if (blk[0] != BLK_MAGIC || blk[1] != BLK_VERSION) {
		return;
	}
	/* Verify CRC-16 over the block body (everything before the CRC field).
	 * A corrupt or all-zero (master-sent-nothing) block is dropped here. */
	uint16_t want = crc16(blk, CRC_OFFSET);
	uint16_t have = (uint16_t)blk[CRC_OFFSET] | ((uint16_t)blk[CRC_OFFSET + 1] << 8);
	if (want != have) {
		g_rx_crc_err++;
		return;
	}
	uint8_t count = blk[2];
	if (count > MAX_RECS) {
		count = MAX_RECS;
	}
	for (uint8_t i = 0; i < count; i++) {
		const uint8_t *r = &blk[4 + i * REC_SIZE];
		struct can_frame f = {0};
		f.id = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
		       ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
		uint8_t nbytes = r[4];
		if (nbytes > MAX_DATA) {
			nbytes = MAX_DATA;
		}
		f.dlc = can_bytes_to_dlc(nbytes);
		/* Send as CAN-FD with bitrate switching. */
		f.flags = CAN_FRAME_FDF | CAN_FRAME_BRS;
		memcpy(f.data, &r[8], nbytes);
		if (can_send(can_dev, &f, K_MSEC(10), NULL, NULL) == 0) {
			g_frames_injected++;
		}
	}
}

static uint8_t pack_block(uint8_t seq)
{
	memset(spi_tx, 0, BLOCK_SIZE);
	spi_tx[0] = BLK_MAGIC;
	spi_tx[1] = BLK_VERSION;
	spi_tx[3] = seq;

	uint8_t n = 0;
	struct can_frame f;
	while (n < MAX_RECS && k_msgq_get(&ship_msgq, &f, K_NO_WAIT) == 0) {
		uint8_t *r = &spi_tx[4 + n * REC_SIZE];
		r[0] = (uint8_t)(f.id & 0xff);
		r[1] = (uint8_t)((f.id >> 8) & 0xff);
		r[2] = (uint8_t)((f.id >> 16) & 0xff);
		r[3] = (uint8_t)((f.id >> 24) & 0xff);
		uint8_t nbytes = can_dlc_to_bytes(f.dlc);
		if (nbytes > MAX_DATA) {
			nbytes = MAX_DATA;
		}
		r[4] = nbytes;
		r[5] = f.flags;
		memcpy(&r[8], f.data, nbytes);
		n++;
		g_frames_packed++;
	}
	spi_tx[2] = n;

	/* Stamp CRC-16 over the block body (outbound integrity for the master). */
	uint16_t crc = crc16(spi_tx, CRC_OFFSET);
	spi_tx[CRC_OFFSET] = (uint8_t)(crc & 0xff);
	spi_tx[CRC_OFFSET + 1] = (uint8_t)((crc >> 8) & 0xff);

	set_rdy((n > 0 || k_msgq_num_used_get(&ship_msgq) > 0) ? 1 : 0);
	return n;
}

static void can_rx_thread(void)
{
	struct can_frame rx;
	while (1) {
		if (k_msgq_get(&can_rx_msgq, &rx, K_FOREVER) == 0) {
			g_can_rx++;
			if (k_msgq_put(&ship_msgq, &rx, K_NO_WAIT) == 0) {
				set_rdy(1);
			}
		}
	}
}
K_THREAD_DEFINE(can_rx_tid, 1024, can_rx_thread, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	LOG_INF("bidirectional CAN-FD<->SPI bridge w/ RDY flow control");

	if (!device_is_ready(can_dev) || !device_is_ready(spi_dev) ||
	    !gpio_is_ready_dt(&rdy)) {
		g_state = 0xE0;
		return -1;
	}

	gpio_pin_configure_dt(&rdy, GPIO_OUTPUT_INACTIVE);
	set_rdy(0);

	if (can_set_mode(can_dev, CAN_MODE_FD | CAN_MODE_LOOPBACK) != 0) {
		g_state = 0xE1;
		return -1;
	}
	if (can_start(can_dev) != 0) {
		g_state = 0xE2;
		return -1;
	}
	const struct can_filter filter = { .flags = 0, .id = 0, .mask = 0 };
	if (can_add_rx_filter_msgq(can_dev, &can_rx_msgq, &filter) < 0) {
		g_state = 0xE3;
		return -1;
	}
	g_state = 3;

	const struct spi_buf tx = { .buf = spi_tx, .len = BLOCK_SIZE };
	const struct spi_buf rx = { .buf = spi_rx, .len = BLOCK_SIZE };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

	uint8_t seq = 0;
	pack_block(seq);

	while (1) {
		int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
		if (ret < 0) {
			g_state = 0xE4;
			k_sleep(K_MSEC(10));
			continue;
		}
		g_blocks_sent++;
		inject_block(spi_rx);
		seq++;
		pack_block(seq);
	}

	return 0;
}
