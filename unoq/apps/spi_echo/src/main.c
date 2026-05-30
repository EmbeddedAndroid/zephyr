/*
 * SPI-slave echo bring-up for the Arduino UNO Q (STM32U585).
 *
 * Purpose: de-risk STM32U5 SPI-slave mode before building the CAN<->SPI bridge.
 * The QCM2290 Linux side is the SPI master (/dev/spidev0.0, the
 * `arduino,unoq-mcu` node on GENI SE spi@4a94000). This MCU app is the SPI
 * slave on SPI3 (SCK=PG9, MISO=PG10, MOSI=PB5, NSS=PG12 -- hardware NSS).
 *
 * Echo semantics: SPI is master-clocked, so a slave cannot react within the
 * same transfer. We preload the TX buffer; on each transfer the master clocks
 * out whatever we preloaded (MISO) while clocking in new bytes (MOSI). After
 * each transfer we copy RX->TX so the *next* transfer echoes what we just
 * received. A Linux test does: xfer1 (reads our 0xA0.. banner), xfer2 (reads
 * back what xfer1 sent). That one-transfer delay proves the slave path.
 *
 * A green-LED heartbeat runs in its own thread so we can see firmware liveness
 * even if SPI wedges (the known-rough part of U5 slave mode).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(spi_echo, LOG_LEVEL_INF);

#define SPI_NODE DT_NODELABEL(spi3)
#define BUF_LEN  32

/* led0 = led3_green (PH11) per the board dts. Heartbeat only. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct device *const spi_dev = DEVICE_DT_GET(SPI_NODE);

/* Slave config: clock polarity/phase must match the master (mode 0 here).
 * Frequency is ignored in slave mode (the master supplies SCK). 8-bit MSB.
 */
static const struct spi_config spi_cfg = {
	.frequency = 1000000U,
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

static uint8_t tx_buf[BUF_LEN];
static uint8_t rx_buf[BUF_LEN];

static void heartbeat_thread(void)
{
	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("led not ready; no heartbeat");
		return;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	while (1) {
		gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(500));
	}
}
K_THREAD_DEFINE(hb_tid, 512, heartbeat_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	LOG_INF("SPI-slave echo starting on %s", spi_dev->name);

	if (!device_is_ready(spi_dev)) {
		LOG_ERR("SPI device %s not ready", spi_dev->name);
		return -1;
	}

	/* Preload a recognizable banner so the master's first read isn't junk. */
	for (int i = 0; i < BUF_LEN; i++) {
		tx_buf[i] = 0xA0 + i;
	}

	const struct spi_buf tx = { .buf = tx_buf, .len = BUF_LEN };
	const struct spi_buf rx = { .buf = rx_buf, .len = BUF_LEN };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

	uint32_t xfer = 0;

	while (1) {
		/* Blocks until the master clocks a transfer against us. */
		int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
		if (ret < 0) {
			LOG_ERR("spi_transceive failed: %d", ret);
			k_sleep(K_MSEC(100));
			continue;
		}

		LOG_INF("xfer %u: rx[0..7] = %02x %02x %02x %02x %02x %02x %02x %02x",
			xfer++, rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3],
			rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);

		/* Echo: next transfer sends back what we just received. */
		memcpy(tx_buf, rx_buf, BUF_LEN);
	}

	return 0;
}
