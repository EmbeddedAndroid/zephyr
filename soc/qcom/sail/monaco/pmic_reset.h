/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_RESET_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_RESET_H_

#include "pmic_policy.h"

enum monaco_pmic_reset_error {
	MONACO_PMIC_RESET_VERIFY = -1300,
};

enum monaco_pmic_reset_stage {
	MONACO_PMIC_RESET_FRESH,
	MONACO_PMIC_RESET_VALIDATE,
	MONACO_PMIC_RESET_WRITE_C9,
	MONACO_PMIC_RESET_WRITE_CB,
	MONACO_PMIC_RESET_WRITE_CC,
	MONACO_PMIC_RESET_READ_C9,
	MONACO_PMIC_RESET_READ_CC,
	MONACO_PMIC_RESET_COMPARE,
	MONACO_PMIC_RESET_VERIFIED,
};

struct monaco_pmic_reset_result {
	enum monaco_pmic_reset_stage stage;
	int status;
	uint8_t last_register;
	uint8_t writes_attempted, writes_completed;
	uint8_t reads_attempted, reads_completed;
	uint8_t observed_c9, observed_cc;
	bool c9_valid, cc_valid;
	uint8_t guards_attempted, guards_completed;
	int guard_status;
};

/*
 * Program the PMIC (device 0x82) PS_HOLD reset policy to power off and on
 * again. Writes C9=00, CB=0a, CC=01 at address 0x28 with CRC, then reads
 * back C9 and CC; CB is not read back. Requires a successful policy sample
 * and boot result for this PMIC from the current boot, and a reset FSM low
 * nibble below 12. @p result must be zeroed and is single use. The first
 * transfer error stops the sequence without retry; earlier writes may
 * already have taken effect.
 *
 * @retval 0 Readback matched.
 * @retval MONACO_PMIC_STATE Result already used or PMIC state not ready.
 * @retval MONACO_PMIC_RESET_VERIFY Readback mismatch.
 * @return Other negative values are transfer errors.
 */
int monaco_pmic_reset_configure_readback(struct monaco_pmic *pmic,
					 const struct monaco_pmic_policy_snapshot *snapshot,
					 const struct monaco_pmic_boot_result *boot,
					 struct monaco_pmic_reset_result *result);

/*
 * As monaco_pmic_reset_configure_readback(), with @p guard checked before each
 * transfer and before success. A negative guard result is returned as is; a
 * positive one becomes MONACO_PMIC_CALLBACK. @p guard may be NULL.
 */
int monaco_pmic_reset_configure_readback_guarded(struct monaco_pmic *pmic,
						 const struct monaco_pmic_policy_snapshot *snapshot,
						 const struct monaco_pmic_boot_result *boot,
						 struct monaco_pmic_reset_result *result,
						 int (*guard)(void *context), void *guard_context);

#endif
