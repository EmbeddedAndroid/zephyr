/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_CLOCK_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_CLOCK_H_

#include "timer.h"

enum monaco_clock_error {
	MONACO_CLOCK_INVALID = -1,
	MONACO_CLOCK_IO = -2,
	MONACO_CLOCK_POLL_LIMIT = -3,
	MONACO_CLOCK_TIMEBASE = -4,
	MONACO_CLOCK_STATE = -5,
};

struct monaco_clock {
	struct monaco_timer_io io;
	struct monaco_timer *timer;
	uint32_t max_poll_reads;
};

bool monaco_clock_write_allowed(uint32_t address);
/* The caller must enable the parent clock before using these two helpers. */
int monaco_clock_cbcr_enable(struct monaco_clock *clock, uint32_t address);
int monaco_clock_rcg_update(struct monaco_clock *clock, uint32_t command, uint8_t source,
			    uint8_t divider_encoding);
int monaco_clock_pll5_enable(struct monaco_clock *clock);
int monaco_clock_pll6_enable(struct monaco_clock *clock);
/* Switches DEMET to PLL5; an existing XO parent is not detected. */
int monaco_clock_demet_enable(struct monaco_clock *clock);
/* Runs SE0 at 19.2 MHz from XO; wrapper clocks must be on. Fails if DFS is enabled. */
int monaco_clock_se0_enable(struct monaco_clock *clock);

#endif
