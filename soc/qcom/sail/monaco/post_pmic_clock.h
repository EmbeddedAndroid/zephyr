/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_POST_PMIC_CLOCK_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_POST_PMIC_CLOCK_H_

#include "clock.h"
#include "pmic_boot.h"

enum monaco_post_pmic_clock_stage {
	MONACO_POST_PMIC_CLOCK_INPUT,
	MONACO_POST_PMIC_CLOCK_INHERITED,
	MONACO_POST_PMIC_CLOCK_CPU_SOURCE,
	MONACO_POST_PMIC_CLOCK_CPU_DOMAIN,
	MONACO_POST_PMIC_CLOCK_ATB,
	MONACO_POST_PMIC_CLOCK_ASILB,
	MONACO_POST_PMIC_CLOCK_DMA_SOURCE,
	MONACO_POST_PMIC_CLOCK_DMA_DOMAIN,
	MONACO_POST_PMIC_CLOCK_COMPLETE,
};

struct monaco_post_pmic_clock_status {
	enum monaco_post_pmic_clock_stage stage;
	unsigned int completed_domains;
	uint32_t address;
};

/**
 * Raise the CPU, ATB, ASILB and DMA clock rates once the PMIC supplies are up.
 * Call after the early clocks and a completed monaco_pmic_boot_finish(), with
 * the same nine regulator policies; every permitted required enable must be
 * on and every rail within its policy. The four starting muxes and PLL0 are
 * checked before any write. PLL0 stays on; PLL4 may be started for DMA.
 * Single use: a failure is not rolled back and status records the stage.
 *
 * @retval 0 on success, or a negative MONACO_CLOCK_ or MONACO_PMIC_ error.
 */
int monaco_post_pmic_clocks(struct monaco_clock *clock, const struct monaco_pmic *pmic,
			    const struct monaco_pmic_boot_result *boot,
			    const struct monaco_pmic_regulator_policy *policies,
			    size_t policy_count, struct monaco_post_pmic_clock_status *status);

#endif
