/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_BOOT_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_I2C_BOOT_H_

#include "clock.h"
#include "i2c.h"
#include "i2c_profile.h"

enum monaco_i2c_boot_error {
	MONACO_I2C_BOOT_INVALID = -2400,
	MONACO_I2C_BOOT_UNQUALIFIED,
};

enum monaco_i2c_boot_stage {
	MONACO_I2C_BOOT_INPUT,
	MONACO_I2C_BOOT_CLOCK,
	MONACO_I2C_BOOT_PROFILE,
	MONACO_I2C_BOOT_FIFO,
	MONACO_I2C_BOOT_COMPLETE,
};

/* Caller assertions about board, clock and reset state; not read from hardware. */
struct monaco_i2c_boot_qualification {
	enum monaco_i2c_profile_board board;
	bool exclusive_ownership;
	bool parent_clocks_qualified;
	bool reset_state_qualified;
	bool board_state_qualified;
};

struct monaco_i2c_boot_status {
	enum monaco_i2c_boot_stage stage;
	unsigned int completed_stages;
	int result;
	struct monaco_i2c_profile_status profile;
	struct monaco_i2c_diagnostic fifo;
};

/**
 * Bring up the SE0 I2C controller from cold: SE0 clock from XO, minicore
 * profile, then FIFO mode. The caller needs exclusive use of SE0, its clock
 * controls, the QUP wrapper and GPIO0/1, with XO and the wrapper clocks
 * running. clock->timer and the callback context must outlive i2c. i2c is
 * cleared on entry and is unusable after a failure; status records the
 * failing stage. Earlier writes are not undone.
 *
 * @retval 0 on success.
 * @retval MONACO_I2C_BOOT_INVALID or MONACO_I2C_BOOT_UNQUALIFIED bad input.
 * @retval negative clock, profile or MONACO_I2C_ error from the failing stage.
 */
int monaco_i2c_boot_init(struct monaco_i2c *i2c, struct monaco_clock *clock,
			 const struct monaco_i2c_boot_qualification *qualification,
			 struct monaco_i2c_boot_status *status);

#endif
