/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_LEMANS_PREPARE_H_
#define ZEPHYR_SOC_QCOM_SAIL_LEMANS_PREPARE_H_

#include <stdbool.h>
#include <stdint.h>
#include "timer.h"

enum lemans_prepare_error {
	LEMANS_PREPARE_INVALID = -2700,
	LEMANS_PREPARE_IO,
	LEMANS_PREPARE_IDENTITY,
	LEMANS_PREPARE_PAD,
};

/*
 * Check that the SoC is Lemans, then configure the SAIL pads the main
 * domain waits for: PSAIL_ERR_N released to a pulled-up GPIO input and
 * the SAIL sleep-enable output routed out. Returns 0 or a
 * lemans_prepare_error.
 */
int lemans_prepare_run(const struct monaco_timer_io *io);

/* True only for the pad configuration registers lemans_prepare_run() writes. */
bool lemans_prepare_write_allowed(uint32_t address);

#endif
