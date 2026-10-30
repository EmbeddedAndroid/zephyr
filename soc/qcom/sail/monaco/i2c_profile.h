/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_PROFILE_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_PROFILE_H_

#include "timer.h"

enum monaco_i2c_profile_board {
	MONACO_I2C_PROFILE_BOARD_UNKNOWN,
	MONACO_I2C_PROFILE_VENTUNO_Q,
};

enum monaco_i2c_profile_error {
	MONACO_I2C_PROFILE_INVALID = -2300,
	MONACO_I2C_PROFILE_UNQUALIFIED,
	MONACO_I2C_PROFILE_VERSION,
	MONACO_I2C_PROFILE_CAPABILITY,
	MONACO_I2C_PROFILE_BUSY,
	MONACO_I2C_PROFILE_FIFO,
	MONACO_I2C_PROFILE_IO,
	MONACO_I2C_PROFILE_READBACK,
};

enum monaco_i2c_profile_stage {
	MONACO_I2C_PROFILE_INPUT,
	MONACO_I2C_PROFILE_GUARDS,
	MONACO_I2C_PROFILE_WRAPPER,
	MONACO_I2C_PROFILE_METADATA,
	MONACO_I2C_PROFILE_CFG0,
	MONACO_I2C_PROFILE_CFG64,
	MONACO_I2C_PROFILE_VERIFY,
	MONACO_I2C_PROFILE_COMPLETE,
};

/* Caller assertions about board, clock and reset state; not read from hardware. */
struct monaco_i2c_profile_qualification {
	enum monaco_i2c_profile_board board;
	bool exclusive_ownership;
	bool clocks_qualified;
	bool reset_state_qualified;
	bool board_state_qualified;
};

struct monaco_i2c_profile_status {
	enum monaco_i2c_profile_stage stage;
	uint32_t address, value;
	uint32_t qup_version, hardware_parameters, protocol_revision;
	uint32_t tx_depth, rx_depth;
	unsigned int reads_attempted, reads_completed;
	unsigned int writes_attempted, writes_completed;
	int callback_result, result;
	bool value_valid, configuration_readback;
};

bool monaco_i2c_profile_write_allowed(uint32_t address);

/**
 * Program the fixed SE0 I2C minicore register profile; no sequencer code is
 * loaded. SE0 must be in its cold-reset state with wrapper clocks and a
 * 19.2 MHz SE clock running, and the caller needs exclusive use of SE0 and
 * the QUP wrapper. The QUP version, minicore capability, idle state and FIFO
 * depths are checked before any write. Each access is tried once; a failed
 * write may still have taken effect (see writes_attempted).
 *
 * @retval 0 profile written and read back.
 * @retval MONACO_I2C_PROFILE_UNQUALIFIED caller assertions not all set.
 * @retval negative MONACO_I2C_PROFILE_ error otherwise.
 */
int monaco_i2c_profile_init(const struct monaco_timer_io *io,
			    const struct monaco_i2c_profile_qualification *qualification,
			    struct monaco_i2c_profile_status *status);

#endif
