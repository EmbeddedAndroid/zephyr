/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c.h"

#include <zephyr/sys/util.h>

#define SE              UINT32_C(0xf1980000)
#define STATUS          (SE + 0x40)
#define FW_REVISION     (SE + 0x68)
#define COMMAND         (SE + 0x600)
#define COMMAND_CONTROL (SE + 0x604)
#define IRQ_STATUS      (SE + 0x610)
#define IRQ_CLEAR       (SE + 0x618)
#define TX_LENGTH       (SE + 0x26c)
#define RX_LENGTH       (SE + 0x270)
#define TX_FIFO         (SE + 0x700)
#define RX_FIFO         (SE + 0x780)
#define TX_STATUS       (SE + 0x800)
#define RX_STATUS       (SE + 0x804)
#define DONE            UINT32_C(1)

bool monaco_i2c_write_allowed(uint32_t address)
{
	switch (address) {
	case 0xf0100000:
	case 0xf0101000:
	case SE + 0x20:
	case SE + 0x24:
	case SE + 0x28:
	case SE + 0x48:
	case SE + 0x7c:
	case SE + 0x80:
	case SE + 0x254:
	case SE + 0x258:
	case SE + 0x260:
	case SE + 0x264:
	case TX_LENGTH:
	case RX_LENGTH:
	case SE + 0x278:
	case SE + 0x284:
	case SE + 0x288:
	case SE + 0x814:
	case COMMAND:
	case COMMAND_CONTROL:
	case IRQ_CLEAR:
	case TX_FIFO:
	case SE + 0x2000:
	case SE + 0x2004:
	case SE + 0x2008:
		return true;
	default:
		return false;
	}
}

static int read32(struct monaco_i2c *i2c, uint32_t address, uint32_t *value)
{
	i2c->diagnostic.address = address;
	if (i2c->io.read32(i2c->io.context, address, value)) {
		return MONACO_I2C_IO;
	}
	i2c->diagnostic.value = *value;
	return 0;
}

static int write32(struct monaco_i2c *i2c, uint32_t address, uint32_t value)
{
	i2c->diagnostic.address = address;
	i2c->diagnostic.value = value;
	if (!monaco_i2c_write_allowed(address)) {
		return MONACO_I2C_INVALID;
	}
	return i2c->io.write32(i2c->io.context, address, value) ? MONACO_I2C_IO : 0;
}

static int update(struct monaco_i2c *i2c, uint32_t address, uint32_t keep, uint32_t value)
{
	int rc;
	uint32_t old;

	rc = read32(i2c, address, &old);
	if (rc != 0) {
		return rc;
	}
	return write32(i2c, address, (old & keep) | value);
}

static int time_us(struct monaco_i2c *i2c, uint64_t *now)
{
	return monaco_timer_time_us(i2c->timer, now) ? MONACO_I2C_TIMEBASE : 0;
}

static int idle(struct monaco_i2c *i2c)
{
	int rc;
	uint32_t value;

	rc = read32(i2c, STATUS, &value);
	if (rc != 0) {
		return rc;
	}
	if (value & 0x1001U) {
		return MONACO_I2C_STATE;
	}
	rc = read32(i2c, TX_STATUS, &value);
	if (rc != 0) {
		return rc;
	}
	if (value & 0x0fffffffU) {
		return MONACO_I2C_FIFO;
	}
	rc = read32(i2c, RX_STATUS, &value);
	if (rc != 0) {
		return rc;
	}
	return value & 0x01ffffffU ? MONACO_I2C_FIFO : 0;
}

