/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_SHUTDOWN_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_SHUTDOWN_H_
#include "pmic_boot.h"
#include "pmic_policy.h"

enum monaco_pmic_shutdown_stage {
	MONACO_PMIC_SHUTDOWN_FRESH,
	MONACO_PMIC_SHUTDOWN_VALIDATE,
	MONACO_PMIC_SHUTDOWN_READ_FORCE,
	MONACO_PMIC_SHUTDOWN_READ_SLAVE2,
	MONACO_PMIC_SHUTDOWN_WRITE_SLAVE2,
	MONACO_PMIC_SHUTDOWN_READ_ENABLE,
	MONACO_PMIC_SHUTDOWN_WRITE_ENABLE,
	MONACO_PMIC_SHUTDOWN_READ_FEEDBACK,
	MONACO_PMIC_SHUTDOWN_WRITE_C9,
	MONACO_PMIC_SHUTDOWN_WRITE_CB,
	MONACO_PMIC_SHUTDOWN_WRITE_CC,
	MONACO_PMIC_SHUTDOWN_READ_C9,
	MONACO_PMIC_SHUTDOWN_READ_CC,
	MONACO_PMIC_SHUTDOWN_VERIFIED,
};
struct monaco_pmic_shutdown_result {
	enum monaco_pmic_shutdown_stage stage;
	int status;
	int guard_status;
	uint32_t guards_attempted, guards_completed;
	uint32_t reads_attempted, reads_completed, writes_attempted, writes_completed;
	unsigned int last_slave;
	uint8_t last_register;
	uint8_t force, slave2_before, enable_before, feedback, observed_c9, observed_cc;
	/* Bit n is set once the nth byte field above has been read. */
	uint8_t valid;
};

/**
 * @brief Lower EN_DRV and program the power-off policy (C9=00, CB=0a, CC=00).
 *
 * Read-modify-write steps preserve unrelated bits. EN_DRV feedback and C9/CC
 * are read back. @p check runs before each transfer and before success.
 * @p result must be zeroed and is single use. The first error stops the
 * sequence without rollback; earlier writes may already have taken effect.
 *
 * @retval 0 EN_DRV low and C9/CC verified.
 * @retval MONACO_PMIC_STATE Result already used or PMIC state not ready.
 * @retval MONACO_PMIC_RESET_VERIFY Force bit set or a readback mismatch.
 * @return Other negative values are transfer or guard errors.
 */
int monaco_pmic_shutdown_configure_guarded(struct monaco_pmic *pmic,
					   const struct monaco_pmic_policy_snapshot *snapshot,
					   const struct monaco_pmic_boot_result *boot,
					   struct monaco_pmic_shutdown_result *result,
					   int (*check)(void *context), void *context);
#endif
