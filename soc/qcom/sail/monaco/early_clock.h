/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_EARLY_CLOCK_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_EARLY_CLOCK_H_

#include "clock.h"

enum monaco_early_clock_stage {
	MONACO_EARLY_CLOCK_INPUT,
	MONACO_EARLY_CLOCK_DOMAIN,
	MONACO_EARLY_CLOCK_BRANCH,
	MONACO_EARLY_CLOCK_CPU_REFRESH,
	MONACO_EARLY_CLOCK_COMPLETE,
};

struct monaco_early_clock_status {
	enum monaco_early_clock_stage stage;
	uint32_t address;
	unsigned int completed_branches;
};

/* Requires a running timer, XO and supplies already at operating voltage. */
int monaco_early_clocks_enable(struct monaco_clock *clock,
			       struct monaco_early_clock_status *status);

#endif
