/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_TRANSACTION_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_TRANSACTION_H_

#include "cold_boot.h"
#include "pmic_reset.h"
#include "pmic_shutdown.h"

enum monaco_pmic_transaction_phase {
	MONACO_PMIC_TRANSACTION_NEW,
	MONACO_PMIC_TRANSACTION_CLAIMED,
	MONACO_PMIC_TRANSACTION_CONFIGURING,
	MONACO_PMIC_TRANSACTION_CONFIGURED,
	MONACO_PMIC_TRANSACTION_DELAYING,
	MONACO_PMIC_TRANSACTION_DELAYED,
	MONACO_PMIC_TRANSACTION_SHUTDOWN_VERIFIED,
};

/** Last read of the crash-debug control at 0xf1149000. */
struct monaco_pmic_shutdown_control_result {
	uint32_t reads_attempted;
	uint32_t reads_completed;
	uint32_t value;
	int read_status;
	bool valid;
};

struct monaco_pmic_transaction_result {
	enum monaco_pmic_transaction_phase phase;
	int error;
	int guard_error;
	int time_error;
	uint32_t guards;
	uint32_t time_reads;
	uint64_t before_us;
	uint64_t after_us;
	struct monaco_pmic_reset_result pmic;
	struct monaco_pmic_shutdown_result shutdown;
	struct monaco_pmic_shutdown_control_result shutdown_control;
};

struct monaco_pmic_transaction {
	bool claimed;
	struct monaco_cold_boot *boot;
	struct monaco_pmic_transaction_result result;
};

/**
 * @brief Program the PMIC reset policy, read it back, then wait 50 ms.
 *
 * The PMIC needs 50 ms after the policy write before the reset is answered.
 * @p transaction must be zeroed and is single use; failure is terminal and
 * earlier PMIC writes are not undone. @p eligible must be side-effect free.
 * The PMIC binding, @p eligible, @p guard and the reset FSM (low nibble below
 * 12) are checked before every transfer and after the delay. Call from a
 * thread with interrupts enabled.
 *
 * @param transaction Single-use attempt and result storage.
 * @param boot The cold boot state of this boot, not a copy.
 * @param eligible Required platform eligibility check.
 * @param guard Required liveness check.
 * @param context Opaque callback context; may be NULL.
 * @retval 0 Policy verified and 50 ms elapsed.
 * @retval -EINVAL Required storage or callback is missing.
 * @retval -EALREADY The transaction has already been used.
 * @retval -EPERM Platform state is not eligible.
 * @retval -ESTALE PMIC binding, eligibility or reset FSM changed.
 * @retval -ETIME The measured interval was shorter than 50 ms.
 * @retval -EIO The guard returned a positive error.
 * @return A negative PMIC, timer or guard error is propagated unchanged.
 */
int monaco_pmic_transaction_run(struct monaco_pmic_transaction *transaction,
				struct monaco_cold_boot *boot,
				bool (*eligible)(void *context,
						 const struct monaco_cold_boot *boot),
				int (*guard)(void *context), void *context);

/**
 * @brief Program the PMIC for graceful shutdown and verify it.
 *
 * Uses the same single-use @p transaction and callback rules as
 * monaco_pmic_transaction_run(). Lowers EN_DRV, sets C9=00, CB=0a, CC=00
 * (power off and stay off) and verifies C9/CC; there is no delay. The
 * crash-debug control at 0xf1149000 must read zero before each transfer and at
 * completion; a nonzero value fails with -ESTALE and is left unchanged.
 */
int monaco_pmic_shutdown_transaction_run(struct monaco_pmic_transaction *transaction,
					 struct monaco_cold_boot *boot,
					 bool (*eligible)(void *context,
							  const struct monaco_cold_boot *boot),
					 int (*guard)(void *context), void *context);

#endif
