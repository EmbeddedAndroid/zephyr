/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_TIMER_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

enum monaco_timer_error {
	MONACO_TIMER_IO = -1,
	MONACO_TIMER_INVALID = -2,
	MONACO_TIMER_INCOHERENT = -3,
	MONACO_TIMER_STOPPED = -4,
	MONACO_TIMER_BACKWARD = -5,
	MONACO_TIMER_RANGE = -6,
	MONACO_TIMER_BUDGET = -7,
};

struct monaco_timer_io {
	void *context;
	/* Ordered Device accesses; callbacks must themselves terminate. */
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	int (*write32)(void *context, uint32_t address, uint32_t value);
};

struct monaco_timer {
	struct monaco_timer_io io;
	uint64_t origin;
	uint64_t last;
	uint32_t frequency;
	uint32_t read_budget;
	bool initialized;
};

/* Checks that the counter advances; does not calibrate it against another clock. */
int monaco_timer_init(struct monaco_timer *timer, const struct monaco_timer_io *io,
		      uint32_t read_budget);
int monaco_timer_time_us(void *context, uint64_t *value);
int monaco_timer_delay_us(void *context, uint32_t delay);

#endif