static int configure(struct monaco_i2c *i2c)
{
	int rc;
	uint32_t protocol, tx, rx, value;
	const uint32_t pins[] = {0xf0100000, 0xf0101000};

	rc = read32(i2c, FW_REVISION, &protocol);
	if (rc != 0) {
		return rc;
	}
	if (((protocol >> 8) & 0xffU) != 3) {
		return MONACO_I2C_FIRMWARE;
	}
	rc = idle(i2c);
	if (rc != 0) {
		return rc;
	}
	rc = read32(i2c, SE + 0xe24, &tx);
	if (rc != 0) {
		return rc;
	}
	rc = read32(i2c, SE + 0xe28, &rx);
	if (rc != 0) {
		return rc;
	}
	i2c->tx_depth = (tx >> 16) & 0x3fU;
	i2c->rx_depth = (rx >> 16) & 0x3fU;
	if (((tx >> 24) & 0x3fU) != 32 || ((rx >> 24) & 0x3fU) != 32 || i2c->tx_depth == 0 ||
	    i2c->rx_depth < 3) {
		return MONACO_I2C_FIFO;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(pins); i++) {
		rc = write32(i2c, pins[i], 0x207);
		if (rc != 0) {
			return rc;
		}
		rc = read32(i2c, pins[i], &value);
		if (rc != 0) {
			return rc;
		}
		if (value != 0x207) {
			return MONACO_I2C_STATE;
		}
	}
	rc = write32(i2c, SE + 0x24, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(i2c, SE + 0x80, 0x700, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x28, 0x7f);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x24, 0x7f);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x258, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x814, i2c->rx_depth - 2);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x20, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(i2c, SE + 0x28, 0x17f, 0x200);
	if (rc != 0) {
		return rc;
	}
	rc = update(i2c, SE + 0x28, 0x27f, 0x100);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x2000, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(i2c, SE + 0x28, 0x17f, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(i2c, SE + 0x28, 0x27f, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x2004, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x2008, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x7c, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x254, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x260, 0x0007f8fe);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x264, 0x000ffefe);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x284, 0x0007f8fe);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x288, 0x000ffefe);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x48, 0x11);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, SE + 0x278, 0x00302412);
	if (rc != 0) {
		return rc;
	}
	return write32(i2c, IRQ_CLEAR, UINT32_MAX);
}

int monaco_i2c_init(struct monaco_i2c *i2c, const struct monaco_timer_io *io,
		    struct monaco_timer *timer, uint32_t max_poll_reads)
{
	if (!i2c) {
		return MONACO_I2C_INVALID;
	}
	if (!io || !io->read32 || !io->write32 || !timer || !timer->initialized ||
	    !max_poll_reads) {
		*i2c = (struct monaco_i2c){0};
		return MONACO_I2C_INVALID;
	}
	*i2c = (struct monaco_i2c){
		.io = *io,
		.timer = timer,
		.max_poll_reads = max_poll_reads,
	};
	int rc = configure(i2c);

	i2c->diagnostic.result = rc;
	i2c->initialized = rc == 0;
	return rc;
}

static int irq_error(uint32_t irq)
{
	if (irq & 0x2000U) {
		return MONACO_I2C_ARBITRATION;
	}
	if (irq & 0x400U) {
		return MONACO_I2C_NACK;
	}
	if (irq & 0x1000U) {
		return MONACO_I2C_PROTOCOL;
	}
	if (irq & 0x33000000U) {
		return MONACO_I2C_FIFO;
	}
	return irq & 0x00004a3eU ? MONACO_I2C_COMMAND : 0;
}

static int fill_tx(struct monaco_i2c *i2c, const uint8_t *tx, size_t size)
{
	int rc;
	uint32_t level;

	rc = read32(i2c, TX_STATUS, &level);
	if (rc != 0) {
		return rc;
	}
	level &= 0x0fffffffU;
	if (level > i2c->tx_depth) {
		return MONACO_I2C_FIFO;
	}
	for (uint32_t word = level; word < i2c->tx_depth && i2c->diagnostic.transmitted < size;
	     word++) {
		uint32_t value = 0;
		size_t pos = i2c->diagnostic.transmitted;
		size_t bytes = size - pos < 4 ? size - pos : 4;

		for (size_t byte = 0; byte < bytes; byte++) {
			value |= (uint32_t)tx[pos + byte] << (byte * 8);
		}
		rc = write32(i2c, TX_FIFO, value);
		if (rc != 0) {
			return rc;
		}
		i2c->diagnostic.transmitted += bytes;
	}
	return 0;
}

