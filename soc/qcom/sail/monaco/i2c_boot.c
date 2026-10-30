/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c_boot.h"

int monaco_i2c_boot_init(struct monaco_i2c *i2c, struct monaco_clock *clock,
			 const struct monaco_i2c_boot_qualification *qualification,
			 struct monaco_i2c_boot_status *status)
{
	if (i2c) {
		*i2c = (struct monaco_i2c){0};
	}
	if (!status) {
		return MONACO_I2C_BOOT_INVALID;
	}
	*status = (struct monaco_i2c_boot_status){
		.stage = MONACO_I2C_BOOT_INPUT,
		.result = MONACO_I2C_BOOT_INVALID,
	};
	if (!i2c || !clock || !clock->io.read32 || !clock->io.write32 || !clock->timer ||
	    !clock->timer->initialized || !clock->timer->io.read32 || !clock->timer->io.write32 ||
	    !clock->timer->frequency || !clock->timer->read_budget || !clock->max_poll_reads ||
	    !qualification) {
		return status->result;
	}
	if (qualification->board != MONACO_I2C_PROFILE_VENTUNO_Q ||
	    !qualification->exclusive_ownership || !qualification->parent_clocks_qualified ||
	    !qualification->reset_state_qualified || !qualification->board_state_qualified) {
		status->result = MONACO_I2C_BOOT_UNQUALIFIED;
		return status->result;
	}

	status->stage = MONACO_I2C_BOOT_CLOCK;
	status->result = monaco_clock_se0_enable(clock);
	if (status->result) {
		return status->result;
	}
	status->completed_stages = 1;

	const struct monaco_i2c_profile_qualification profile = {
		.board = qualification->board,
		.exclusive_ownership = qualification->exclusive_ownership,
		/* The SE0 clock was set up above, on top of the asserted parent clocks. */
		.clocks_qualified = qualification->parent_clocks_qualified,
		.reset_state_qualified = qualification->reset_state_qualified,
		.board_state_qualified = qualification->board_state_qualified,
	};
	status->stage = MONACO_I2C_BOOT_PROFILE;
	status->result = monaco_i2c_profile_init(&clock->io, &profile, &status->profile);
	if (status->result) {
		return status->result;
	}
	status->completed_stages = 2;

	status->stage = MONACO_I2C_BOOT_FIFO;
	status->result = monaco_i2c_init(i2c, &clock->io, clock->timer, clock->max_poll_reads);
	status->fifo = i2c->diagnostic;
	if (status->result) {
		i2c->initialized = false;
		return status->result;
	}
	status->completed_stages = 3;
	status->stage = MONACO_I2C_BOOT_COMPLETE;
	return 0;
}
