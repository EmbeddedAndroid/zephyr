/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_H_

#include "timer.h"
#include <stddef.h>

enum monaco_i2c_error {
	MONACO_I2C_INVALID = -2000,
	MONACO_I2C_IO,
	MONACO_I2C_STATE,
	MONACO_I2C_FIRMWARE,
	MONACO_I2C_FIFO,
	MONACO_I2C_TIMEBASE,
	MONACO_I2C_TIMEOUT,
	MONACO_I2C_NACK,
	MONACO_I2C_PROTOCOL,
	MONACO_I2C_ARBITRATION,
	MONACO_I2C_COMMAND,
	MONACO_I2C_SHORT,
};

struct monaco_i2c_diagnostic {
	uint32_t address, value, polls;
	size_t transmitted, received;
	int result, cleanup_result;
};

struct monaco_i2c {
	struct monaco_timer_io io;
	struct monaco_timer *timer;
	uint32_t max_poll_reads, tx_depth, rx_depth;
	bool initialized, busy;
	struct monaco_i2c_diagnostic diagnostic;
};

bool monaco_i2c_write_allowed(uint32_t address);
/**
 * Put the SE0 I2C controller in FIFO mode.
 * SE0 must already run at 19.2 MHz with its wrapper clocks on and the I2C
 * sequencer firmware loaded; the protocol ID and FIFO depths are checked.
 * The caller needs exclusive use of the controller and GPIO0/1. After a
 * failure the context is unusable until initialized again.
 *
 * @retval 0 on success, or a negative MONACO_I2C_ error.
 */
int monaco_i2c_init(struct monaco_i2c *i2c, const struct monaco_timer_io *io,
		    struct monaco_timer *timer, uint32_t max_poll_reads);
/**
 * Write 1..4096 bytes, optionally followed by a read of up to 4096 bytes.
 * Pure reads are not supported. timeout_us must be at least 4; a quarter of
 * it is reserved for cancel and abort on failure. Callers must serialize
 * access; the busy flag only catches reentry. On error, RX data is invalid
 * and the cleanup result is kept in the diagnostic; if cleanup fails the
 * context must be initialized again.
 *
 * @retval 0 on success, or a negative MONACO_I2C_ error.
 */
int monaco_i2c_transfer(void *context, uint8_t address7, const uint8_t *tx, size_t tx_size,
			uint8_t *rx, size_t rx_size, uint32_t timeout_us);
/* Address-only transfer; returns 0 if the target ACKs. */
int monaco_i2c_probe(struct monaco_i2c *i2c, uint8_t address7, uint32_t timeout_us);

#endif