static int drain_rx(struct monaco_i2c *i2c, uint8_t *rx, size_t size)
{
	int rc;
	uint32_t state, words, last_bytes = 4;

	rc = read32(i2c, RX_STATUS, &state);
	if (rc != 0) {
		return rc;
	}
	words = state & 0x01ffffffU;
	if (words > i2c->rx_depth) {
		return MONACO_I2C_FIFO;
	}
	if (state & 0x80000000U) {
		last_bytes = (state >> 28) & 7U;
		if (!words || !last_bytes || last_bytes > 4) {
			return MONACO_I2C_FIFO;
		}
	}
	for (uint32_t word = 0; word < words; word++) {
		uint32_t value, bytes = word + 1 == words ? last_bytes : 4;

		if (size - i2c->diagnostic.received < bytes) {
			return MONACO_I2C_FIFO;
		}
		rc = read32(i2c, RX_FIFO, &value);
		if (rc != 0) {
			return rc;
		}
		for (uint32_t byte = 0; byte < bytes; byte++) {
			rx[i2c->diagnostic.received++] = (uint8_t)value;
			value >>= 8;
		}
	}
	return 0;
}

static int wait_recovery(struct monaco_i2c *i2c, uint64_t deadline, uint32_t bit)
{
	int rc;

	for (uint32_t poll = 0; poll < i2c->max_poll_reads; poll++) {
		uint64_t now;
		uint32_t irq;

		rc = time_us(i2c, &now);
		if (rc != 0) {
			return rc;
		}
		if (now >= deadline) {
			return MONACO_I2C_TIMEOUT;
		}
		rc = read32(i2c, IRQ_STATUS, &irq);
		if (rc != 0) {
			return rc;
		}
		if (irq & bit) {
			rc = write32(i2c, IRQ_CLEAR, irq);
			if (rc != 0) {
				return rc;
			}
			return idle(i2c);
		}
	}
	return MONACO_I2C_TIMEOUT;
}

static int recover(struct monaco_i2c *i2c, uint64_t deadline)
{
	int rc;
	uint64_t now;
	uint32_t irq;

	rc = read32(i2c, IRQ_STATUS, &irq);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, IRQ_CLEAR, irq);
	if (rc != 0) {
		return rc;
	}
	int idle_result = idle(i2c);

	if (!idle_result) {
		return 0;
	}
	if (idle_result == MONACO_I2C_IO) {
		return idle_result;
	}
	rc = time_us(i2c, &now);
	if (rc != 0) {
		return rc;
	}
	if (now >= deadline) {
		return MONACO_I2C_TIMEOUT;
	}
	rc = write32(i2c, COMMAND_CONTROL, 4);
	if (rc != 0) {
		return rc;
	}
	rc = wait_recovery(i2c, now + (deadline - now) / 2, 0x10);

	if (rc == 0) {
		return 0;
	}
	if (rc == MONACO_I2C_IO || rc == MONACO_I2C_TIMEBASE) {
		return rc;
	}
	rc = time_us(i2c, &now);
	if (rc != 0) {
		return rc;
	}
	if (now >= deadline) {
		return MONACO_I2C_TIMEOUT;
	}
	rc = write32(i2c, COMMAND_CONTROL, 2);
	if (rc != 0) {
		return rc;
	}
	return wait_recovery(i2c, deadline, 0x20);
}

