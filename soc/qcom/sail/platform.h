/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_PLATFORM_H_
#define ZEPHYR_SOC_QCOM_SAIL_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>
#include "pmic.h"

/*
 * A board the image supports, from a devicetree qcom,sail-platform node.
 * The running chip selects the entry; at most one entry per chip.
 */
struct sail_platform {
	uint16_t chip_id;
	/* SAIL I2C PMIC rails in boot order; NULL when SAIL has no PMIC of its own. */
	const struct monaco_pmic_regulator_policy *pmic_regulators;
	size_t pmic_regulator_count;
};

struct sail_platforms {
	const struct sail_platform *entries;
	size_t count;
};

/* The platforms described in the devicetree. */
extern const struct sail_platforms sail_platforms;

/* Return the only entry for chip_id; NULL if there is none or more than one. */
const struct sail_platform *sail_platform_find(const struct sail_platforms *platforms,
					       uint16_t chip_id);

#endif