static int run(struct monaco_i2c *i2c, uint8_t address7, const uint8_t *tx, size_t tx_size,
	       uint8_t *rx, size_t rx_size, uint32_t timeout_us)
{
	uint64_t start, now, deadline, transfer_deadline;
	uint32_t irq, opcode = tx_size ? (rx_size ? 3U : 1U) : 4U;
	int rc;

	rc = time_us(i2c, &start);
	if (rc != 0) {
		return rc;
	}
	if (UINT64_MAX - start < timeout_us) {
		return MONACO_I2C_TIMEBASE;
	}
	deadline = start + timeout_us;
	transfer_deadline = deadline - timeout_us / 4U;
	rc = idle(i2c);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, IRQ_CLEAR, UINT32_MAX);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, TX_LENGTH, (uint32_t)tx_size);
	if (rc != 0) {
		return rc;
	}
	rc = write32(i2c, RX_LENGTH, (uint32_t)rx_size);
	if (rc != 0) {
		return rc;
	}
	rc = time_us(i2c, &now);
	if (rc != 0) {
		return rc;
	}
	if (now >= transfer_deadline) {
		return MONACO_I2C_TIMEOUT;
	}
	/* Status was cleared above, so a completion raised right after the command is kept. */
	rc = write32(i2c, COMMAND, (opcode << 27) | ((uint32_t)address7 << 9));
	if (rc != 0) {
		goto failed;
	}
	for (uint32_t poll = 0; poll < i2c->max_poll_reads; poll++) {
		i2c->diagnostic.polls++;
		rc = time_us(i2c, &now);
		if (rc != 0) {
			goto failed;
		}
		if (now >= transfer_deadline) {
			rc = MONACO_I2C_TIMEOUT;
			goto failed;
		}
		rc = read32(i2c, IRQ_STATUS, &irq);
		if (rc != 0) {
			goto failed;
		}
		rc = irq_error(irq);
		if (rc != 0) {
			goto failed;
		}
		if (!(irq & DONE) && i2c->diagnostic.transmitted < tx_size) {
			rc = fill_tx(i2c, tx, tx_size);
			if (rc != 0) {
				goto failed;
			}
		}
		rc = drain_rx(i2c, rx, rx_size);
		if (rc != 0) {
			goto failed;
		}
		if (irq & DONE) {
			if (i2c->diagnostic.transmitted != tx_size ||
			    i2c->diagnostic.received != rx_size) {
				rc = MONACO_I2C_SHORT;
				goto failed;
			}
			rc = write32(i2c, IRQ_CLEAR, irq);
			if (rc == 0) {
				rc = idle(i2c);
			}
			if (rc != 0) {
				goto failed;
			}
			return 0;
		}
		rc = write32(i2c, IRQ_CLEAR, irq);
		if (rc != 0) {
			goto failed;
		}
	}
	rc = MONACO_I2C_TIMEOUT;
failed:
	{
		uint32_t failed_address = i2c->diagnostic.address;
		uint32_t failed_value = i2c->diagnostic.value;

		i2c->diagnostic.cleanup_result = recover(i2c, deadline);
		i2c->diagnostic.address = failed_address;
		i2c->diagnostic.value = failed_value;
		if (i2c->diagnostic.cleanup_result) {
			i2c->initialized = false;
		}
		return rc;
	}
}

static int execute(struct monaco_i2c *i2c, uint8_t address7, const uint8_t *tx, size_t tx_size,
		   uint8_t *rx, size_t rx_size, uint32_t timeout_us)
{
	if (!i2c || address7 > 0x7f || timeout_us < 4 || tx_size > 4096 || rx_size > 4096 ||
	    (tx_size && !tx) || (rx_size && !rx) || (rx_size && !tx_size)) {
		return MONACO_I2C_INVALID;
	}
	if (!i2c->initialized || i2c->busy) {
		return MONACO_I2C_STATE;
	}
	i2c->diagnostic = (struct monaco_i2c_diagnostic){0};
	i2c->busy = true;
	int rc = run(i2c, address7, tx, tx_size, rx, rx_size, timeout_us);

	i2c->busy = false;
	i2c->diagnostic.result = rc;
	return rc;
}

int monaco_i2c_transfer(void *context, uint8_t address7, const uint8_t *tx, size_t tx_size,
			uint8_t *rx, size_t rx_size, uint32_t timeout_us)
{
	if (!tx_size) {
		return MONACO_I2C_INVALID;
	}
	return execute(context, address7, tx, tx_size, rx, rx_size, timeout_us);
}

int monaco_i2c_probe(struct monaco_i2c *i2c, uint8_t address7, uint32_t timeout_us)
{
	return execute(i2c, address7, NULL, 0, NULL, 0, timeout_us);
}
